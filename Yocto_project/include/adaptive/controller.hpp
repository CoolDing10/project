#ifndef ADAPTIVE_CONTROLLER_HPP
#define ADAPTIVE_CONTROLLER_HPP

#include <cstdint>
#include <string>

namespace adaptive {

enum class ControlMode { Fixed, Pid, Mpc };

struct ControllerConfig {
    ControlMode mode = ControlMode::Mpc;
    std::uint32_t minBitrateBps = 800000;
    std::uint32_t maxBitrateBps = 6000000;
    std::uint32_t initialBitrateBps = 2500000;
    std::uint16_t initialGopFrames = 50;
    std::uint32_t controlIntervalMs = 500;
    std::uint32_t horizon = 5;
    std::uint32_t beamWidth = 24;
};

struct NetworkSample {
    double throughputBps = 0.0;
    double lossRatio = 0.0;
    double jitterMs = 0.0;
    double decodeLatencyMs = 0.0;
    double endToEndLatencyMs = 0.0;
    double receiverBufferMs = 0.0;
    double intervalSeconds = 0.5;
};

struct EncodingDecision {
    std::uint32_t bitrateBps = 2500000;
    std::uint16_t gopFrames = 50;
    double estimatedBandwidthBps = 0.0;
    double estimatedQueueMs = 0.0;
    double objective = 0.0;
    std::uint64_t computeTimeUs = 0;
};

class AdaptiveController {
public:
    explicit AdaptiveController(const ControllerConfig &config = ControllerConfig());
    EncodingDecision update(const NetworkSample &sample);
    EncodingDecision current() const { return current_; }
    void reset();

private:
    EncodingDecision updateFixed();
    EncodingDecision updatePid(const NetworkSample &sample);
    EncodingDecision updateMpc(const NetworkSample &sample);
    void updateEstimator(const NetworkSample &sample);

    ControllerConfig config_;
    EncodingDecision current_;
    double fastBandwidthBps_ = 0.0;
    double slowBandwidthBps_ = 0.0;
    double queueSeconds_ = 0.0;
    double pidIntegral_ = 0.0;
    double pidPreviousError_ = 0.0;
};

ControlMode parseControlMode(const std::string &text);
const char *controlModeName(ControlMode mode);

} // namespace adaptive

#endif
