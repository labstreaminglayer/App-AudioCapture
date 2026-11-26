// QtMultimedia (QAudioSource) backend: no hardware timestamps, so liblsl stamps each chunk at push.
// Needs a QCoreApplication and a running Qt event loop on the thread that calls start().
#include "capture.h"
#include <QAudioDevice>
#include <QAudioSource>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>

namespace {

QAudioFormat::SampleFormat toQt(SampleFormat f) {
	switch (f) {
	case SampleFormat::Int16: return QAudioFormat::Int16;
	case SampleFormat::Int32: return QAudioFormat::Int32;
	case SampleFormat::Float32: return QAudioFormat::Float;
	}
	return QAudioFormat::Unknown;
}

// QAudioSource push mode writes captured bytes into a QIODevice; forward them to the sink.
class Forwarder : public QIODevice {
public:
	Forwarder(CaptureBackend::Sink sink, int frameBytes) : sink(std::move(sink)), frameBytes(frameBytes) {}
	qint64 writeData(const char *data, qint64 maxSize) override {
		if (maxSize % frameBytes) qWarning() << "Partial frame from QAudioSource:" << maxSize << "bytes";
		sink(data, maxSize / frameBytes, 0.0);
		return maxSize;
	}
	qint64 readData(char *, qint64) override { return 0; }

private:
	CaptureBackend::Sink sink;
	const int frameBytes;
};

class QtCapture final : public CaptureBackend {
public:
	QtCapture(QAudioDevice dev, const AudioFormat &f) : dev(std::move(dev)) {
		fmt.setChannelCount(f.channels);
		fmt.setSampleRate(f.rate);
		fmt.setSampleFormat(toQt(f.format));
	}
	~QtCapture() override { stop(); }

	bool start(Sink sink, ErrorHandler onError, std::string &err) override {
		if (dev.isNull()) {
			err = "Capture device not found";
			return false;
		}
		if (!dev.isFormatSupported(fmt)) {
			err = "Format not supported by '" + dev.description().toStdString() + "'";
			return false;
		}
		fwd = std::make_unique<Forwarder>(std::move(sink), fmt.bytesPerFrame());
		fwd->open(QIODevice::WriteOnly);
		src = std::make_unique<QAudioSource>(dev, fmt);
		QObject::connect(src.get(), &QAudioSource::stateChanged, [this, onError](QAudio::State s) {
			if (s == QAudio::StoppedState && src->error() != QAudio::NoError && onError)
				onError("QAudioSource error " + std::to_string(src->error()));
		});
		src->start(fwd.get());
		if (src->error() != QAudio::NoError) {
			err = "QAudioSource failed to start (error " + std::to_string(src->error()) + ")";
			stop();
			return false;
		}
		return true;
	}

	void stop() override {
		if (src) {
			src->disconnect();
			src->stop();
		}
		src.reset();
		fwd.reset();
	}

private:
	QAudioDevice dev;
	QAudioFormat fmt;
	std::unique_ptr<Forwarder> fwd;
	std::unique_ptr<QAudioSource> src;
};

std::vector<DeviceInfo> listDevices() {
	std::vector<DeviceInfo> out;
	for (const auto &d : QMediaDevices::audioInputs()) {
		const auto pf = d.preferredFormat();
		AudioFormat f{pf.channelCount(), pf.sampleRate(), SampleFormat::Int16};
		if (pf.sampleFormat() == QAudioFormat::Int32) f.format = SampleFormat::Int32;
		if (pf.sampleFormat() == QAudioFormat::Float) f.format = SampleFormat::Float32;
		out.push_back({d.id().toStdString(), d.description().toStdString(), f});
	}
	return out;
}

std::unique_ptr<CaptureBackend> make(const std::string &deviceId, const AudioFormat &fmt) {
	QAudioDevice dev;
	for (const auto &d : QMediaDevices::audioInputs())
		if (d.id().toStdString() == deviceId) dev = d;
	return std::make_unique<QtCapture>(dev, fmt);
}

} // namespace

extern const CaptureApi kQtApi{"QtMultimedia", listDevices, make, true};
