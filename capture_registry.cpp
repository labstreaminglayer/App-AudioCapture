// Which capture APIs this target has is decided by CMake (AC_HAVE_* definitions).
#include "capture.h"

#ifdef AC_HAVE_PIPEWIRE
extern const CaptureApi kPipeWireApi;
#endif
#ifdef AC_HAVE_COREAUDIO
extern const CaptureApi kCoreAudioApi;
#endif
#ifdef AC_HAVE_WASAPI
extern const CaptureApi kWasapiApi;
#endif
#ifdef AC_HAVE_QT
extern const CaptureApi kQtApi;
#endif
#ifdef AC_HAVE_ALSA
extern const CaptureApi kAlsaApi;
#endif

const std::vector<CaptureApi> &captureApis() {
	static const std::vector<CaptureApi> apis{
#ifdef AC_HAVE_PIPEWIRE
		kPipeWireApi,
#endif
#ifdef AC_HAVE_COREAUDIO
		kCoreAudioApi,
#endif
#ifdef AC_HAVE_WASAPI
		kWasapiApi,
#endif
#ifdef AC_HAVE_QT
		kQtApi,
#endif
#ifdef AC_HAVE_ALSA
		kAlsaApi,
#endif
	};
	return apis;
}

const CaptureApi *findCaptureApi(const std::string &name) {
	for (const auto &a : captureApis())
		if (name == a.name) return &a;
	return nullptr;
}

const CaptureApi *defaultCaptureApi() {
	for (const auto &a : captureApis())
		if (a.autoSelect && !a.list().empty()) return &a;
	return nullptr;
}
