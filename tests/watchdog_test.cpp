#undef NDEBUG  // asserts are the checks; keep them in Release builds
// Self-check for CaptureSession's watchdog: a fake backend that delivers 1 s of audio from its own thread,
// then stalls (or reports an error); the session must restart it and retry after a failed start.
#include "session.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

struct FakeBackend : CaptureBackend {
	bool failStart, emitError;
	std::thread th;
	std::atomic<bool> quit{false};
	FakeBackend(bool failStart, bool emitError) : failStart(failStart), emitError(emitError) {}
	~FakeBackend() override { stop(); }
	bool start(Sink sink, ErrorHandler onError, std::string &err) override {
		if (failStart) return err = "fake start failure", false;
		th = std::thread([this, sink, onError] {
			std::vector<int16_t> buf(480);
			for (int k = 0; k < 100 && !quit; ++k) {  // 10 ms chunks for 1 s, then stall
				sink(buf.data(), 480, 0.0);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (emitError && !quit) onError("fake error");
		});
		return true;
	}
	void stop() override {
		quit = true;
		if (th.joinable()) th.join();
	}
};

static void runFor(CaptureSession &s, double secs) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
	while (std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		s.poll();
	}
}

int main() {
	lsl::stream_info info("wdtest", "Audio", 1, 48000, lsl::cf_int16, "wdtest");
	for (bool emitError : {false, true}) {
		int made = 0;
		// 2nd backend fails to start, 3rd works: covers stall/error -> failed restart -> retry.
		CaptureSession s(info, 0.0, [&] { ++made; return std::make_unique<FakeBackend>(made == 2, emitError); },
			[](const std::string &m) { std::cout << "  log: " << m << '\n'; });
		std::string err;
		assert(s.start(err));
		runFor(s, 7.5);
		std::cout << (emitError ? "error" : "stall") << " case: backends made " << made << ", samples "
				  << s.samples_written() << '\n';
		assert(made >= 3);                                // restarted after stall/error, retried after failed start
		assert(s.samples_written() >= 2 * 48000 - 4800);  // 1st and 3rd backends each delivered ~1 s
	}
	CaptureSession bad(info, 0.0, [] { return std::make_unique<FakeBackend>(true, false); }, [](auto &) {});
	std::string err;
	assert(!bad.start(err) && err == "fake start failure");
	std::cout << "OK\n";
}
