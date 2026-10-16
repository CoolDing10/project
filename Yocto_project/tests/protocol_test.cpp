#include "adaptive/protocol.hpp"

#include <iostream>

#define REQUIRE(expression) do { if (!(expression)) { \
    std::cerr << "requirement failed at line " << __LINE__ << ": " #expression << '\n'; \
    return 1; } } while (false)

int main() {
    adaptive::VideoMessage input;
    input.sequence = 42;
    input.captureTimestampUs = 123456789;
    input.width = 640;
    input.height = 480;
    input.targetBitrateBps = 350000;
    input.gopFrames = 15;
    input.framesPerSecond = 10;
    input.keyFrame = true;
    input.accessUnit = {0, 0, 0, 1, 0x65, 1, 2, 3};
    const auto wire = adaptive::serializeVideo(input);

    adaptive::StreamParser parser;
    parser.append(wire.data(), 7);
    adaptive::Message message;
    REQUIRE(!parser.pop(message));
    parser.append(wire.data() + 7, wire.size() - 7);
    REQUIRE(parser.pop(message));
    adaptive::VideoMessage output;
    REQUIRE(adaptive::parseVideo(message, output));
    REQUIRE(output.sequence == input.sequence);
    REQUIRE(output.width == input.width && output.height == input.height);
    REQUIRE(output.targetBitrateBps == input.targetBitrateBps);
    REQUIRE(output.gopFrames == input.gopFrames && output.keyFrame);
    REQUIRE(output.framesPerSecond == input.framesPerSecond);
    REQUIRE(output.accessUnit == input.accessUnit);

    // Corrupt bytes and an invalid header must not prevent a complete message
    // already in the same TCP read from being recovered.
    std::vector<std::uint8_t> corrupt = {'x', 'x', 'A', 'V', 'T', 'S', 0, 99,
                                         0, 1, 0, 0, 0, 0, 0, 0};
    corrupt.insert(corrupt.end(), wire.begin(), wire.end());
    parser.append(corrupt.data(), corrupt.size());
    REQUIRE(parser.pop(message));
    REQUIRE(adaptive::parseVideo(message, output));
    REQUIRE(output.sequence == input.sequence);

    adaptive::FeedbackMessage feedback;
    feedback.sequence = 9;
    feedback.reportTimestampUs = 987654321;
    feedback.intervalMs = 500;
    feedback.receivedBytes = 12345;
    feedback.receivedFrames = 15;
    feedback.droppedFrames = 2;
    feedback.stallEvents = 1;
    feedback.decodeLatencyUs = 4500;
    feedback.endToEndLatencyUs = 125000;
    feedback.jitterUs = 2300;
    feedback.receiverBufferMs = 20;
    feedback.lastVideoSequence = 41;
    const auto feedbackWire = adaptive::serializeFeedback(feedback);
    parser.append(feedbackWire.data(), feedbackWire.size());
    REQUIRE(parser.pop(message));
    adaptive::FeedbackMessage parsedFeedback;
    REQUIRE(adaptive::parseFeedback(message, parsedFeedback));
    REQUIRE(parsedFeedback.receivedBytes == feedback.receivedBytes);
    REQUIRE(parsedFeedback.stallEvents == feedback.stallEvents);
    REQUIRE(parsedFeedback.endToEndLatencyUs == feedback.endToEndLatencyUs);
    REQUIRE(parsedFeedback.lastVideoSequence == feedback.lastVideoSequence);
    std::cout << "protocol tests passed\n";
}
