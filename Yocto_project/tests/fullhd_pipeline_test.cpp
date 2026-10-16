#include "adaptive/media.hpp"

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

adaptive::RawFrame makeFullHdFrame(std::uint32_t index) {
    adaptive::RawFrame frame;
    frame.captureTimestampUs = 1000000ULL + index * 40000ULL;
    frame.width = 1920;
    frame.height = 1080;
    frame.data.resize(static_cast<std::size_t>(frame.width) * frame.height * 2);
    for (std::uint16_t y = 0; y < frame.height; ++y) {
        for (std::uint16_t x = 0; x < frame.width; x += 2) {
            const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 2;
            frame.data[offset] = static_cast<std::uint8_t>(16 + (x + index * 5) % 220);
            frame.data[offset + 1] = static_cast<std::uint8_t>(80 + (y / 8) % 96);
            frame.data[offset + 2] = static_cast<std::uint8_t>(16 + (y + index * 3) % 220);
            frame.data[offset + 3] = static_cast<std::uint8_t>(80 + (x / 8) % 96);
        }
    }
    return frame;
}

} // namespace

int main() {
    const char *recordingPath = "/tmp/adaptive_fullhd_pipeline_test.ts";
    std::remove(recordingPath);
    std::string error;
    adaptive::H264Encoder encoder;
    adaptive::H264Decoder decoder;
    adaptive::TsRecorder recorder;
    REQUIRE(encoder.open(1920, 1080, 25, 4500000, 25, &error));
    REQUIRE(decoder.open(&error));
    REQUIRE(recorder.open(recordingPath, 1920, 1080, 25, &error));

    int decodedFrames = 0;
    int keyFrames = 0;
    for (std::uint32_t index = 0; index < 30; ++index) {
        const adaptive::RawFrame raw = makeFullHdFrame(index);
        adaptive::EncodedFrame encoded;
        REQUIRE(encoder.encode(raw, encoded, &error));
        if (encoded.keyFrame) ++keyFrames;
        int decoded = 0;
        REQUIRE(decoder.decode(encoded.accessUnit, decoded, &error));
        decodedFrames += decoded;
        REQUIRE(recorder.write(encoded.accessUnit, encoded.keyFrame,
                               raw.captureTimestampUs, &error));
    }
    recorder.close();
    REQUIRE(decodedFrames >= 28);
    REQUIRE(keyFrames >= 2);

    FILE *recording = std::fopen(recordingPath, "rb");
    REQUIRE(recording);
    REQUIRE(std::fseek(recording, 0, SEEK_END) == 0);
    REQUIRE(std::ftell(recording) > 100000);
    REQUIRE(std::fseek(recording, 0, SEEK_SET) == 0);
    REQUIRE(std::fgetc(recording) == 0x47); // MPEG-TS sync byte
    std::fclose(recording);
    std::remove(recordingPath);
    std::cout << "1080P pipeline passed: decoded=" << decodedFrames
              << " keyframes=" << keyFrames << '\n';
}

