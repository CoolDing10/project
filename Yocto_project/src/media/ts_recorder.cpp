#include "adaptive/media.hpp"

#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace adaptive {
namespace {
std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}
void setError(std::string *error, const std::string &text) { if (error) *error = text; }
} // namespace

class TsRecorder::Impl {
public:
    AVFormatContext *context = nullptr;
    AVStream *stream = nullptr;
    std::uint64_t firstTimestampUs = 0;
    bool headerWritten = false;
};

TsRecorder::TsRecorder() : impl_(new Impl) {}
TsRecorder::~TsRecorder() { close(); }

bool TsRecorder::open(const std::string &path, int width, int height, int fps,
                      std::string *error) {
    close();
    if (path.empty() || width < 2 || height < 2 || fps < 1 || fps > 240) {
        setError(error, "invalid recorder parameters");
        return false;
    }
    int result = avformat_alloc_output_context2(&impl_->context, nullptr, "mpegts", path.c_str());
    if (result < 0 || !impl_->context) {
        setError(error, "avformat_alloc_output_context2: " + avError(result));
        return false;
    }
    impl_->stream = avformat_new_stream(impl_->context, nullptr);
    if (!impl_->stream) {
        setError(error, "avformat_new_stream failed");
        close();
        return false;
    }
    impl_->stream->time_base = AVRational{1, 90000};
    impl_->stream->avg_frame_rate = AVRational{fps, 1};
    impl_->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    impl_->stream->codecpar->codec_id = AV_CODEC_ID_H264;
    impl_->stream->codecpar->width = width;
    impl_->stream->codecpar->height = height;
    if (!(impl_->context->oformat->flags & AVFMT_NOFILE)) {
        result = avio_open(&impl_->context->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) {
            setError(error, "avio_open: " + avError(result));
            close();
            return false;
        }
    }
    result = avformat_write_header(impl_->context, nullptr);
    if (result < 0) {
        setError(error, "avformat_write_header: " + avError(result));
        close();
        return false;
    }
    impl_->headerWritten = true;
    return true;
}

bool TsRecorder::write(const std::vector<std::uint8_t> &accessUnit, bool keyFrame,
                       std::uint64_t timestampUs, std::string *error) {
    if (!impl_->context || accessUnit.empty()) return false;
    if (impl_->firstTimestampUs == 0) impl_->firstTimestampUs = timestampUs;
    AVPacket *packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, static_cast<int>(accessUnit.size())) < 0) {
        av_packet_free(&packet);
        setError(error, "recording packet allocation failed");
        return false;
    }
    std::memcpy(packet->data, accessUnit.data(), accessUnit.size());
    packet->stream_index = impl_->stream->index;
    const std::int64_t relativeUs = timestampUs >= impl_->firstTimestampUs
        ? static_cast<std::int64_t>(timestampUs - impl_->firstTimestampUs) : 0;
    packet->pts = packet->dts = av_rescale_q(relativeUs, AVRational{1, 1000000},
                                             impl_->stream->time_base);
    if (keyFrame) packet->flags |= AV_PKT_FLAG_KEY;
    const int result = av_interleaved_write_frame(impl_->context, packet);
    av_packet_free(&packet);
    if (result < 0) {
        setError(error, "av_interleaved_write_frame: " + avError(result));
        return false;
    }
    return true;
}

void TsRecorder::close() {
    if (!impl_->context) return;
    if (impl_->headerWritten) av_write_trailer(impl_->context);
    if (!(impl_->context->oformat->flags & AVFMT_NOFILE) && impl_->context->pb)
        avio_closep(&impl_->context->pb);
    avformat_free_context(impl_->context);
    impl_->context = nullptr;
    impl_->stream = nullptr;
    impl_->firstTimestampUs = 0;
    impl_->headerWritten = false;
}

} // namespace adaptive
