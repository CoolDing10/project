#ifndef ADAPTIVE_LATEST_QUEUE_HPP
#define ADAPTIVE_LATEST_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace adaptive {

// Bounded queue for live video. When the encoder falls behind, stale frames are
// discarded instead of increasing latency without bound.
template <typename T>
class LatestQueue {
public:
    explicit LatestQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        while (queue_.size() >= capacity_) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(value));
        condition_.notify_one();
    }

    bool waitPop(T &value) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        condition_.notify_all();
    }

    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T> queue_;
    std::uint64_t dropped_ = 0;
    bool stopped_ = false;
};

} // namespace adaptive

#endif
