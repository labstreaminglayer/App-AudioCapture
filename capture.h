#pragma once
// Audio capture backend interface (no Qt). Each target compiles in several capture APIs (see captureApis());
// the default is chosen at runtime: the first auto-selectable API that reports devices.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class SampleFormat { Int16, Int32, Float32 };
const char *toString(SampleFormat f);
bool parseSampleFormat(const std::string &s, SampleFormat &out);  // "Int16", "Int32", "Float"

struct AudioFormat {
	int channels = 2;
	int rate = 48000;
	SampleFormat format = SampleFormat::Int16;
	[[nodiscard]] int bytesPerFrame() const { return channels * (format == SampleFormat::Int16 ? 2 : 4); }
};

struct DeviceInfo {
	std::string id;           // stable backend id (PipeWire node.name, QAudioDevice::id())
	std::string description;  // human-readable name
	AudioFormat preferred;
};

class CaptureBackend {
public:
	// Interleaved frames in the requested format. t_first is the capture time of the first frame
	// in lsl::local_clock() seconds, or 0.0 if the backend has none (the outlet then stamps at push).
	using Sink = std::function<void(const void *data, int64_t frames, double t_first)>;
	using ErrorHandler = std::function<void(const std::string &msg)>;

	virtual ~CaptureBackend() = default;
	// Returns false and sets err if capture could not start; onError reports later failures.
	// Both callbacks may run on a backend-owned thread.
	virtual bool start(Sink sink, ErrorHandler onError, std::string &err) = 0;
	virtual void stop() = 0;
};

struct CaptureApi {
	const char *name;  // "PipeWire", "QtMultimedia", "ALSA"
	std::vector<DeviceInfo> (*list)();
	std::unique_ptr<CaptureBackend> (*make)(const std::string &deviceId, const AudioFormat &fmt);
	bool autoSelect;  // false: takes the device exclusively, so only used when asked for (ALSA hw)
};

const std::vector<CaptureApi> &captureApis();  // compiled-in APIs, in order of preference
const CaptureApi *findCaptureApi(const std::string &name);
const CaptureApi *defaultCaptureApi();  // first auto-selectable API with devices, or nullptr
