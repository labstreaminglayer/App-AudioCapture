// Headless AudioCapture: same capture path and stream metadata as the GUI, configured from a GUI .cfg file
// and/or flags. No Qt, except the event loop that the QtMultimedia backend needs (AUDIOCAPTURE_CLI_QT).
#include "session.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#ifdef AUDIOCAPTURE_CLI_QT
#include <QCoreApplication>
#include <QTimer>
#endif

static std::atomic<bool> g_shutdown{false};

static void usage(const char *prog) {
	std::cout << "Usage: " << prog << " [options]\n"
			  << "Capture audio and stream it over LSL, without a GUI.\n\n"
			  << "  -h, --help            Show this help\n"
			  << "  --list                List capture devices of every capture API and exit\n"
			  << "  -a, --api API         Capture API (see --list). Default: the first one that has devices\n"
			  << "  -c, --config FILE     Load settings from a GUI .cfg file; other options override it\n"
			  << "  -d, --device DEV      Device index (see --list) or id. Default: 0\n"
			  << "  -n, --name NAME       LSL stream name. Default: MyAudioStream\n"
			  << "  -r, --rate HZ         Sample rate. Default: device preferred\n"
			  << "  --channels N          Channel count. Default: device preferred\n"
			  << "  -f, --format FMT      Int16, Int32 or Float. Default: device preferred\n"
			  << "  --ts-offset-us US     Subtract US microseconds from every timestamp: the device's measured\n"
			  << "                        input latency (default: 0)\n";
}

// The [AudioCapture] section of a GUI-saved (QSettings INI) config: key=value lines.
static bool readConfig(const std::string &path, std::map<std::string, std::string> &kv) {
	std::ifstream in(path);
	if (!in) return false;
	std::string line, section;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[') section = line.substr(1, line.find(']') - 1);
		else if (const auto eq = line.find('='); section == "AudioCapture" && eq != std::string::npos) {
			auto v = line.substr(eq + 1);
			if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
			kv[line.substr(0, eq)] = v;
		}
	}
	return true;
}

static bool toInt(const std::string &s, int &out) {
	char *end = nullptr;
	const long v = std::strtol(s.c_str(), &end, 10);
	if (s.empty() || *end) return false;
	out = static_cast<int>(v);
	return true;
}

