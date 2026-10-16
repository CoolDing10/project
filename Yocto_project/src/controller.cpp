#include "adaptive/controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace adaptive {
namespace {

double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

std::uint32_t quantizeBitrate(double value, const ControllerConfig &config) {
    static const double ladder[] = {120000, 180000, 250000, 350000,
                                    500000, 700000, 900000, 1200000,
                                    1600000, 2200000, 3000000, 4500000,
                                    6000000, 8000000, 10000000, 12000000};
    std::uint32_t best = config.minBitrateBps;
    double distance = std::numeric_limits<double>::max();
    for (double candidate : ladder) {
        if (candidate < config.minBitrateBps || candidate > config.maxBitrateBps) continue;
        const double d = std::abs(candidate - value);
        if (d < distance) {
            distance = d;
            best = static_cast<std::uint32_t>(candidate);
        }
    }
    return best;
}

std::vector<std::uint32_t> bitrateCandidates(const ControllerConfig &config) {
    static const std::uint32_t ladder[] = {120000, 180000, 250000, 350000,
                                          500000, 700000, 900000, 1200000,
                                          1600000, 2200000, 3000000, 4500000,
                                          6000000, 8000000, 10000000, 12000000};
    std::vector<std::uint32_t> result;
    for (auto value : ladder)
        if (value >= config.minBitrateBps && value <= config.maxBitrateBps)
            result.push_back(value);
    if (result.empty()) result.push_back(config.minBitrateBps);
    return result;
}

} // namespace

AdaptiveController::AdaptiveController(const ControllerConfig &config) : config_(config) {
    if (config_.minBitrateBps == 0) config_.minBitrateBps = 1;
    if (config_.maxBitrateBps < config_.minBitrateBps)
        config_.maxBitrateBps = config_.minBitrateBps;
    config_.initialBitrateBps = std::max(config_.minBitrateBps,
        std::min(config_.maxBitrateBps, config_.initialBitrateBps));
    config_.horizon = std::max<std::uint32_t>(1, std::min<std::uint32_t>(8, config_.horizon));
    config_.beamWidth = std::max<std::uint32_t>(4, std::min<std::uint32_t>(128, config_.beamWidth));
    reset();
}

void AdaptiveController::reset() {
    current_ = EncodingDecision{};
    current_.bitrateBps = config_.initialBitrateBps;
    current_.gopFrames = config_.initialGopFrames;
    fastBandwidthBps_ = slowBandwidthBps_ = 0.0;
    queueSeconds_ = pidIntegral_ = pidPreviousError_ = 0.0;
}

void AdaptiveController::updateEstimator(const NetworkSample &sample) {
    if (sample.throughputBps > 0.0) {
        if (fastBandwidthBps_ == 0.0) fastBandwidthBps_ = sample.throughputBps;
        if (slowBandwidthBps_ == 0.0) slowBandwidthBps_ = sample.throughputBps;
        fastBandwidthBps_ = 0.45 * sample.throughputBps + 0.55 * fastBandwidthBps_;
        slowBandwidthBps_ = 0.12 * sample.throughputBps + 0.88 * slowBandwidthBps_;
    }
    const double bandwidth = std::max(1.0, std::min(fastBandwidthBps_, slowBandwidthBps_));
    const double interval = clamp(sample.intervalSeconds, 0.05, 5.0);
    queueSeconds_ = clamp(queueSeconds_ +
        (static_cast<double>(current_.bitrateBps) / bandwidth - 0.90) * interval,
        0.0, 3.0);
    queueSeconds_ = std::max(queueSeconds_, sample.receiverBufferMs / 1000.0);
}

EncodingDecision AdaptiveController::update(const NetworkSample &sample) {
    const auto begin = std::chrono::steady_clock::now();
    updateEstimator(sample);
    EncodingDecision result;
    switch (config_.mode) {
    case ControlMode::Fixed: result = updateFixed(); break;
    case ControlMode::Pid: result = updatePid(sample); break;
    case ControlMode::Mpc: result = updateMpc(sample); break;
    }
    result.estimatedBandwidthBps = std::max(0.0, std::min(fastBandwidthBps_, slowBandwidthBps_));
    result.estimatedQueueMs = queueSeconds_ * 1000.0;
    result.computeTimeUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count());
    current_ = result;
    return current_;
}

EncodingDecision AdaptiveController::updateFixed() {
    EncodingDecision result = current_;
    result.objective = 0.0;
    return result;
}

