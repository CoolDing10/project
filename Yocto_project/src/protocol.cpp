#include "adaptive/protocol.hpp"

#include <algorithm>

namespace adaptive {
namespace {

void put16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

void put64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

bool get16(const std::vector<std::uint8_t> &in, std::size_t &offset,
           std::uint16_t &value) {
    if (offset + 2 > in.size()) return false;
    value = static_cast<std::uint16_t>((in[offset] << 8) | in[offset + 1]);
    offset += 2;
    return true;
}

bool get32(const std::vector<std::uint8_t> &in, std::size_t &offset,
           std::uint32_t &value) {
    if (offset + 4 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | in[offset++];
    return true;
}

bool get64(const std::vector<std::uint8_t> &in, std::size_t &offset,
           std::uint64_t &value) {
    if (offset + 8 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | in[offset++];
    return true;
}

std::vector<std::uint8_t> wrap(MessageType type, std::uint32_t sequence,
                               const std::vector<std::uint8_t> &payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kWireHeaderSize + payload.size());
    put32(out, kProtocolMagic);
    put16(out, kProtocolVersion);
    put16(out, static_cast<std::uint16_t>(type));
    put32(out, static_cast<std::uint32_t>(payload.size()));
    put32(out, sequence);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool fail(std::string *error, const char *message) {
    if (error) *error = message;
    return false;
}

} // namespace

std::vector<std::uint8_t> serializeVideo(const VideoMessage &video) {
    constexpr std::size_t metadataSize = 26;
    if (video.width == 0 || video.height == 0 || video.framesPerSecond == 0 ||
        video.framesPerSecond > 240 || video.accessUnit.empty() ||
        video.accessUnit.size() > kMaximumMessageSize - metadataSize)
        return {};
    std::vector<std::uint8_t> payload;
    payload.reserve(26 + video.accessUnit.size());
    put64(payload, video.captureTimestampUs);
    put16(payload, video.width);
    put16(payload, video.height);
    put32(payload, video.targetBitrateBps);
    put16(payload, video.gopFrames);
    put16(payload, video.framesPerSecond);
    put16(payload, video.keyFrame ? 1U : 0U);
    put32(payload, static_cast<std::uint32_t>(video.accessUnit.size()));
    payload.insert(payload.end(), video.accessUnit.begin(), video.accessUnit.end());
    return wrap(MessageType::Video, video.sequence, payload);
}

std::vector<std::uint8_t> serializeFeedback(const FeedbackMessage &feedback) {
    if (feedback.intervalMs == 0 || feedback.intervalMs > 60000) return {};
    std::vector<std::uint8_t> payload;
    payload.reserve(52);
    put64(payload, feedback.reportTimestampUs);
    put32(payload, feedback.intervalMs);
    put64(payload, feedback.receivedBytes);
    put32(payload, feedback.receivedFrames);
    put32(payload, feedback.droppedFrames);
    put32(payload, feedback.stallEvents);
    put32(payload, feedback.decodeLatencyUs);
    put32(payload, feedback.endToEndLatencyUs);
    put32(payload, feedback.jitterUs);
    put32(payload, feedback.receiverBufferMs);
    put32(payload, feedback.lastVideoSequence);
    return wrap(MessageType::Feedback, feedback.sequence, payload);
}

bool parseVideo(const Message &message, VideoMessage &video, std::string *error) {
    if (message.type != MessageType::Video) return fail(error, "not a video message");
    std::size_t offset = 0;
    std::uint16_t flags = 0;
    std::uint32_t bytes = 0;
    video.sequence = message.sequence;
    if (!get64(message.payload, offset, video.captureTimestampUs) ||
        !get16(message.payload, offset, video.width) ||
        !get16(message.payload, offset, video.height) ||
        !get32(message.payload, offset, video.targetBitrateBps) ||
        !get16(message.payload, offset, video.gopFrames) ||
        !get16(message.payload, offset, video.framesPerSecond) ||
        !get16(message.payload, offset, flags) ||
        !get32(message.payload, offset, bytes))
        return fail(error, "truncated video metadata");
    if (video.width == 0 || video.height == 0 || video.framesPerSecond == 0 ||
        video.framesPerSecond > 240 || bytes == 0 ||
        bytes > kMaximumMessageSize || offset + bytes != message.payload.size())
        return fail(error, "invalid video payload length or dimensions");
    video.keyFrame = (flags & 1U) != 0;
    video.accessUnit.assign(message.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                            message.payload.end());
    return true;
}

bool parseFeedback(const Message &message, FeedbackMessage &feedback,
                   std::string *error) {
    if (message.type != MessageType::Feedback) return fail(error, "not a feedback message");
    std::size_t offset = 0;
    feedback.sequence = message.sequence;
    if (!get64(message.payload, offset, feedback.reportTimestampUs) ||
        !get32(message.payload, offset, feedback.intervalMs) ||
        !get64(message.payload, offset, feedback.receivedBytes) ||
        !get32(message.payload, offset, feedback.receivedFrames) ||
        !get32(message.payload, offset, feedback.droppedFrames) ||
        !get32(message.payload, offset, feedback.stallEvents) ||
        !get32(message.payload, offset, feedback.decodeLatencyUs) ||
        !get32(message.payload, offset, feedback.endToEndLatencyUs) ||
        !get32(message.payload, offset, feedback.jitterUs) ||
        !get32(message.payload, offset, feedback.receiverBufferMs) ||
        !get32(message.payload, offset, feedback.lastVideoSequence) ||
        offset != message.payload.size())
        return fail(error, "invalid feedback payload");
    if (feedback.intervalMs == 0 || feedback.intervalMs > 60000)
        return fail(error, "invalid feedback interval");
    return true;
}

void StreamParser::append(const std::uint8_t *data, std::size_t size) {
    if (!data || size == 0) return;
    if (size > kMaximumMessageSize * 2) {
        buffer_.clear();
        return;
    }
    if (buffer_.size() + size > kMaximumMessageSize * 2) buffer_.clear();
    buffer_.insert(buffer_.end(), data, data + size);
}

bool StreamParser::pop(Message &message, std::string *error) {
    const std::uint8_t magic[] = {'A', 'V', 'T', 'S'};
    while (true) {
        while (buffer_.size() >= 4 && !std::equal(magic, magic + 4, buffer_.begin())) {
            auto next = std::search(buffer_.begin() + 1, buffer_.end(), magic, magic + 4);
            if (next == buffer_.end()) {
                const std::size_t keep = std::min<std::size_t>(3, buffer_.size());
                buffer_.erase(buffer_.begin(), buffer_.end() - static_cast<std::ptrdiff_t>(keep));
                return false;
            }
            buffer_.erase(buffer_.begin(), next);
        }
        if (buffer_.size() < kWireHeaderSize) return false;

        std::size_t offset = 0;
        std::uint32_t magicValue = 0, payloadSize = 0, sequence = 0;
        std::uint16_t version = 0, typeValue = 0;
        if (!get32(buffer_, offset, magicValue) || !get16(buffer_, offset, version) ||
            !get16(buffer_, offset, typeValue) || !get32(buffer_, offset, payloadSize) ||
            !get32(buffer_, offset, sequence))
            return false;
        if (magicValue != kProtocolMagic || version != kProtocolVersion ||
            (typeValue != static_cast<std::uint16_t>(MessageType::Video) &&
             typeValue != static_cast<std::uint16_t>(MessageType::Feedback)) ||
            payloadSize > kMaximumMessageSize) {
            buffer_.erase(buffer_.begin());
            if (error) *error = "discarded invalid protocol header";
            continue;
        }
        const std::size_t total = kWireHeaderSize + payloadSize;
        if (buffer_.size() < total) return false;
        message.type = static_cast<MessageType>(typeValue);
        message.sequence = sequence;
        message.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize),
                               buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        return true;
    }
}

void StreamParser::reset() { buffer_.clear(); }

} // namespace adaptive
