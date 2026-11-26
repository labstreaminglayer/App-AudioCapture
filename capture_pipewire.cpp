// PipeWire backend (Linux). Each buffer is stamped from the graph clock instead of its arrival time:
// pw_time.now is the CLOCK_MONOTONIC time (= liblsl's local_clock() on Linux) of the graph cycle, which the
// driver (e.g. PipeWire's ALSA source) filters against the hardware pointer; pw_time.delay says how long ago
// the newest data left the capture device. Any constant left over needs a per-device calibration: the codec/ADC
// filter, and ALSA's runtime delay, which PipeWire does not see (32 frames = 0.67 ms of HDA FIFO on an ALC897,
// /proc/asound/cardN/pcmMc/sub0/status delay - avail).
#include "capture.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <cstring>
#include <mutex>

namespace {

void ensureInit() {
	static std::once_flag once;
	std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

spa_audio_format toSpa(SampleFormat f) {
	switch (f) {
	case SampleFormat::Int16: return SPA_AUDIO_FORMAT_S16;
	case SampleFormat::Int32: return SPA_AUDIO_FORMAT_S32;
	case SampleFormat::Float32: return SPA_AUDIO_FORMAT_F32;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

class PwCapture final : public CaptureBackend {
public:
	PwCapture(std::string target, const AudioFormat &fmt) : target(std::move(target)), fmt(fmt) {}
	~PwCapture() override { stop(); }

	bool start(Sink s, ErrorHandler e, std::string &err) override {
		ensureInit();
		sink = std::move(s);
		onError = std::move(e);
		loop = pw_thread_loop_new("audiocapture-pw", nullptr);
		if (!loop) return err = "pw_thread_loop_new failed", false;
		auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, "AudioCapture", nullptr);
#ifdef PW_KEY_TARGET_OBJECT
		if (!target.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
#else
		if (!target.empty()) pw_properties_set(props, PW_KEY_NODE_TARGET, target.c_str());
#endif
		stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "AudioCapture", props, &kEvents, this);
		if (!stream) {
			stop();
			return err = std::string("Cannot connect to PipeWire: ") + strerror(errno), false;
		}
		spa_audio_info_raw info{};
		info.format = toSpa(fmt.format);
		info.rate = static_cast<uint32_t>(fmt.rate);
		info.channels = static_cast<uint32_t>(fmt.channels);
		if (fmt.channels == 1) info.position[0] = SPA_AUDIO_CHANNEL_MONO;
		else if (fmt.channels == 2) info.position[0] = SPA_AUDIO_CHANNEL_FL, info.position[1] = SPA_AUDIO_CHANNEL_FR;
		else info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
		uint8_t buf[1024];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
		// RT_PROCESS: process() runs in the graph cycle itself, so pw_time matches the dequeued buffer.
		const auto flags = static_cast<pw_stream_flags>(
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
		if (const int rc = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1); rc < 0) {
			stop();
			return err = std::string("pw_stream_connect: ") + spa_strerror(rc), false;
		}
		if (pw_thread_loop_start(loop) < 0) {
			stop();
			return err = "pw_thread_loop_start failed", false;
		}
		return true;
	}

	void stop() override {
		if (loop) pw_thread_loop_stop(loop);
		if (stream) pw_stream_destroy(stream);  // also tears down its context and core connection
		if (loop) pw_thread_loop_destroy(loop);
		stream = nullptr;
		loop = nullptr;
	}

private:
	void process() {
		pw_buffer *bufs[8];
		int nb = 0;
		while (nb < 8 && (bufs[nb] = pw_stream_dequeue_buffer(stream))) ++nb;
		if (!nb) return;
		pw_time t{};
#if PW_CHECK_VERSION(0, 3, 50)
		pw_stream_get_time_n(stream, &t, sizeof t);
#else
		pw_stream_get_time(stream, &t);
#endif
		const int bpf = fmt.bytesPerFrame();
		int64_t frames[8];
		for (int i = 0; i < nb; ++i) {
			const spa_data &d = bufs[i]->buffer->datas[0];
			frames[i] = d.data && d.chunk ? std::min(d.chunk->size, d.maxsize) / bpf : 0;
		}
		// For capture, delay = quantum + ALSA headroom (1056 at quantum 1024, 288 at 256); the cycle read the oldest
		// frames, so now - delay is the capture time of the newest buffer's FIRST frame. Not the driver's measured
		// fill (spa_io_position clock.delay): `now` is the DLL-smoothed cycle time, and subtracting the raw fill from
		// it gave +-2 sample stamp errors (measured 2026-11-26). Older buffers (rare) are contiguous before it.
		double ts[8] = {};
		if (t.now > 0 && t.rate.denom > 0) {
			double lat = static_cast<double>(t.delay) * t.rate.num / t.rate.denom;
#if PW_CHECK_VERSION(0, 3, 50)
			lat += static_cast<double>(t.buffered) / fmt.rate;  // frames held in this stream's resampler
#endif
			ts[nb - 1] = static_cast<double>(t.now) * 1e-9 - lat;
			for (int i = nb - 2; i >= 0; --i) ts[i] = ts[i + 1] - static_cast<double>(frames[i]) / fmt.rate;
		}  // else 0.0: unknown, liblsl stamps at push
		for (int i = 0; i < nb; ++i) {
			const spa_data &d = bufs[i]->buffer->datas[0];
			if (frames[i] > 0) {
				const uint32_t off = std::min(d.chunk->offset, d.maxsize);
				sink(static_cast<const uint8_t *>(d.data) + off, frames[i], ts[i]);
			}
			pw_stream_queue_buffer(stream, bufs[i]);
		}
	}

	static void onProcess(void *self) { static_cast<PwCapture *>(self)->process(); }
	static void onState(void *self, pw_stream_state, pw_stream_state state, const char *error) {
		auto *c = static_cast<PwCapture *>(self);
		if (state == PW_STREAM_STATE_ERROR && c->onError)
			c->onError(std::string("PipeWire stream error: ") + (error ? error : "unknown"));
	}
	static inline const pw_stream_events kEvents = [] {
		pw_stream_events ev{};
		ev.version = PW_VERSION_STREAM_EVENTS;
		ev.state_changed = onState;
		ev.process = onProcess;
		return ev;
	}();

	std::string target;
	AudioFormat fmt;
	Sink sink;
	ErrorHandler onError;
	pw_thread_loop *loop = nullptr;
	pw_stream *stream = nullptr;
};

std::vector<DeviceInfo> listDevices() {
	ensureInit();
	std::vector<DeviceInfo> out;
	pw_main_loop *loop = pw_main_loop_new(nullptr);
	pw_context *ctx = loop ? pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0) : nullptr;
	pw_core *core = ctx ? pw_context_connect(ctx, nullptr, 0) : nullptr;
	if (core) {
		struct State {
			std::vector<DeviceInfo> &out;
			pw_main_loop *loop;
			int seq = 0;
		} st{out, loop};
		pw_registry_events regEv{};
		regEv.version = PW_VERSION_REGISTRY_EVENTS;
		regEv.global = [](void *data, uint32_t, uint32_t, const char *type, uint32_t, const spa_dict *props) {
			if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
			const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
			const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
			if (!cls || !name || std::strncmp(cls, "Audio/Source", 12) != 0) return;
			const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
			// PipeWire converts to any format; F32 is its native sample type.
			static_cast<State *>(data)->out.push_back({name, desc ? desc : name, {2, 48000, SampleFormat::Float32}});
		};
		pw_core_events coreEv{};
		coreEv.version = PW_VERSION_CORE_EVENTS;
		coreEv.done = [](void *data, uint32_t id, int seq) {
			auto *s = static_cast<State *>(data);
			if (id == PW_ID_CORE && seq == s->seq) pw_main_loop_quit(s->loop);
		};
		spa_hook regL{}, coreL{};
		pw_registry *reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(reg, &regL, &regEv, &st);
		pw_core_add_listener(core, &coreL, &coreEv, &st);
		st.seq = pw_core_sync(core, PW_ID_CORE, 0);  // 'done' arrives after all existing globals
		pw_main_loop_run(loop);
		spa_hook_remove(&regL);
		spa_hook_remove(&coreL);
		pw_proxy_destroy(reinterpret_cast<pw_proxy *>(reg));
		pw_core_disconnect(core);
	}
	if (ctx) pw_context_destroy(ctx);
	if (loop) pw_main_loop_destroy(loop);
	return out;
}

std::unique_ptr<CaptureBackend> make(const std::string &deviceId, const AudioFormat &fmt) {
	return std::make_unique<PwCapture>(deviceId, fmt);
}

} // namespace

extern const CaptureApi kPipeWireApi{"PipeWire", listDevices, make, true};
