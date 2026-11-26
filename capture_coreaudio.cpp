// CoreAudio backend (macOS): AUHAL input unit. Each buffer is stamped from the input callback's AudioTimeStamp:
// mHostTime is the mach_absolute_time of the buffer's first frame on the device timeline; the device and stream input
// latencies (codec/ADC side, as reported by the driver) are subtracted. mach_absolute_time stops during sleep but
// steady_clock (= liblsl's local_clock()) does not, so host time is mapped through their current offset.
// Any constant left over needs a per-device calibration.
#include "capture.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

template <typename T>
bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T &out) {
	AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
	UInt32 size = sizeof(T);
	return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &out) == noErr;
}

std::string cfToString(CFStringRef s) {
	if (!s) return {};
	char buf[512];
	const bool ok = CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
	CFRelease(s);
	return ok ? buf : std::string();
}

std::vector<AudioObjectID> allDevices() {
	AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
	std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
	return ids;
}

int inputChannels(AudioObjectID dev) {
	AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || !size) return 0;
	std::vector<uint8_t> buf(size);
	auto *abl = reinterpret_cast<AudioBufferList *>(buf.data());
	if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, abl) != noErr) return 0;
	int n = 0;
	for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) n += static_cast<int>(abl->mBuffers[i].mNumberChannels);
	return n;
}

AudioObjectID deviceByUid(const std::string &uid) {
	for (auto id : allDevices()) {
		CFStringRef s = nullptr;
		if (getProp(id, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, s) && cfToString(s) == uid) return id;
	}
	return kAudioObjectUnknown;
}

class CaCapture final : public CaptureBackend {
public:
	CaCapture(std::string uid, const AudioFormat &fmt) : uid(std::move(uid)), fmt(fmt) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		ticksToSec = static_cast<double>(tb.numer) / tb.denom * 1e-9;
	}
	~CaCapture() override { stop(); }

	bool start(Sink s, ErrorHandler, std::string &err) override {
		const AudioObjectID dev = deviceByUid(uid);
		if (dev == kAudioObjectUnknown) return err = "CoreAudio device not found: " + uid, false;
		// Input latency on the device timeline: device + first input stream (codec/ADC side).
		UInt32 devLat = 0, streamLat = 0, safety = 0, bufFrames = 0;
		Float64 devRate = fmt.rate;
		getProp(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, devLat);
		getProp(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, safety);
		getProp(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, bufFrames);
		getProp(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, devRate);
		AudioStreamID streams[8];
		AudioObjectPropertyAddress sa{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
		UInt32 size = sizeof streams;
		if (AudioObjectGetPropertyData(dev, &sa, 0, nullptr, &size, streams) == noErr && size >= sizeof(AudioStreamID))
			getProp(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, streamLat);
		latency = (devLat + streamLat) / devRate;
		std::fprintf(stderr, "CoreAudio input: device rate %.0f Hz, latency device %u + stream %u frames, safety offset %u, buffer %u frames\n",
			devRate, devLat, streamLat, safety, bufFrames);

		AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
		AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
		if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return err = "Cannot create the AUHAL unit", false;
		const UInt32 one = 1, zero = 0;
		AudioStreamBasicDescription asbd{};
		asbd.mSampleRate = fmt.rate;
		asbd.mFormatID = kAudioFormatLinearPCM;
		asbd.mFormatFlags = (fmt.format == SampleFormat::Float32 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
							kAudioFormatFlagIsPacked;
		asbd.mBytesPerPacket = asbd.mBytesPerFrame = static_cast<UInt32>(fmt.bytesPerFrame());
		asbd.mFramesPerPacket = 1;
		asbd.mChannelsPerFrame = static_cast<UInt32>(fmt.channels);
		asbd.mBitsPerChannel = fmt.format == SampleFormat::Int16 ? 16 : 32;
		const AURenderCallbackStruct cb{&onInput, this};
		OSStatus rc;
		const char *step = nullptr;
		if ((rc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one))) step = "enable input";
		else if ((rc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof zero))) step = "disable output";
		else if ((rc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev))) step = "select device";
		else if ((rc = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &asbd, sizeof asbd))) step = "set format";
		else if ((rc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb))) step = "set callback";
		else if ((rc = AudioUnitInitialize(unit))) step = "initialize";
		if (step) {
			stop();
			return err = std::string("CoreAudio ") + step + " failed (OSStatus " + std::to_string(rc) + ")", false;
		}
		buf.resize(static_cast<size_t>(8192) * fmt.bytesPerFrame());  // allocate here, never in the callback
		sink = std::move(s);
		if ((rc = AudioOutputUnitStart(unit))) {
			stop();
			return err = "CoreAudio start failed (OSStatus " + std::to_string(rc) + ")", false;
		}
		return true;
	}

	void stop() override {
		if (!unit) return;
		AudioOutputUnitStop(unit);
		AudioUnitUninitialize(unit);
		AudioComponentInstanceDispose(unit);
		unit = nullptr;
	}

private:
	static OSStatus onInput(void *self, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
		UInt32 frames, AudioBufferList *) {
		auto *c = static_cast<CaCapture *>(self);
		const UInt32 bytes = frames * static_cast<UInt32>(c->fmt.bytesPerFrame());
		if (bytes > c->buf.size()) return kAudio_ParamError;
		AudioBufferList abl;
		abl.mNumberBuffers = 1;
		abl.mBuffers[0] = {static_cast<UInt32>(c->fmt.channels), bytes, c->buf.data()};
		if (const OSStatus rc = AudioUnitRender(c->unit, flags, ts, bus, frames, &abl)) return rc;
		double t = 0.0;
		if (ts->mFlags & kAudioTimeStampHostTimeValid) {
			const double steady = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			const double offset = steady - static_cast<double>(mach_absolute_time()) * c->ticksToSec;  // = time slept
			t = static_cast<double>(ts->mHostTime) * c->ticksToSec + offset - c->latency;
		}
		c->sink(c->buf.data(), frames, t);
		return noErr;
	}

	std::string uid;
	AudioFormat fmt;
	Sink sink;
	AudioComponentInstance unit = nullptr;
	std::vector<uint8_t> buf;
	double ticksToSec = 0, latency = 0;
};

std::vector<DeviceInfo> listDevices() {
	std::vector<DeviceInfo> out;
	for (auto id : allDevices()) {
		const int ch = inputChannels(id);
		if (ch <= 0) continue;
		CFStringRef uid = nullptr, name = nullptr;
		Float64 rate = 48000;
		getProp(id, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, uid);
		getProp(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name);
		getProp(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
		out.push_back({cfToString(uid), cfToString(name), {ch, static_cast<int>(rate), SampleFormat::Float32}});
	}
	return out;
}

std::unique_ptr<CaptureBackend> make(const std::string &deviceId, const AudioFormat &fmt) {
	return std::make_unique<CaCapture>(deviceId, fmt);
}

} // namespace

extern const CaptureApi kCoreAudioApi{"CoreAudio", listDevices, make, true};
