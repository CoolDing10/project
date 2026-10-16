#include "adaptive/controller.hpp"

#include <iostream>

#define REQUIRE(expression) do { if (!(expression)) { \
    std::cerr << "requirement failed at line " << __LINE__ << ": " #expression << '\n'; \
    return 1; } } while (false)

int main() {
    adaptive::ControllerConfig config;
    config.mode = adaptive::ControlMode::Mpc;
    config.minBitrateBps = 120000;
    config.maxBitrateBps = 700000;
    adaptive::AdaptiveController controller(config);

    adaptive::NetworkSample good;
    good.throughputBps = 1000000;
    good.intervalSeconds = 0.5;
    adaptive::EncodingDecision goodDecision;
    for (int i = 0; i < 8; ++i) goodDecision = controller.update(good);
    REQUIRE(goodDecision.bitrateBps >= 350000);
    REQUIRE(goodDecision.bitrateBps <= config.maxBitrateBps);

    adaptive::NetworkSample bad;
    bad.throughputBps = 170000;
    bad.lossRatio = 0.08;
    bad.jitterMs = 60;
    bad.intervalSeconds = 0.5;
    adaptive::EncodingDecision badDecision;
    for (int i = 0; i < 5; ++i) badDecision = controller.update(bad);
    REQUIRE(badDecision.bitrateBps <= 250000);
    REQUIRE(badDecision.gopFrames <= 15);
    REQUIRE(badDecision.computeTimeUs < 20000);

    adaptive::ControllerConfig fullHdConfig;
    fullHdConfig.mode = adaptive::ControlMode::Mpc;
    fullHdConfig.minBitrateBps = 800000;
    fullHdConfig.maxBitrateBps = 6000000;
    fullHdConfig.initialBitrateBps = 2500000;
    adaptive::AdaptiveController fullHdController(fullHdConfig);
    adaptive::NetworkSample fullHdNetwork;
    fullHdNetwork.throughputBps = 8000000;
    fullHdNetwork.intervalSeconds = 0.5;
    adaptive::EncodingDecision fullHdDecision;
    for (int i = 0; i < 8; ++i) fullHdDecision = fullHdController.update(fullHdNetwork);
    REQUIRE(fullHdDecision.bitrateBps >= 3000000);
    REQUIRE(fullHdDecision.bitrateBps <= fullHdConfig.maxBitrateBps);
    REQUIRE(fullHdDecision.computeTimeUs < 20000);
    std::cout << "controller tests passed\n";
}
