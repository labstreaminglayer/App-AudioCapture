// WASAPI backend (Windows): shared-mode, event-driven capture. Each packet is stamped from IAudioCaptureClient::GetBuffer's
// QPC position: the performance-counter time at which the device recorded the packet's first frame. It is mapped to
// steady_clock (= liblsl's local_clock()) through their current offset, as the clocks need not share an epoch.
// Any constant left over (ADC filter, driver FIFO) needs a per-device calibration.
#include "capture.h"
#include <windows.h>
#include <initguid.h>  // defines the GUIDs below in this file (no uuid import library needed)
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>  // after mmdeviceapi.h, which brings DEFINE_PROPERTYKEY
#include <ksmedia.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

namespace {

template <typename T> struct Com {  // minimal COM pointer
	T *p = nullptr;
	~Com() { if (p) p->Release(); }
	T *operator->() const { return p; }
	T **operator&() { return &p; }
};

struct ComInit {
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

std::string utf8(const wchar_t *w) {
	if (!w) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring wide(const std::string &s) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

std::string hrText(const char *what, HRESULT hr) {
	char buf[96];
	snprintf(buf, sizeof buf, "WASAPI %s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
	return buf;
}

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT, spelled out: MinGW's ksmedia.h only declares them.
const GUID kSubtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

double qpcSeconds() {
	static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return static_cast<double>(c.QuadPart) / freq;
}

class WasapiCapture final : public CaptureBackend {
public:
	WasapiCapture(std::string id, const AudioFormat &fmt) : id(std::move(id)), fmt(fmt) {}
	~WasapiCapture() override { stop(); }

	bool start(Sink s, ErrorHandler e, std::string &err) override {
		sink = std::move(s);
		onError = std::move(e);
		running = true;
		std::promise<std::string> ready;
		auto started = ready.get_future();
		th = std::thread([this, &ready] { run(ready); });  // COM objects live and die on this thread
		err = started.get();
		if (err.empty()) return true;
		stop();
		return false;
	}

	void stop() override {
		running = false;
		if (th.joinable()) th.join();
	}

private:
	void run(std::promise<std::string> &ready) {
		ComInit com;
		Com<IMMDeviceEnumerator> en;
		Com<IMMDevice> dev;
		Com<IAudioClient> client;
		Com<IAudioCaptureClient> cap;
		HRESULT hr;
		const char *step = nullptr;
		if (FAILED(hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
					   reinterpret_cast<void **>(&en)))) step = "device enumerator";
		else if (FAILED(hr = en->GetDevice(wide(id).c_str(), &dev))) step = "open device";
		else if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client)))) step = "activate";
		if (step) return ready.set_value(hrText(step, hr));
		WAVEFORMATEX *mix = nullptr;
		if (SUCCEEDED(client->GetMixFormat(&mix))) {
			if (static_cast<int>(mix->nSamplesPerSec) != fmt.rate)  // measured: +0.72 ms for 44.1 -> 48 kHz
				fprintf(stderr, "WASAPI: the device runs at %lu Hz; Windows resamples to %d Hz, which adds latency the "
								"timestamps do not include (calibrate at this rate, or capture at %lu Hz)\n",
					static_cast<unsigned long>(mix->nSamplesPerSec), fmt.rate, static_cast<unsigned long>(mix->nSamplesPerSec));
			std::fflush(stderr);  // MinGW buffers stderr when redirected
			CoTaskMemFree(mix);
		}

