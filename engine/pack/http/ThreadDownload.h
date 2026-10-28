// Shared state of a background transfer (native fetchers): written by the worker thread, read by
// ChunkManager::poll() on the main thread.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "../Chunks.h"

namespace rts::pack {

class ThreadDownload : public Download {
public:
    ~ThreadDownload() override { if (worker_.joinable()) worker_.join(); }
    bool done() const override { return done_.load(); }
    bool ok() const override { return ok_.load(); }
    uint64_t bytes() const override { return bytes_.load(); }
    std::string error() const override { std::lock_guard<std::mutex> l(m_); return error_; }

    template <class F> void run(F&& body) { worker_ = std::thread([this, b = std::forward<F>(body)]() mutable { b(*this); done_ = true; }); }
    void succeed() { ok_ = true; }
    void fail(const std::string& e) { std::lock_guard<std::mutex> l(m_); error_ = e; ok_ = false; }
    std::atomic<uint64_t> bytes_{0};

private:
    std::atomic<bool> done_{false}, ok_{false};
    mutable std::mutex m_;
    std::string error_;
    std::thread worker_;
};

} // namespace rts::pack
