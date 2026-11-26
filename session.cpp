#include "session.h"

const char *toString(SampleFormat f) {
	switch (f) {
	case SampleFormat::Int16: return "Int16";
	case SampleFormat::Int32: return "Int32";
	case SampleFormat::Float32: return "Float";
	}
	return "?";
}

bool parseSampleFormat(const std::string &s, SampleFormat &out) {
	for (auto f : {SampleFormat::Int16, SampleFormat::Int32, SampleFormat::Float32})
		if (s == toString(f)) return out = f, true;
	return false;
}

static lsl::channel_format_t toLSL(SampleFormat f) {
	switch (f) {
	case SampleFormat::Int16: return lsl::cf_int16;
	case SampleFormat::Int32: return lsl::cf_int32;
	case SampleFormat::Float32: return lsl::cf_float32;
	}
	return lsl::cf_undefined;
}

lsl::stream_info audioStreamInfo(const std::string &name, const AudioFormat &fmt, const DeviceInfo &dev, const char *api,
	double offset_us) {
	// source_id: the unique device id, so identical devices don't collide (issue #10).
	lsl::stream_info info(name, "Audio", fmt.channels, fmt.rate, toLSL(fmt.format), dev.id);
	info.desc().append_child("provider").append_child_value("api", api);
	info.desc().append_child_value("device", dev.description);
	info.desc().append_child_value("timestamp_offset_us", std::to_string(offset_us));
	return info;
}

LslPusher::LslPusher(const lsl::stream_info &info, double offset_s)
	: out(info), channels(info.channel_count()), srate(info.nominal_srate()), cf(info.channel_format()),
	  offset(offset_s) {}

void LslPusher::push(const void *data, const int64_t frames, const double t_first) {
	if (frames <= 0) return;
	const auto n = static_cast<std::size_t>(frames) * channels;
	// liblsl's chunk timestamp refers to the last sample; it back-fills earlier ones at srate.
	double ts = 0.0;  // 0: liblsl stamps the last sample with local_clock() at push
	if (t_first > 0.0) ts = t_first + static_cast<double>(frames - 1) / srate - offset;
	else if (offset != 0.0) ts = lsl::local_clock() - offset;
	switch (cf) {
	case lsl::cf_int16: out.push_chunk_multiplexed(static_cast<const int16_t *>(data), n, ts); break;
	case lsl::cf_int32: out.push_chunk_multiplexed(static_cast<const int32_t *>(data), n, ts); break;
	case lsl::cf_float32: out.push_chunk_multiplexed(static_cast<const float *>(data), n, ts); break;
	default: return;
	}
	written += frames;
}

CaptureSession::CaptureSession(const lsl::stream_info &info, double offset_s, Factory factory, Log log)
	: pusher(info, offset_s), factory(std::move(factory)), log(std::move(log)) {}

bool CaptureSession::start(std::string &err) {
	lastCheck = std::chrono::steady_clock::now();
	return startBackend(err);
}

void CaptureSession::poll() {
	if (!backend) return;
	const auto now = std::chrono::steady_clock::now();
	if (now - lastCheck < std::chrono::seconds(2)) return;  // also rate-limits restarts of a failing backend
	lastCheck = now;
	if (const auto n = pusher.samples_written(); !failed && n != lastCount) {
		lastCount = n;
		return;
	}
	log(failed ? "Restarting audio capture after error." : "No audio for 2 s; restarting audio capture.");
	if (std::string err; !startBackend(err)) log(err + "; retrying in 2 s.");
}

bool CaptureSession::startBackend(std::string &err) {
	backend.reset();  // stop the old backend before opening the device again
	failed = false;
	lastCount = pusher.samples_written();
	backend = factory();
	// onError may fire inside the backend's own callbacks, so only flag it; poll() restarts.
	return backend->start([this](const void *d, int64_t f, double t) { pusher.push(d, f, t); },
		[this](const std::string &msg) {
			failed = true;
			log(msg);
		},
		err);
}