		WAVEFORMATEXTENSIBLE wf{};
		wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wf.Format.nChannels = static_cast<WORD>(fmt.channels);
		wf.Format.nSamplesPerSec = static_cast<DWORD>(fmt.rate);
		wf.Format.wBitsPerSample = fmt.format == SampleFormat::Int16 ? 16 : 32;
		wf.Format.nBlockAlign = static_cast<WORD>(fmt.bytesPerFrame());
		wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
		wf.Format.cbSize = sizeof(wf) - sizeof(WAVEFORMATEX);
		wf.Samples.wValidBitsPerSample = wf.Format.wBitsPerSample;
		wf.dwChannelMask = fmt.channels == 1 ? KSAUDIO_SPEAKER_MONO : fmt.channels == 2 ? KSAUDIO_SPEAKER_STEREO : 0;
		wf.SubFormat = fmt.format == SampleFormat::Float32 ? kSubtypeFloat : kSubtypePcm;
		// AUTOCONVERTPCM: the engine converts from its mix format to ours (Windows 8+), so any rate/format works.
		const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, &wf.Format, nullptr))) step = "initialize";
		else if (FAILED(hr = client->SetEventHandle(ev))) step = "set event";
		else if (FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap)))) step = "capture client";
		else if (FAILED(hr = client->Start())) step = "start";
		if (step) {
			CloseHandle(ev);
			return ready.set_value(hrText(step, hr));
		}
		ready.set_value({});

		const int bpf = fmt.bytesPerFrame();
		std::vector<uint8_t> zeros;
		while (running) {
			if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;  // the watchdog notices a silent device
			UINT32 n = 0;
			while (running && SUCCEEDED(hr = cap->GetNextPacketSize(&n)) && n > 0) {
				BYTE *data = nullptr;
				DWORD bflags = 0;
				UINT64 qpc100ns = 0;
				if (FAILED(hr = cap->GetBuffer(&data, &n, &bflags, nullptr, &qpc100ns))) break;
				double t = 0.0;  // 0: unknown, liblsl stamps at push
				if (!(bflags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc100ns) {
					const double steady = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
					t = static_cast<double>(qpc100ns) * 1e-7 + (steady - qpcSeconds());
				}
				if (bflags & AUDCLNT_BUFFERFLAGS_SILENT) {
					zeros.assign(static_cast<size_t>(n) * bpf, 0);
					data = zeros.data();
				}
				sink(data, n, t);
				cap->ReleaseBuffer(n);
			}
			if (FAILED(hr)) {  // e.g. AUDCLNT_E_DEVICE_INVALIDATED (unplugged, format changed)
				if (onError) onError(hrText("capture", hr));
				break;
			}
		}
		client->Stop();
		CloseHandle(ev);
	}

	std::string id;
	AudioFormat fmt;
	Sink sink;
	ErrorHandler onError;
	std::atomic<bool> running{false};
	std::thread th;
};

std::vector<DeviceInfo> listDevices() {
	ComInit com;
	std::vector<DeviceInfo> out;
	Com<IMMDeviceEnumerator> en;
	Com<IMMDeviceCollection> all;
	UINT count = 0;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void **>(&en))) ||
		FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all)) || FAILED(all->GetCount(&count)))
		return out;
	for (UINT i = 0; i < count; ++i) {
		Com<IMMDevice> dev;
		Com<IPropertyStore> props;
		Com<IAudioClient> client;
		LPWSTR wid = nullptr;
		if (FAILED(all->Item(i, &dev)) || FAILED(dev->GetId(&wid))) continue;
		DeviceInfo d;
		d.id = utf8(wid);
		CoTaskMemFree(wid);
		PROPVARIANT name;
		PropVariantInit(&name);
		if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
			name.vt == VT_LPWSTR)
			d.description = utf8(name.pwszVal);
		PropVariantClear(&name);
		d.preferred = {2, 48000, SampleFormat::Float32};
		WAVEFORMATEX *mix = nullptr;
		if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) &&
			SUCCEEDED(client->GetMixFormat(&mix))) {
			d.preferred.channels = mix->nChannels;
			d.preferred.rate = static_cast<int>(mix->nSamplesPerSec);
			CoTaskMemFree(mix);
		}
		out.push_back(std::move(d));
	}
	return out;
}

std::unique_ptr<CaptureBackend> make(const std::string &deviceId, const AudioFormat &fmt) {
	return std::make_unique<WasapiCapture>(deviceId, fmt);
}

} // namespace

extern const CaptureApi kWasapiApi{"WASAPI", listDevices, make, true};
