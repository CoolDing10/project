#ifndef ADAPTIVE_PROTOCOL_HPP
#define ADAPTIVE_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adaptive {

constexpr std::uint32_t kProtocolMagic = 0x41565453U; // "AVTS"
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kWireHeaderSize = 16;
constexpr std::size_t kMaximumMessageSize = 8U * 1024U * 1024U;

enum class MessageType : std::uint16_t {
    Video = 1,
    Feedback = 2,
};

struct Message {
    MessageType type = MessageType::Video;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

struct VideoMessage {
    std::uint32_t sequence = 0;
    std::uint64_t captureTimestampUs = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t targetBitrateBps = 0;
    std::uint16_t gopFrames = 0;
    std::uint16_t framesPerSecond = 0;
    bool keyFrame = false;
    std::vector<std::uint8_t> accessUnit;
};

struct FeedbackMessage {
    std::uint32_t sequence = 0;
    std::uint64_t reportTimestampUs = 0;
    std::uint32_t intervalMs = 0;
    std::uint64_t receivedBytes = 0;
    std::uint32_t receivedFrames = 0;
    std::uint32_t droppedFrames = 0;
    std::uint32_t stallEvents = 0;
    std::uint32_t decodeLatencyUs = 0;
    std::uint32_t endToEndLatencyUs = 0;
    std::uint32_t jitterUs = 0;
    std::uint32_t receiverBufferMs = 0;
    std::uint32_t lastVideoSequence = 0;
};

std::vector<std::uint8_t> serializeVideo(const VideoMessage &video);
std::vector<std::uint8_t> serializeFeedback(const FeedbackMessage &feedback);
bool parseVideo(const Message &message, VideoMessage &video, std::string *error = nullptr);
bool parseFeedback(const Message &message, FeedbackMessage &feedback,
                   std::string *error = nullptr);

// TCP has no message boundary. Feed arbitrary byte chunks and repeatedly pop
// complete, validated messages from this parser.
class StreamParser {
public:
    void append(const std::uint8_t *data, std::size_t size);
    bool pop(Message &message, std::string *error = nullptr);
    void reset();
    std::size_t bufferedBytes() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
};

} // namespace adaptive

#endif
