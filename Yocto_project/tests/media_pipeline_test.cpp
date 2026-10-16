#include "adaptive/media.hpp"
#include "adaptive/protocol.hpp"

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        std::cerr << "requirement failed at line " << __LINE__ << ": " #expression \
                  << (error.empty() ? "" : " (" + error + ")") << '\n'; \
        return 1; \
    } \
} while (false)

adaptive::RawFrame makeFrame(std::uint32_t index) {
    adaptive::RawFrame frame;
    frame.captureTimestampUs = 1000000ULL + index * 100000ULL;
    frame.width = 160;
    frame.height = 120;
    frame.data.resize(static_cast<std::size_t>(frame.width) * frame.height * 2);
    for (std::size_t i = 0; i < frame.data.size(); i += 4) {
        frame.data[i] = static_cast<std::uint8_t>(32 + (i / 4 + index * 5) % 180);
        frame.data[i + 1] = 100;
        frame.data[i + 2] = static_cast<std::uint8_t>(32 + (i / 8 + index * 3) % 180);
        frame.data[i + 3] = 150;
    }
    return frame;
}

} // namespace

int main() {
    const char *recordingPath = "/tmp/adaptive_media_pipeline_test.ts";
    std::remove(recordingPath);
    std::string error;
    adaptive::H264Encoder encoder;
    adaptive::H264Decoder decoder;
    adaptive::TsRecorder recorder;
    REQUIRE(encoder.open(160, 120, 10, 150000, 10, &error));
    REQUIRE(decoder.open(&error));
    REQUIRE(recorder.open(recordingPath, 160, 120, 10, &error));

    int totalDecoded = 0, keyFrames = 0;
    adaptive::StreamParser parser;
    for (std::uint32_t index = 0; index < 24; ++index) {
        const adaptive::RawFrame raw = makeFrame(index);
        adaptive::EncodedFrame encoded;
        REQUIRE(encoder.encode(raw, encoded, &error));
        if (encoded.keyFrame) ++keyFrames;

        adaptive::VideoMessage input;
        input.sequence = index;
        input.captureTimestampUs = raw.captureTimestampUs;
        input.width = raw.width;
        input.height = raw.height;
        input.targetBitrateBps = 150000;
        input.gopFrames = 10;
        input.framesPerSecond = 10;
        input.keyFrame = encoded.keyFrame;
        input.accessUnit = encoded.accessUnit;
        const auto wire = adaptive::serializeVideo(input);
        const std::size_t split = wire.size() / 3;
        parser.append(wire.data(), split);
        parser.append(wire.data() + split, wire.size() - split);
        adaptive::Message message;
        REQUIRE(parser.pop(message, &error));
        adaptive::VideoMessage output;
        REQUIRE(adaptive::parseVideo(message, output, &error));
        int decoded = 0;
        REQUIRE(decoder.decode(output.accessUnit, decoded, &error));
        totalDecoded += decoded;
        REQUIRE(recorder.write(output.accessUnit, output.keyFrame,
                               output.captureTimestampUs, &error));
    }
    recorder.close();
    REQUIRE(totalDecoded >= 20);
    REQUIRE(keyFrames >= 2);
    FILE *recording = std::fopen(recordingPath, "rb");
    REQUIRE(recording);
    REQUIRE(std::fseek(recording, 0, SEEK_END) == 0);
    REQUIRE(std::ftell(recording) > 1024);
    std::fclose(recording);
    std::remove(recordingPath);
    std::cout << "media pipeline tests passed: decoded=" << totalDecoded
              << " keyframes=" << keyFrames << '\n';
}
