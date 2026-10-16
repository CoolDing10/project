#include "adaptive/controller.hpp"
#include "adaptive/latest_queue.hpp"
#include "adaptive/media.hpp"
#include "adaptive/protocol.hpp"
#include "adaptive/socket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running{true};
void stopHandler(int) { running.store(false); }

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8888;
    std::string device = "/dev/video0";
    int width = 1920;
    int height = 1080;
    int fps = 25;
    std::string mode = "mpc";
    std::uint32_t minBitrate = 800000;
    std::uint32_t maxBitrate = 6000000;
    std::uint32_t initialBitrate = 2500000;
    std::uint16_t initialGop = 50;
    std::string metrics = "/tmp/adaptive_sender_metrics.csv";
    bool synthetic = false;
};

bool takeValue(int &index, int argc, char **argv, std::string &value) {
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

unsigned long parseUnsigned(const std::string &value, const char *name,
                            unsigned long maximum) {
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed > maximum) throw std::out_of_range(name);
        return parsed;
    } catch (const std::exception &) {
        std::cerr << "invalid " << name << ": " << value << '\n';
        std::exit(2);
    }
}

Options parseOptions(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i], value;
        if (key == "--host" && takeValue(i, argc, argv, value)) options.host = value;
        else if (key == "--port" && takeValue(i, argc, argv, value))
            options.port = static_cast<std::uint16_t>(parseUnsigned(value, "port", 65535));
        else if (key == "--device" && takeValue(i, argc, argv, value)) options.device = value;
        else if (key == "--width" && takeValue(i, argc, argv, value)) options.width = parseUnsigned(value, "width", 4096);
        else if (key == "--height" && takeValue(i, argc, argv, value)) options.height = parseUnsigned(value, "height", 2160);
        else if (key == "--fps" && takeValue(i, argc, argv, value)) options.fps = parseUnsigned(value, "fps", 120);
        else if (key == "--mode" && takeValue(i, argc, argv, value)) options.mode = value;
        else if (key == "--min-bitrate" && takeValue(i, argc, argv, value)) options.minBitrate = parseUnsigned(value, "minimum bitrate", 100000000);
        else if (key == "--max-bitrate" && takeValue(i, argc, argv, value)) options.maxBitrate = parseUnsigned(value, "maximum bitrate", 100000000);
        else if (key == "--initial-bitrate" && takeValue(i, argc, argv, value)) options.initialBitrate = parseUnsigned(value, "initial bitrate", 100000000);
        else if (key == "--gop" && takeValue(i, argc, argv, value)) options.initialGop = static_cast<std::uint16_t>(parseUnsigned(value, "GOP", 600));
        else if (key == "--metrics" && takeValue(i, argc, argv, value)) options.metrics = value;
        else if (key == "--synthetic") options.synthetic = true;
        else if (key == "--help") {
            std::cout << "adaptive_sender --host IP [--port 8888] [--device /dev/video0] "
                         "[--width 1920 --height 1080 --fps 25] [--mode fixed|pid|mpc] "
                         "[--min-bitrate N --max-bitrate N --initial-bitrate N --gop N] "
                         "[--metrics FILE] [--synthetic]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown or incomplete option: " << key << '\n';
            std::exit(2);
        }
    }
    if (options.host.empty() || options.port == 0 || options.width < 2 ||
        options.height < 2 || options.fps < 1 || options.minBitrate < 10000 ||
        options.maxBitrate < options.minBitrate || options.initialBitrate < options.minBitrate ||
        options.initialBitrate > options.maxBitrate || options.initialGop == 0 ||
        (options.mode != "fixed" && options.mode != "pid" && options.mode != "mpc")) {
        std::cerr << "invalid option combination; use --help for valid arguments\n";
        std::exit(2);
    }
    return options;
}

std::uint64_t steadyMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t systemMicroseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

adaptive::RawFrame syntheticFrame(int width, int height, std::uint32_t index) {
    adaptive::RawFrame frame;
    frame.captureTimestampUs = systemMicroseconds();
    frame.width = static_cast<std::uint16_t>(width);
    frame.height = static_cast<std::uint16_t>(height);
    frame.data.resize(static_cast<std::size_t>(width) * height * 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 2;
            frame.data[offset] = static_cast<std::uint8_t>((x + index * 3) % 220 + 16);
            frame.data[offset + 1] = static_cast<std::uint8_t>(96 + (y + index) % 64);
            frame.data[offset + 2] = static_cast<std::uint8_t>((y + index * 2) % 220 + 16);
            frame.data[offset + 3] = static_cast<std::uint8_t>(96 + (x + index) % 64);
        }
    }
    return frame;
}

} // namespace

