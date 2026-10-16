#ifndef ADAPTIVE_MEDIA_HPP
#define ADAPTIVE_MEDIA_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adaptive {

enum class RawPixelFormat {
    Yuyv422,
    Rgb565,
};

struct RawFrame {
    std::uint64_t captureTimestampUs = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    RawPixelFormat pixelFormat = RawPixelFormat::Yuyv422;
    std::vector<std::uint8_t> data;
};

struct EncodedFrame {
    std::vector<std::uint8_t> accessUnit;
    bool keyFrame = false;
};

class V4l2Capture {
public:
    V4l2Capture();
    ~V4l2Capture();
    V4l2Capture(const V4l2Capture &) = delete;
    V4l2Capture &operator=(const V4l2Capture &) = delete;
    bool open(const std::string &device, int width, int height, int fps,
              std::string *error = nullptr);
    bool read(RawFrame &frame, int timeoutMs, std::string *error = nullptr);
    void close();
    int width() const;
    int height() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder &) = delete;
    H264Encoder &operator=(const H264Encoder &) = delete;
    bool open(int width, int height, int fps, std::uint32_t bitrateBps,
              std::uint16_t gopFrames, std::string *error = nullptr);
    bool encode(const RawFrame &frame, EncodedFrame &encoded,
                std::string *error = nullptr);
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;
    bool open(std::string *error = nullptr);
    bool decode(const std::vector<std::uint8_t> &accessUnit,
                int &decodedFrames, std::string *error = nullptr);
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TsRecorder {
public:
    TsRecorder();
    ~TsRecorder();
    TsRecorder(const TsRecorder &) = delete;
    TsRecorder &operator=(const TsRecorder &) = delete;
    bool open(const std::string &path, int width, int height, int fps,
              std::string *error = nullptr);
    bool write(const std::vector<std::uint8_t> &accessUnit, bool keyFrame,
               std::uint64_t captureTimestampUs, std::string *error = nullptr);
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adaptive

#endif
