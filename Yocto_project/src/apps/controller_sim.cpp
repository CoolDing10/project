#include "adaptive/controller.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

adaptive::NetworkSample generatedSample(int step) {
    adaptive::NetworkSample sample;
    const double seconds = step * 0.5;
    if (seconds < 15.0) sample.throughputBps = 8000000;
    else if (seconds < 30.0) sample.throughputBps = 1500000;
    else if (seconds < 45.0) sample.throughputBps = 5000000;
    else sample.throughputBps = 3500000 + 2000000 * std::sin(seconds * 0.7);
    sample.lossRatio = sample.throughputBps < 2000000 ? 0.045 : 0.005;
    sample.jitterMs = sample.throughputBps < 2000000 ? 38.0 : 8.0;
    sample.decodeLatencyMs = 12.0;
    sample.intervalSeconds = 0.5;
    return sample;
}

} // namespace

int main(int argc, char **argv) {
    const std::string output = argc > 1 ? argv[1] : "controller_metrics.csv";
    std::ofstream csv(output);
    if (!csv) {
        std::cerr << "cannot create " << output << '\n';
        return 1;
    }
    adaptive::ControllerConfig pidConfig;
    pidConfig.mode = adaptive::ControlMode::Pid;
    pidConfig.minBitrateBps = 800000;
    pidConfig.maxBitrateBps = 6000000;
    pidConfig.initialBitrateBps = 2500000;
    pidConfig.initialGopFrames = 50;
    adaptive::ControllerConfig mpcConfig;
    mpcConfig.mode = adaptive::ControlMode::Mpc;
    mpcConfig.minBitrateBps = 800000;
    mpcConfig.maxBitrateBps = 6000000;
    mpcConfig.initialBitrateBps = 2500000;
    mpcConfig.initialGopFrames = 50;
    adaptive::AdaptiveController pid(pidConfig), mpc(mpcConfig);
    csv << "time_s,throughput_bps,loss,pid_bitrate,pid_gop,pid_queue_ms,pid_compute_us,"
           "mpc_bitrate,mpc_gop,mpc_queue_ms,mpc_compute_us\n";
    std::uint64_t maxMpcUs = 0;
    for (int step = 0; step < 120; ++step) {
        const auto sample = generatedSample(step);
        const auto pidDecision = pid.update(sample);
        const auto mpcDecision = mpc.update(sample);
        maxMpcUs = std::max(maxMpcUs, mpcDecision.computeTimeUs);
        csv << std::fixed << std::setprecision(3) << step * 0.5 << ','
            << sample.throughputBps << ',' << sample.lossRatio << ','
            << pidDecision.bitrateBps << ',' << pidDecision.gopFrames << ','
            << pidDecision.estimatedQueueMs << ',' << pidDecision.computeTimeUs << ','
            << mpcDecision.bitrateBps << ',' << mpcDecision.gopFrames << ','
            << mpcDecision.estimatedQueueMs << ',' << mpcDecision.computeTimeUs << '\n';
    }
    std::cout << "wrote " << output << ", max MPC compute time: " << maxMpcUs << " us\n";
    if (maxMpcUs > 20000) {
        std::cerr << "MPC computation exceeded the 20 ms project target\n";
        return 1;
    }
    return 0;
}
