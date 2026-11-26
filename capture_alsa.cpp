// ALSA direct backend (Linux, hw: devices only, no conversion). It opens the device exclusively, so it fails while
// a sound server (PipeWire, PulseAudio) holds it, and is never auto-selected. Each chunk is stamped from the kernel's
// status snapshot taken right after the read: htstamp is the CLOCK_MONOTONIC time (= liblsl's local_clock() on
// Linux) at which the driver read the DMA pointer, and delay is the frames captured but not yet read at that moment
// plus any FIFO delay the driver reports. No filtering; a constant (codec/ADC filter) still needs calibration.
#include "capture.h"
#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

snd_pcm_format_t toAlsa(SampleFormat f) {
	switch (f) {
	case SampleFormat::Int16: return SND_PCM_FORMAT_S16;
	case SampleFormat::Int32: return SND_PCM_FORMAT_S32;
	case SampleFormat::Float32: return SND_PCM_FORMAT_FLOAT;
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

class AlsaCapture final : public CaptureBackend {
public:
	AlsaCapture(std::string dev, const AudioFormat &fmt) : dev(std::move(dev)), fmt(fmt) {}
	~AlsaCapture() override { stop(); }

	bool start(Sink s, ErrorHandler e, std::string &err) override {
		if (const int rc = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE, 0); rc < 0) {
			pcm = nullptr;
			err = "snd_pcm_open(" + dev + "): " + snd_strerror(rc);
			if (rc == -EBUSY) err += " (held by a sound server such as PipeWire or PulseAudio?)";
			return false;
		}
		if (err = configure(); !err.empty() || (err = check(snd_pcm_start(pcm), "start")).size()) {
			stop();
			return false;
		}
		sink = std::move(s);
		onError = std::move(e);
		quit = false;
		th = std::thread([this] { run(); });
		return true;
	}

	void stop() override {
		quit = true;
		if (th.joinable()) th.join();
		if (pcm) {
			snd_pcm_drop(pcm);
			snd_pcm_close(pcm);
			pcm = nullptr;
		}
	}

private:
	static std::string check(int rc, const char *what) {
		return rc < 0 ? std::string("ALSA ") + what + ": " + snd_strerror(rc) : std::string();
	}

	std::string configure() {
		snd_pcm_hw_params_t *hw;
		snd_pcm_hw_params_alloca(&hw);
		snd_pcm_hw_params_any(pcm, hw);
		std::string e;
		if ((e = check(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "interleaved access")).size() ||
			(e = check(snd_pcm_hw_params_set_format(pcm, hw, toAlsa(fmt.format)), toString(fmt.format))).size() ||
			(e = check(snd_pcm_hw_params_set_channels(pcm, hw, fmt.channels), "channel count")).size() ||
			(e = check(snd_pcm_hw_params_set_rate(pcm, hw, fmt.rate, 0), "sample rate")).size())
			return e + " not supported by " + dev;
		snd_pcm_uframes_t period = 256, buffer = 256 * 8;  // ~5 ms chunks at 48 kHz
		int dir = 0;
		snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
		snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
		if ((e = check(snd_pcm_hw_params(pcm, hw), "hw params")).size()) return e;
		snd_pcm_hw_params_get_period_size(hw, &periodFrames, &dir);
		snd_pcm_sw_params_t *sw;
		snd_pcm_sw_params_alloca(&sw);
		snd_pcm_sw_params_current(pcm, sw);
		snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
		snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
		snd_pcm_sw_params_set_avail_min(pcm, sw, periodFrames);
		return check(snd_pcm_sw_params(pcm, sw), "sw params");
	}

	void run() {
		std::vector<uint8_t> buf(periodFrames * fmt.bytesPerFrame());
		snd_pcm_status_t *st;
		snd_pcm_status_malloc(&st);
		while (!quit) {
			int rc = snd_pcm_wait(pcm, 100);  // bounded, so stop() never waits on a stalled device
			if (rc == 0) continue;
			snd_pcm_sframes_t n = rc < 0 ? rc : snd_pcm_readi(pcm, buf.data(), periodFrames);
			if (n == -EAGAIN) continue;
			if (n < 0) {  // overrun or suspend: frames are lost, but stamps stay right since each comes from status
				rc = snd_pcm_recover(pcm, static_cast<int>(n), 1);
				if (rc >= 0 && snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) rc = snd_pcm_start(pcm);  // not after a resume
				if (rc < 0) {
					if (onError) onError(check(rc, "capture"));
					break;
				}
				continue;
			}
			double t_first = 0.0;
			if (snd_pcm_status(pcm, st) == 0) {
				snd_htimestamp_t ts;
				snd_pcm_status_get_htstamp(st, &ts);
				// At ts, `delay` newer frames were captured after the ones just read.
				const auto delay = snd_pcm_status_get_delay(st);
				if (ts.tv_sec || ts.tv_nsec)
					t_first = ts.tv_sec + ts.tv_nsec * 1e-9 - static_cast<double>(delay + n) / fmt.rate;
			}
			sink(buf.data(), n, t_first);
		}
		snd_pcm_status_free(st);
	}

	std::string dev;
	AudioFormat fmt;
	Sink sink;
	ErrorHandler onError;
	snd_pcm_t *pcm = nullptr;
	snd_pcm_uframes_t periodFrames = 256;
	std::thread th;
	std::atomic<bool> quit{false};
};

std::vector<DeviceInfo> listDevices() {  // like `arecord -l`: every card/device with a capture stream
	std::vector<DeviceInfo> out;
	snd_ctl_card_info_t *ci;
	snd_pcm_info_t *pi;
	snd_ctl_card_info_alloca(&ci);
	snd_pcm_info_alloca(&pi);
	for (int card = -1; snd_card_next(&card) == 0 && card >= 0;) {
		char hw[32];
		std::snprintf(hw, sizeof hw, "hw:%d", card);
		snd_ctl_t *ctl;
		if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
		if (snd_ctl_card_info(ctl, ci) == 0)
			for (int dev = -1; snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0;) {
				snd_pcm_info_set_device(pi, dev);
				snd_pcm_info_set_subdevice(pi, 0);
				snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
				if (snd_ctl_pcm_info(ctl, pi) < 0) continue;
				out.push_back({std::string("hw:CARD=") + snd_ctl_card_info_get_id(ci) + ",DEV=" + std::to_string(dev),
					std::string(snd_ctl_card_info_get_name(ci)) + ", " + snd_pcm_info_get_name(pi),
					{2, 48000, SampleFormat::Int16}});
			}
		snd_ctl_close(ctl);
	}
	return out;
}

std::unique_ptr<CaptureBackend> make(const std::string &deviceId, const AudioFormat &fmt) {
	return std::make_unique<AlsaCapture>(deviceId, fmt);
}

} // namespace

extern const CaptureApi kAlsaApi{"ALSA", listDevices, make, false};
