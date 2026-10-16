#include "adaptive/media.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace adaptive {
namespace {

bool ioctlRetry(int fd, unsigned long request, void *argument) {
    int result;
    do result = ::ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
    return result == 0;
}

void setError(std::string *error, const std::string &text) {
    if (error) *error = text;
}

std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

class V4l2Capture::Impl {
public:
    struct Buffer { void *address = MAP_FAILED; std::size_t length = 0; };
    int fd = -1;
    int width = 0;
    int height = 0;
    int bytesPerLine = 0;
    RawPixelFormat pixelFormat = RawPixelFormat::Yuyv422;
    bool streaming = false;
    std::vector<Buffer> buffers;
};

V4l2Capture::V4l2Capture() : impl_(new Impl) {}
V4l2Capture::~V4l2Capture() { close(); }

bool V4l2Capture::open(const std::string &device, int requestedWidth,
                       int requestedHeight, int fps, std::string *error) {
    close();
    if (device.empty() || requestedWidth < 2 || requestedHeight < 2 ||
        requestedWidth > 4096 || requestedHeight > 2160 || fps < 1 || fps > 120) {
        setError(error, "invalid capture parameters");
        return false;
    }
    impl_->fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (impl_->fd < 0) {
        setError(error, "open " + device + ": " + std::strerror(errno));
        return false;
    }
    v4l2_capability capability{};
    if (!ioctlRetry(impl_->fd, VIDIOC_QUERYCAP, &capability) ||
        !(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(capability.capabilities & V4L2_CAP_STREAMING)) {
        setError(error, "device does not support V4L2 streaming capture");
        close();
        return false;
    }
    v4l2_format format{};
    auto setFormat = [&](std::uint32_t pixelFormat) {
        format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<std::uint32_t>(requestedWidth);
        format.fmt.pix.height = static_cast<std::uint32_t>(requestedHeight);
        format.fmt.pix.pixelformat = pixelFormat;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        return ioctlRetry(impl_->fd, VIDIOC_S_FMT, &format) &&
               (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ||
                format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565);
    };
    if (!setFormat(V4L2_PIX_FMT_YUYV) && !setFormat(V4L2_PIX_FMT_RGB565)) {
        setError(error, "camera supports neither YUYV nor RGB565 capture");
        close();
        return false;
    }
    impl_->width = static_cast<int>(format.fmt.pix.width);
    impl_->height = static_cast<int>(format.fmt.pix.height);
    impl_->bytesPerLine = std::max<int>(format.fmt.pix.bytesperline, impl_->width * 2);
    impl_->pixelFormat = format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565
        ? RawPixelFormat::Rgb565 : RawPixelFormat::Yuyv422;

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(fps);
    ioctlRetry(impl_->fd, VIDIOC_S_PARM, &parameters); // Some drivers do not expose FPS control.

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (!ioctlRetry(impl_->fd, VIDIOC_REQBUFS, &request) || request.count < 2) {
        setError(error, "VIDIOC_REQBUFS failed");
        close();
        return false;
    }
    impl_->buffers.resize(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = i;
        if (!ioctlRetry(impl_->fd, VIDIOC_QUERYBUF, &buffer)) {
            setError(error, "VIDIOC_QUERYBUF failed");
            close();
            return false;
        }
        Impl::Buffer &mapped = impl_->buffers[i];
        mapped.length = buffer.length;
        mapped.address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, impl_->fd, buffer.m.offset);
        if (mapped.address == MAP_FAILED) {
            setError(error, "mmap: " + std::string(std::strerror(errno)));
            close();
            return false;
        }
        if (!ioctlRetry(impl_->fd, VIDIOC_QBUF, &buffer)) {
            setError(error, "initial VIDIOC_QBUF failed");
            close();
            return false;
        }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!ioctlRetry(impl_->fd, VIDIOC_STREAMON, &type)) {
        setError(error, "VIDIOC_STREAMON failed");
        close();
        return false;
    }
    impl_->streaming = true;
    return true;
}

bool V4l2Capture::read(RawFrame &frame, int timeoutMs, std::string *error) {
    if (impl_->fd < 0) {
        setError(error, "capture device is not open");
        return false;
    }
    pollfd descriptor{impl_->fd, POLLIN, 0};
    int ready;
    do ready = ::poll(&descriptor, 1, timeoutMs); while (ready < 0 && errno == EINTR);
    if (ready == 0) {
        setError(error, "capture timeout");
        return false;
    }
    if (ready < 0) {
        setError(error, "poll: " + std::string(std::strerror(errno)));
        return false;
    }
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (!ioctlRetry(impl_->fd, VIDIOC_DQBUF, &buffer) || buffer.index >= impl_->buffers.size()) {
        setError(error, "VIDIOC_DQBUF failed");
        return false;
    }
    const Impl::Buffer &mapped = impl_->buffers[buffer.index];
    const std::size_t rowBytes = static_cast<std::size_t>(impl_->width) * 2;
    const std::size_t expected = rowBytes * impl_->height;
    const std::size_t required = static_cast<std::size_t>(impl_->bytesPerLine) *
                                 (impl_->height - 1) + rowBytes;
    const std::size_t bytes = std::min<std::size_t>(buffer.bytesused, mapped.length);
    bool valid = bytes >= required;
    if (valid) {
        frame.captureTimestampUs = nowUs();
        frame.width = static_cast<std::uint16_t>(impl_->width);
        frame.height = static_cast<std::uint16_t>(impl_->height);
        frame.pixelFormat = impl_->pixelFormat;
        const auto *source = static_cast<const std::uint8_t *>(mapped.address);
        frame.data.resize(expected);
        for (int row = 0; row < impl_->height; ++row)
            std::memcpy(frame.data.data() + static_cast<std::size_t>(row) * rowBytes,
                        source + static_cast<std::size_t>(row) * impl_->bytesPerLine,
                        rowBytes);
    } else {
        setError(error, "short V4L2 frame");
    }
    if (!ioctlRetry(impl_->fd, VIDIOC_QBUF, &buffer)) {
        setError(error, "VIDIOC_QBUF failed");
        return false;
    }
    return valid;
}

void V4l2Capture::close() {
    if (impl_->fd >= 0 && impl_->streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctlRetry(impl_->fd, VIDIOC_STREAMOFF, &type);
    }
    impl_->streaming = false;
    for (auto &buffer : impl_->buffers)
        if (buffer.address != MAP_FAILED) ::munmap(buffer.address, buffer.length);
    impl_->buffers.clear();
    if (impl_->fd >= 0) ::close(impl_->fd);
    impl_->fd = -1;
    impl_->width = impl_->height = 0;
    impl_->bytesPerLine = 0;
    impl_->pixelFormat = RawPixelFormat::Yuyv422;
}

int V4l2Capture::width() const { return impl_->width; }
int V4l2Capture::height() const { return impl_->height; }

} // namespace adaptive
