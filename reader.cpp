#include "reader.h"
#include <chrono>
#include <thread>
#include <QDebug>

LslPusher::LslPusher(lsl::stream_outlet &&outlet)
	: out(std::move(outlet)), sample_bytes(out.info().sample_bytes()),
	  cf(out.info().channel_format()) {}

qint64 LslPusher::writeData(const char* data, qint64 maxSize)
{
	// qInfo() << "Write " << maxSize << ' ' << (maxSize/sample_bytes) << ' ' << (maxSize%sample_bytes);
	switch(cf) {
	case lsl::cf_int8:
		out.push_chunk_multiplexed(data, maxSize);
		break;
	case lsl::cf_int16:
		out.push_chunk_multiplexed(reinterpret_cast<const int16_t *>(data), maxSize / 2);
		break;
	case lsl::cf_float32:
		out.push_chunk_multiplexed(reinterpret_cast<const float *>(data), maxSize / 4);
		break;
	}
	return maxSize;
}