EncodingDecision AdaptiveController::updatePid(const NetworkSample &sample) {
    constexpr double targetQueueSeconds = 0.080;
    const double dt = clamp(sample.intervalSeconds, 0.05, 5.0);
    const double error = targetQueueSeconds - queueSeconds_;
    pidIntegral_ = clamp(pidIntegral_ + error * dt, -0.5, 0.5);
    const double derivative = (error - pidPreviousError_) / dt;
    pidPreviousError_ = error;
    const double capacity = std::max<double>(config_.minBitrateBps,
        std::min(fastBandwidthBps_, slowBandwidthBps_));
    double target = current_.bitrateBps + 1.3 * capacity * error +
                    0.20 * capacity * pidIntegral_ + 0.02 * capacity * derivative;
    target *= (1.0 - clamp(sample.lossRatio * 3.0, 0.0, 0.45));
    target = std::min(target, capacity * 0.90);
    EncodingDecision result = current_;
    result.bitrateBps = quantizeBitrate(target, config_);
    result.gopFrames = sample.lossRatio > 0.03 ? 15 : 30;
    result.objective = queueSeconds_ * queueSeconds_;
    return result;
}

EncodingDecision AdaptiveController::updateMpc(const NetworkSample &sample) {
    struct Node {
        double cost = 0.0;
        double queue = 0.0;
        std::uint32_t previousBitrate = 0;
        std::uint32_t firstBitrate = 0;
        std::uint16_t firstGop = 30;
    };
    const auto rates = bitrateCandidates(config_);
    static const std::uint16_t gops[] = {10, 15, 30, 60};
    const double baseBandwidth = std::max<double>(config_.minBitrateBps,
        std::min(fastBandwidthBps_, slowBandwidthBps_));
    const double uncertainty = clamp(0.08 + sample.lossRatio * 2.5 +
                                     sample.jitterMs / 250.0, 0.08, 0.45);
    const double predictedBandwidth = baseBandwidth * (1.0 - uncertainty);
    const double dt = clamp(sample.intervalSeconds, 0.10, 2.0);
    std::vector<Node> beam(1);
    beam[0].queue = queueSeconds_;
    beam[0].previousBitrate = current_.bitrateBps;

    for (std::uint32_t step = 0; step < config_.horizon; ++step) {
        std::vector<Node> expanded;
        expanded.reserve(beam.size() * rates.size() * 4);
        for (const Node &node : beam) {
            for (std::uint32_t rate : rates) {
                for (std::uint16_t gop : gops) {
                    Node next = node;
                    const double load = static_cast<double>(rate) / predictedBandwidth;
                    next.queue = clamp(node.queue + (load - 0.88) * dt, 0.0, 3.0);
                    const double quality = std::log(static_cast<double>(rate) /
                                                    config_.minBitrateBps);
                    const double change = std::log(static_cast<double>(rate) /
                                                   std::max<std::uint32_t>(1, node.previousBitrate));
                    const double overflow = std::max(0.0, load - 1.0);
                    const double lossRisk = sample.lossRatio * (gop / 15.0) * load;
                    const double keyframeOverhead = 0.55 * 30.0 / gop;
                    next.cost += 65.0 * next.queue * next.queue +
                                 35.0 * overflow * overflow +
                                 4.0 * change * change +
                                 18.0 * lossRisk + keyframeOverhead - 1.7 * quality;
                    next.previousBitrate = rate;
                    if (step == 0) {
                        next.firstBitrate = rate;
                        next.firstGop = gop;
                    }
                    expanded.push_back(next);
                }
            }
        }
        const std::size_t keep = std::min<std::size_t>(config_.beamWidth, expanded.size());
        std::partial_sort(expanded.begin(), expanded.begin() + keep, expanded.end(),
                          [](const Node &a, const Node &b) { return a.cost < b.cost; });
        expanded.resize(keep);
        beam.swap(expanded);
    }
    const Node &best = beam.front();
    EncodingDecision result = current_;
    result.bitrateBps = best.firstBitrate;
    result.gopFrames = best.firstGop;
    result.objective = best.cost;
    return result;
}

ControlMode parseControlMode(const std::string &text) {
    if (text == "fixed") return ControlMode::Fixed;
    if (text == "pid") return ControlMode::Pid;
    return ControlMode::Mpc;
}

const char *controlModeName(ControlMode mode) {
    switch (mode) {
    case ControlMode::Fixed: return "fixed";
    case ControlMode::Pid: return "pid";
    case ControlMode::Mpc: return "mpc";
    }
    return "unknown";
}

} // namespace adaptive
