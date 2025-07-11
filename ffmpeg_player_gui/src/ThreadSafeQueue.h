#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>

// 线程安全队列模板
// 用于音/视频包队列、帧队列

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue(size_t maxSize = 100) : maxSize_(maxSize) {}

    void push(const T& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_not_full_.wait(lock, [this]{ return queue_.size() < maxSize_ || stopped_; });
        if (stopped_) return;
        queue_.push(value);
        cond_not_empty_.notify_one();
    }

    bool try_pop(T& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop();
        cond_not_full_.notify_one();
        return true;
    }

    void pop(T& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_not_empty_.wait(lock, [this]{ return !queue_.empty() || stopped_; });
        if (stopped_ || queue_.empty()) return;
        value = std::move(queue_.front());
        queue_.pop();
        cond_not_full_.notify_one();
    }

    void clear() {
        std::unique_lock<std::mutex> lock(mtx_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        cond_not_full_.notify_all();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }

    void wakeAll() {
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
    std::queue<T> queue_;
    size_t maxSize_;
    bool stopped_ = false;
}; 