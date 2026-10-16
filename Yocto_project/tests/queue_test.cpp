#include "adaptive/latest_queue.hpp"

#include <iostream>

#define REQUIRE(expression) do { if (!(expression)) { \
    std::cerr << "requirement failed at line " << __LINE__ << ": " #expression << '\n'; \
    return 1; } } while (false)

int main() {
    adaptive::LatestQueue<int> queue(2);
    queue.push(1);
    queue.push(2);
    queue.push(3);
    int value = 0;
    REQUIRE(queue.waitPop(value) && value == 2);
    REQUIRE(queue.waitPop(value) && value == 3);
    REQUIRE(queue.dropped() == 1);
    queue.stop();
    REQUIRE(!queue.waitPop(value));
    std::cout << "queue tests passed\n";
}
