#include "adaptive/media.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace adaptive {
namespace {

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

void setError(std::string *error, const std::string &text) {
    if (error) *error = text;
}

} // namespace

class H264Encoder::Impl {
public:
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *scaler = nullptr;
    std::int64_t pts = 0;
};

H264Encoder::H264Encoder() : impl_(new Impl) {}
H264Encoder::~H264Encoder() { close(); }

bool H264Encoder::open(int width, int height, int fps, std::uint32_t bitrateBps,
                       std::uint16_t gopFrames, std::string *error) {
    close();
    if (width < 2 || height < 2 || width > 4096 || height > 2160 || fps < 1 ||
        fps > 120 || bitrateBps < 10000 || gopFrames == 0) {
        setError(error, "invalid encoder parameters");
        return false;
    }
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        setError(error, "FFmpeg has no H.264 encoder; enable libx264 in the Yocto ffmpeg recipe");
        return false;
    }
    impl_->context = avcodec_alloc_context3(codec);
    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!impl_->context || !impl_->frame || !impl_->packet) {
        setError(error, "FFmpeg allocation failed");
        close();
        return false;
    }
    AVCodecContext *context = impl_->context;
    context->width = width & ~1;
    context->height = height & ~1;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{1, fps};
    context->framerate = AVRational{fps, 1};
    context->bit_rate = bitrateBps;
    context->rc_max_rate = bitrateBps;
    context->rc_buffer_size = std::max<std::uint32_t>(bitrateBps / 2, 50000);
    context->gop_size = gopFrames;
    context->max_b_frames = 0;
    context->thread_count = 1;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "preset", "ultrafast", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
    av_dict_set(&options, "profile", "baseline", 0);
    av_dict_set(&options, "repeat_headers", "1", 0);
    av_dict_set(&options, "x264-params", "repeat-headers=1:scenecut=0", 0);
    const int opened = avcodec_open2(context, codec, &options);
    av_dict_free(&options);
    if (opened < 0) {
        setError(error, "avcodec_open2: " + avError(opened));
        close();
        return false;
    }
    impl_->frame->format = context->pix_fmt;
    impl_->frame->width = context->width;
    impl_->frame->height = context->height;
    const int allocated = av_frame_get_buffer(impl_->frame, 32);
    if (allocated < 0) {
        setError(error, "av_frame_get_buffer: " + avError(allocated));
        close();
        return false;
    }
    impl_->pts = 0;
    return true;
}

bool H264Encoder::encode(const RawFrame &raw, EncodedFrame &encoded, std::string *error) {
    const std::size_t expectedBytes = static_cast<std::size_t>(raw.width) * raw.height * 2;
    if (!impl_->context || raw.width == 0 || raw.height == 0 ||
        raw.data.size() < expectedBytes) {
        setError(error, "encoder is not open or raw frame is invalid");
        return false;
    }
    if (av_frame_make_writable(impl_->frame) < 0) {
        setError(error, "encoder frame is not writable");
        return false;
    }
    AVPixelFormat sourceFormat = AV_PIX_FMT_YUYV422;
    if (raw.pixelFormat == RawPixelFormat::Rgb565) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        sourceFormat = AV_PIX_FMT_RGB565LE;
#else
        sourceFormat = AV_PIX_FMT_RGB565BE;
#endif
    }
    impl_->scaler = sws_getCachedContext(impl_->scaler, raw.width, raw.height,
        sourceFormat, impl_->context->width, impl_->context->height,
        AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->scaler) {
        setError(error, "sws_getCachedContext failed");
        return false;
    }
    const std::uint8_t *source[] = {raw.data.data(), nullptr, nullptr, nullptr};
    int strides[] = {static_cast<int>(raw.width) * 2, 0, 0, 0};
    sws_scale(impl_->scaler, source, strides, 0, raw.height,
              impl_->frame->data, impl_->frame->linesize);
    impl_->frame->pts = impl_->pts++;
    encoded.accessUnit.clear();
    encoded.keyFrame = false;
    const int sent = avcodec_send_frame(impl_->context, impl_->frame);
    if (sent < 0) {
        setError(error, "avcodec_send_frame: " + avError(sent));
        return false;
    }
    while (true) {
        const int received = avcodec_receive_packet(impl_->context, impl_->packet);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
        if (received < 0) {
            setError(error, "avcodec_receive_packet: " + avError(received));
            return false;
        }
        encoded.accessUnit.insert(encoded.accessUnit.end(), impl_->packet->data,
                                  impl_->packet->data + impl_->packet->size);
        encoded.keyFrame = encoded.keyFrame || (impl_->packet->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(impl_->packet);
    }
    if (encoded.accessUnit.empty()) {
        setError(error, "encoder produced no access unit");
        return false;
    }
    return true;
}

void H264Encoder::close() {
    if (impl_->scaler) sws_freeContext(impl_->scaler);
    impl_->scaler = nullptr;
    av_packet_free(&impl_->packet);
    av_frame_free(&impl_->frame);
    avcodec_free_context(&impl_->context);
    impl_->pts = 0;
}

class H264Decoder::Impl {
public:
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
};

H264Decoder::H264Decoder() : impl_(new Impl) {}
H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::open(std::string *error) {
    close();
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    impl_->context = codec ? avcodec_alloc_context3(codec) : nullptr;
    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!codec || !impl_->context || !impl_->frame || !impl_->packet) {
        setError(error, "H.264 decoder allocation failed");
        close();
        return false;
    }
    impl_->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    const int opened = avcodec_open2(impl_->context, codec, nullptr);
    if (opened < 0) {
        setError(error, "decoder avcodec_open2: " + avError(opened));
        close();
        return false;
    }
    return true;
}

bool H264Decoder::decode(const std::vector<std::uint8_t> &accessUnit,
                         int &decodedFrames, std::string *error) {
    decodedFrames = 0;
    if (!impl_->context || accessUnit.empty() || accessUnit.size() > INT_MAX) {
        setError(error, "decoder is not open or access unit is invalid");
        return false;
    }
    av_packet_unref(impl_->packet);
    const int allocated = av_new_packet(impl_->packet, static_cast<int>(accessUnit.size()));
    if (allocated < 0) {
        setError(error, "av_new_packet: " + avError(allocated));
        return false;
    }
    std::memcpy(impl_->packet->data, accessUnit.data(), accessUnit.size());
    const int sent = avcodec_send_packet(impl_->context, impl_->packet);
    av_packet_unref(impl_->packet);
    if (sent < 0) {
        setError(error, "avcodec_send_packet: " + avError(sent));
        return false;
    }
    while (true) {
        const int received = avcodec_receive_frame(impl_->context, impl_->frame);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
        if (received < 0) {
            setError(error, "avcodec_receive_frame: " + avError(received));
            return false;
        }
        ++decodedFrames;
        av_frame_unref(impl_->frame);
    }
    return true;
}

void H264Decoder::close() {
    av_packet_free(&impl_->packet);
    av_frame_free(&impl_->frame);
    avcodec_free_context(&impl_->context);
}

} // namespace adaptive
