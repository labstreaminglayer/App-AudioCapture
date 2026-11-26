#pragma once
// LSL side of AudioCapture (no Qt): the outlet pusher and the capture session shared by GUI and CLI.
#include "capture.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <lsl_cpp.h>

// Shared by GUI and CLI so both publish identical stream metadata.
// offset_us: fixed input latency (measured per device) that the pusher subtracts; recorded in the metadata.
lsl::stream_info audioStreamInfo(const std::string &name, const AudioFormat &fmt, const DeviceInfo &dev, const char *api,
	double offset_us = 0);

// Pushes interleaved audio frames from a CaptureBackend to an LSL outlet.
class LslPusher {
public:
	// offset_s is subtracted from every timestamp (the device's measured input latency).
	LslPusher(const lsl::stream_info &info, double offset_s);
	// t_first: local_clock() time of the first frame, or 0.0 if unknown (then the push time stands in for the last frame).
	void push(const void *data, int64_t frames, double t_first);
	[[nodiscard]] int64_t samples_written() const { return written; }

private:
	lsl::stream_outlet out;
	const int channels;
	const double srate;
	const lsl::channel_format_t cf;
	const double offset;
	std::atomic<int64_t> written{0};  // backends may push from their own audio thread
};

// Owns the LSL outlet and a capture backend. poll() (call it a few times a second) restarts the backend
// after an error, or when no audio arrived for 2 s, keeping the same outlet so consumers stay subscribed (#12).
class CaptureSession {
public:
	using Factory = std::function<std::unique_ptr<CaptureBackend>()>;
	using Log = std::function<void(const std::string &msg)>;
	CaptureSession(const lsl::stream_info &info, double offset_s, Factory factory, Log log);
	// Starts capture; returns false and sets err if the first start fails.
	bool start(std::string &err);
	void poll();
	[[nodiscard]] int64_t samples_written() const { return pusher.samples_written(); }

private:
	bool startBackend(std::string &err);
	LslPusher pusher;  // declared first so it outlives the backend feeding it
	Factory factory;
	Log log;
	std::unique_ptr<CaptureBackend> backend;
	std::chrono::steady_clock::time_point lastCheck;
	int64_t lastCount = 0;
	std::atomic<bool> failed{false};  // onError may come from a backend thread
};