int main(int argc, char *argv[]) {
#ifdef AUDIOCAPTURE_CLI_QT
	QCoreApplication app(argc, argv);
#endif
	std::map<std::string, std::string> opt;  // flag values, keyed like the config file
	bool list = false;
	std::string cfgPath;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto val = [&](const char *key) {
			if (i + 1 >= argc) return false;
			opt[key] = argv[++i];
			return true;
		};
		bool ok = true;
		if (a == "-h" || a == "--help") return usage(argv[0]), 0;
		else if (a == "--list") list = true;
		else if (a == "-c" || a == "--config") ok = i + 1 < argc && !(cfgPath = argv[++i]).empty();
		else if (a == "-a" || a == "--api") ok = val("api");
		else if (a == "-d" || a == "--device") ok = val("device");
		else if (a == "-n" || a == "--name") ok = val("name");
		else if (a == "-r" || a == "--rate") ok = val("samplerate");
		else if (a == "--channels") ok = val("channels");
		else if (a == "-f" || a == "--format") ok = val("samplesize");
		else if (a == "--ts-offset-us") ok = val("timestamp_offset_us");
		else ok = false;
		if (!ok) {
			std::cerr << "Bad or incomplete option: " << a << "\n\n";
			usage(argv[0]);
			return 1;
		}
	}

	if (list) {
		const auto *def = defaultCaptureApi();
		for (const auto &api : captureApis()) {
			const auto devs = api.list();
			std::cout << api.name << (&api == def ? " (default)" : "") << (api.autoSelect ? "" : " (exclusive; only with --api)")
					  << ": " << devs.size() << " device(s)\n";
			for (size_t i = 0; i < devs.size(); ++i)
				std::cout << "  " << i << ": " << devs[i].description << "  [" << devs[i].id << "] (preferred "
						  << devs[i].preferred.channels << " ch @ " << devs[i].preferred.rate << " Hz)\n";
		}
		return 0;
	}

	std::map<std::string, std::string> cfg;
	if (!cfgPath.empty() && !readConfig(cfgPath, cfg)) {
		std::cerr << "Cannot read config file: " << cfgPath << '\n';
		return 1;
	}
	for (const auto &[k, v] : opt) cfg[k] = v;  // flags override the config file
	auto get = [&](const std::string &k, const std::string &def) { return cfg.count(k) ? cfg[k] : def; };

	const CaptureApi *api = cfg.count("api") ? findCaptureApi(cfg["api"]) : defaultCaptureApi();
	if (!api) {
		std::cerr << (cfg.count("api") ? "Unknown capture API '" + cfg["api"] + "'" : "No capture devices found")
				  << " (see --list).\n";
		return 1;
	}
	const auto devices = api->list();

	const auto devArg = get("device", "0");
	int devIdx = -1;
	if (!toInt(devArg, devIdx))
		for (size_t i = 0; i < devices.size(); ++i)
			if (devices[i].id == devArg) devIdx = static_cast<int>(i);
	if (devIdx < 0 || devIdx >= static_cast<int>(devices.size())) {
		std::cerr << "Unknown device '" << devArg << "'; " << devices.size() << " capture device(s) found (see --list).\n";
		return 1;
	}
	const auto &dev = devices[devIdx];
	AudioFormat fmt = dev.preferred;
	if (cfg.count("samplerate") && (!toInt(cfg["samplerate"], fmt.rate) || fmt.rate <= 0)) {
		std::cerr << "Invalid sample rate: " << cfg["samplerate"] << '\n';
		return 1;
	}
	if (cfg.count("channels") && (!toInt(cfg["channels"], fmt.channels) || fmt.channels <= 0)) {
		std::cerr << "Invalid channel count: " << cfg["channels"] << '\n';
		return 1;
	}
	if (cfg.count("samplesize") && !parseSampleFormat(cfg["samplesize"], fmt.format)) {
		std::cerr << "Invalid sample format '" << cfg["samplesize"] << "' (Int16, Int32 or Float).\n";
		return 1;
	}
	const auto name = get("name", "MyAudioStream");
	char *end = nullptr;
	const auto offStr = get("timestamp_offset_us", "0");
	const double offset_us = std::strtod(offStr.c_str(), &end);
	if (offStr.empty() || *end) {
		std::cerr << "Invalid timestamp offset: " << offStr << '\n';
		return 1;
	}

	CaptureSession session(audioStreamInfo(name, fmt, dev, api->name, offset_us), offset_us * 1e-6,
		[&] { return api->make(dev.id, fmt); },
		[](const std::string &msg) { std::cerr << msg << std::endl; });

	// First signal: clean shutdown. Second: exit now, in case the main loop is wedged.
	const auto onSignal = [](int) {
		if (g_shutdown.exchange(true)) std::_Exit(130);
	};
	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	if (std::string err; !session.start(err)) {
		std::cerr << err << '\n';
		return 1;
	}
	std::cout << "Streaming '" << name << "' from '" << dev.description << "' via " << api->name << ": "
			  << fmt.channels << " ch @ " << fmt.rate << " Hz, " << toString(fmt.format) << ". Ctrl+C to stop."
			  << std::endl;
#ifdef AUDIOCAPTURE_CLI_QT
	QTimer poll;
	QObject::connect(&poll, &QTimer::timeout, [&] {
		session.poll();
		if (g_shutdown) app.quit();
	});
	poll.start(100);
	app.exec();
#else
	while (!g_shutdown) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		session.poll();
	}
#endif
	std::cout << "Pushed " << session.samples_written() << " samples ("
			  << static_cast<double>(session.samples_written()) / fmt.rate << " s)." << std::endl;
	return 0;
}