int main(int argc, char **argv) {
    const Options options = parseOptions(argc, argv);
    std::signal(SIGINT, stopHandler);
    std::signal(SIGTERM, stopHandler);

    adaptive::ControllerConfig controlConfig;
    controlConfig.mode = adaptive::parseControlMode(options.mode);
    controlConfig.minBitrateBps = options.minBitrate;
    controlConfig.maxBitrateBps = options.maxBitrate;
    controlConfig.initialBitrateBps = options.initialBitrate;
    controlConfig.initialGopFrames = options.initialGop;
    adaptive::AdaptiveController controller(controlConfig);
    adaptive::EncodingDecision desired = controller.current();
    std::mutex decisionMutex;
    std::ofstream metrics(options.metrics);
    if (metrics)
        metrics << "time_us,interval_ms,throughput_bps,loss,stall_events,jitter_ms,decode_ms,e2e_ms,bitrate_bps,gop,"
                   "estimated_bandwidth_bps,queue_ms,objective,compute_us\n";
    else
        std::cerr << "warning: cannot open metrics file " << options.metrics << '\n';

    adaptive::LatestQueue<adaptive::RawFrame> frames(2);
    adaptive::V4l2Capture capture;
    std::string error;
    if (!options.synthetic &&
        !capture.open(options.device, options.width, options.height, options.fps, &error)) {
        std::cerr << "capture initialization failed: " << error << '\n';
        return 1;
    }
    const int captureWidth = options.synthetic ? (options.width & ~1) : (capture.width() & ~1);
    const int captureHeight = options.synthetic ? (options.height & ~1) : (capture.height() & ~1);
    std::thread captureThread([&] {
        std::uint32_t syntheticIndex = 0;
        auto nextFrame = std::chrono::steady_clock::now();
        while (running.load()) {
            if (options.synthetic) {
                frames.push(syntheticFrame(captureWidth, captureHeight, syntheticIndex++));
                nextFrame += std::chrono::microseconds(1000000 / options.fps);
                std::this_thread::sleep_until(nextFrame);
                continue;
            }
            adaptive::RawFrame frame;
            std::string captureError;
            if (capture.read(frame, 1000, &captureError)) frames.push(std::move(frame));
            else if (captureError != "capture timeout")
                std::cerr << "capture: " << captureError << '\n';
        }
        frames.stop();
    });

    std::uint32_t videoSequence = 0;
    while (running.load()) {
        adaptive::Socket socket;
        if (!socket.connectTo(options.host, options.port, &error)) {
            std::cerr << "connect " << options.host << ':' << options.port << ": " << error << '\n';
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::cout << "connected to " << options.host << ':' << options.port << '\n';
        {
            std::lock_guard<std::mutex> lock(decisionMutex);
            controller.reset();
            desired = controller.current();
        }
        std::atomic<bool> connected{true};
        std::thread feedbackThread([&] {
            adaptive::StreamParser parser;
            std::uint8_t buffer[4096];
            while (running.load() && connected.load()) {
                if (!socket.waitReadable(500)) continue;
                std::string receiveError;
                const long bytes = socket.receive(buffer, sizeof(buffer), &receiveError);
                if (bytes <= 0) {
                    if (bytes < 0) std::cerr << "feedback receive: " << receiveError << '\n';
                    connected.store(false);
                    break;
                }
                parser.append(buffer, static_cast<std::size_t>(bytes));
                adaptive::Message message;
                while (parser.pop(message)) {
                    if (message.type != adaptive::MessageType::Feedback) continue;
                    adaptive::FeedbackMessage feedback;
                    if (!adaptive::parseFeedback(message, feedback, &receiveError)) continue;
                    adaptive::NetworkSample sample;
                    sample.intervalSeconds = feedback.intervalMs / 1000.0;
                    const double goodput = feedback.receivedBytes * 8.0 / sample.intervalSeconds;
                    double tcpCapacity = 0.0, rttMs = 0.0;
                    if (socket.tcpMetrics(tcpCapacity, rttMs))
                        sample.throughputBps = std::max(goodput,
                            std::min(tcpCapacity, controlConfig.maxBitrateBps * 1.5));
                    else
                        sample.throughputBps = goodput;
                    const double impairedFrames = feedback.droppedFrames + feedback.stallEvents;
                    const double totalFrames = feedback.receivedFrames + impairedFrames;
                    sample.lossRatio = totalFrames > 0 ? impairedFrames / totalFrames : 0.0;
                    sample.jitterMs = feedback.jitterUs / 1000.0;
                    sample.decodeLatencyMs = feedback.decodeLatencyUs / 1000.0;
                    sample.endToEndLatencyMs = feedback.endToEndLatencyUs / 1000.0;
                    sample.receiverBufferMs = feedback.receiverBufferMs;
                    const adaptive::EncodingDecision decision = controller.update(sample);
                    {
                        std::lock_guard<std::mutex> lock(decisionMutex);
                        desired = decision;
                    }
                    if (metrics) {
                        metrics << feedback.reportTimestampUs << ',' << feedback.intervalMs << ','
                                << sample.throughputBps << ','
                                << sample.lossRatio << ',' << feedback.stallEvents << ','
                                << sample.jitterMs << ',' << sample.decodeLatencyMs << ','
                                << sample.endToEndLatencyMs << ',' << decision.bitrateBps << ','
                                << decision.gopFrames << ',' << decision.estimatedBandwidthBps << ','
                                << decision.estimatedQueueMs << ',' << decision.objective << ','
                                << decision.computeTimeUs << '\n';
                        metrics.flush();
                    }
                    std::cout << "control " << adaptive::controlModeName(controlConfig.mode)
                              << " bandwidth=" << static_cast<long long>(decision.estimatedBandwidthBps)
                              << " bitrate=" << decision.bitrateBps << " gop=" << decision.gopFrames
                              << " compute=" << decision.computeTimeUs << "us\n";
                }
            }
        });

        adaptive::H264Encoder encoder;
        adaptive::EncodingDecision applied = desired;
        if (!encoder.open(captureWidth, captureHeight, options.fps,
                          applied.bitrateBps, applied.gopFrames, &error)) {
            std::cerr << "encoder: " << error << '\n';
            connected.store(false);
            socket.close();
            feedbackThread.join();
            running.store(false);
            break;
        }
        std::uint64_t lastReconfigureMs = steadyMilliseconds();
        while (running.load() && connected.load()) {
            adaptive::RawFrame raw;
            if (!frames.waitPop(raw)) break;
            adaptive::EncodingDecision next;
            {
                std::lock_guard<std::mutex> lock(decisionMutex);
                next = desired;
            }
            const bool materiallyChanged = next.gopFrames != applied.gopFrames ||
                next.bitrateBps > applied.bitrateBps * 115 / 100 ||
                next.bitrateBps < applied.bitrateBps * 85 / 100;
            if (materiallyChanged && steadyMilliseconds() - lastReconfigureMs >= 2000) {
                if (!encoder.open(captureWidth, captureHeight, options.fps,
                                  next.bitrateBps, next.gopFrames, &error)) {
                    std::cerr << "encoder reconfiguration: " << error << '\n';
                    break;
                }
                applied = next;
                lastReconfigureMs = steadyMilliseconds();
            }
            adaptive::EncodedFrame encoded;
            if (!encoder.encode(raw, encoded, &error)) {
                std::cerr << "encode: " << error << '\n';
                continue;
            }
            adaptive::VideoMessage video;
            video.sequence = videoSequence;
            video.captureTimestampUs = raw.captureTimestampUs;
            // H.264/YUV420 requires even output dimensions. A V4L2 driver may
            // negotiate an odd raw size, while the encoder crops it down.
            video.width = static_cast<std::uint16_t>(captureWidth);
            video.height = static_cast<std::uint16_t>(captureHeight);
            video.targetBitrateBps = applied.bitrateBps;
            video.gopFrames = applied.gopFrames;
            video.framesPerSecond = static_cast<std::uint16_t>(options.fps);
            video.keyFrame = encoded.keyFrame;
            video.accessUnit = std::move(encoded.accessUnit);
            const auto wire = adaptive::serializeVideo(video);
            if (wire.empty()) {
                std::cerr << "protocol: encoded frame cannot be serialized\n";
                continue;
            }
            if (!socket.sendAll(wire.data(), wire.size(), &error)) {
                std::cerr << "video send: " << error << '\n';
                connected.store(false);
                break;
            }
            ++videoSequence;
        }
        socket.close();
        connected.store(false);
        feedbackThread.join();
        if (running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    running.store(false);
    frames.stop();
    capture.close();
    if (captureThread.joinable()) captureThread.join();
    std::cout << "stopped; capture queue dropped " << frames.dropped() << " stale frames\n";
    return 0;
}
