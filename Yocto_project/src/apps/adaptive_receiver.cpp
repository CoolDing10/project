#include "adaptive/media.hpp"
#include "adaptive/protocol.hpp"
#include "adaptive/socket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic<bool> running{true};
void stopHandler(int) { running.store(false); }

std::uint64_t systemUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::uint64_t steadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Options {
    std::uint16_t port = 8888;
    std::string recordPath;
};

std::uint16_t parsePort(const std::string &value) {
    try {
        std::size_t consumed = 0;
        const unsigned long port = std::stoul(value, &consumed);
        if (consumed != value.size() || port == 0 || port > 65535)
            throw std::out_of_range("port");
        return static_cast<std::uint16_t>(port);
    } catch (const std::exception &) {
        std::cerr << "invalid port: " << value << '\n';
        std::exit(2);
    }
}

Options parseOptions(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--port" && i + 1 < argc) options.port = parsePort(argv[++i]);
        else if (key == "--record" && i + 1 < argc) options.recordPath = argv[++i];
        else if (key == "--help") {
            std::cout << "adaptive_receiver [--port 8888] [--record capture.ts]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown or incomplete option: " << key << '\n';
            std::exit(2);
        }
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    const Options options = parseOptions(argc, argv);
    std::signal(SIGINT, stopHandler);
    std::signal(SIGTERM, stopHandler);
    adaptive::Socket server;
    std::string error;
    if (!server.listenOn(options.port, &error)) {
        std::cerr << "listen: " << error << '\n';
        return 1;
    }
    std::cout << "listening on TCP port " << options.port << '\n';
    while (running.load()) {
        if (!server.waitReadable(500)) continue;
        adaptive::Socket client = server.acceptOne(&error);
        if (!client.valid()) {
            if (running.load()) std::cerr << "accept: " << error << '\n';
            continue;
        }
        std::cout << "sender connected\n";
        adaptive::H264Decoder decoder;
        if (!decoder.open(&error)) {
            std::cerr << "decoder: " << error << '\n';
            return 1;
        }
        adaptive::TsRecorder recorder;
        bool recorderOpened = false;
        adaptive::StreamParser parser;
        std::uint8_t buffer[64 * 1024];
        std::uint64_t intervalStartUs = steadyUs();
        std::uint64_t receivedBytes = 0;
        std::uint32_t receivedFrames = 0, droppedFrames = 0, stallEvents = 0;
        std::uint32_t feedbackSequence = 0, lastSequence = 0;
        bool haveSequence = false, waitingForKeyFrame = true;
        double jitterUs = 0.0, decodeLatencyUs = 0.0, endToEndLatencyUs = 0.0;
        std::int64_t previousTransitUs = 0;
        std::uint64_t previousArrivalUs = 0;
        bool connectionHealthy = true;
        while (running.load() && connectionHealthy) {
            if (!client.waitReadable(500)) continue;
            const long count = client.receive(buffer, sizeof(buffer), &error);
            if (count <= 0) {
                if (count < 0) std::cerr << "receive: " << error << '\n';
                break;
            }
            parser.append(buffer, static_cast<std::size_t>(count));
            adaptive::Message message;
            while (parser.pop(message, &error)) {
                if (message.type != adaptive::MessageType::Video) continue;
                adaptive::VideoMessage video;
                if (!adaptive::parseVideo(message, video, &error)) {
                    std::cerr << "protocol: " << error << '\n';
                    continue;
                }
                receivedBytes += adaptive::kWireHeaderSize + message.payload.size();
                const std::uint64_t arrivalUs = steadyUs();
                const std::uint64_t stallThresholdUs = std::max<std::uint64_t>(
                    200000, 3000000ULL / video.framesPerSecond);
                if (previousArrivalUs != 0 && arrivalUs - previousArrivalUs > stallThresholdUs)
                    ++stallEvents;
                previousArrivalUs = arrivalUs;
                if (haveSequence && video.sequence != lastSequence + 1) {
                    const std::uint32_t gap = video.sequence - lastSequence - 1;
                    droppedFrames += gap;
                    waitingForKeyFrame = true;
                    decoder.close();
                    if (!decoder.open(&error)) {
                        std::cerr << "decoder reset: " << error << '\n';
                        connectionHealthy = false;
                        break;
                    }
                }
                haveSequence = true;
                lastSequence = video.sequence;
                const std::int64_t transit = static_cast<std::int64_t>(systemUs()) -
                                             static_cast<std::int64_t>(video.captureTimestampUs);
                if (previousTransitUs != 0)
                    jitterUs += (std::abs(transit - previousTransitUs) - jitterUs) / 16.0;
                previousTransitUs = transit;
                if (video.keyFrame) waitingForKeyFrame = false;
                if (transit >= 0 && transit <= 10000000)
                    endToEndLatencyUs = endToEndLatencyUs == 0.0 ? transit
                        : 0.2 * transit + 0.8 * endToEndLatencyUs;

                if (!(waitingForKeyFrame && !video.keyFrame)) {
                    if (!options.recordPath.empty() && !recorderOpened) {
                        recorderOpened = recorder.open(options.recordPath, video.width,
                                                       video.height, video.framesPerSecond, &error);
                        if (!recorderOpened) std::cerr << "recorder: " << error << '\n';
                    }
                    if (recorderOpened &&
                        !recorder.write(video.accessUnit, video.keyFrame,
                                        video.captureTimestampUs, &error)) {
                        std::cerr << "recorder write: " << error << '\n';
                        recorder.close();
                        recorderOpened = false;
                    }
                    const std::uint64_t decodeStart = steadyUs();
                    int decoded = 0;
                    if (!decoder.decode(video.accessUnit, decoded, &error)) {
                        std::cerr << "decode: " << error << '\n';
                        waitingForKeyFrame = true;
                    } else {
                        const double elapsed = static_cast<double>(steadyUs() - decodeStart);
                        decodeLatencyUs = decodeLatencyUs == 0.0 ? elapsed
                            : 0.2 * elapsed + 0.8 * decodeLatencyUs;
                        receivedFrames += static_cast<std::uint32_t>(decoded);
                    }
                }

                const std::uint64_t now = steadyUs();
                const std::uint64_t intervalUs = now - intervalStartUs;
                if (intervalUs >= 500000) {
                    adaptive::FeedbackMessage feedback;
                    feedback.sequence = feedbackSequence++;
                    feedback.reportTimestampUs = systemUs();
                    feedback.intervalMs = static_cast<std::uint32_t>(intervalUs / 1000);
                    feedback.receivedBytes = receivedBytes;
                    feedback.receivedFrames = receivedFrames;
                    feedback.droppedFrames = droppedFrames;
                    feedback.stallEvents = stallEvents;
                    feedback.decodeLatencyUs = static_cast<std::uint32_t>(decodeLatencyUs);
                    feedback.endToEndLatencyUs = static_cast<std::uint32_t>(endToEndLatencyUs);
                    feedback.jitterUs = static_cast<std::uint32_t>(jitterUs);
                    feedback.receiverBufferMs = 0;
                    feedback.lastVideoSequence = lastSequence;
                    const auto wire = adaptive::serializeFeedback(feedback);
                    if (wire.empty()) {
                        std::cerr << "feedback serialization failed\n";
                        connectionHealthy = false;
                        break;
                    }
                    if (!client.sendAll(wire.data(), wire.size(), &error)) {
                        std::cerr << "feedback send: " << error << '\n';
                        connectionHealthy = false;
                        break;
                    }
                    const double mbps = receivedBytes * 8.0 / intervalUs;
                    std::cout << "rx=" << mbps << "Mbps frames=" << receivedFrames
                              << " lost=" << droppedFrames << " stalls=" << stallEvents
                              << " jitter=" << jitterUs / 1000.0
                              << "ms decode=" << decodeLatencyUs / 1000.0
                              << "ms e2e=" << endToEndLatencyUs / 1000.0 << "ms\n";
                    intervalStartUs = now;
                    receivedBytes = 0;
                    receivedFrames = droppedFrames = stallEvents = 0;
                }
            }
        }
        recorder.close();
        client.close();
        std::cout << "sender disconnected\n";
    }
    return 0;
}
