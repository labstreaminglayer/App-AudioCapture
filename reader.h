#pragma once
#include <QAudioInput>
#include <QBuffer>
#include <cstdint>
#include <vector>
#include <lsl_cpp.h>

class LslPusher : public QIODevice {
public:
	LslPusher(lsl::stream_outlet &&outlet);
	qint64 writeData(const char *data, qint64 maxSize) override;
	qint64 readData(char*, qint64 maxSize) override { return maxSize;}
	qint64 samples_written() const { return pos() / sample_bytes; }

private:
	lsl::stream_outlet out;
	const int sample_bytes;
	lsl::channel_format_t cf;
};
