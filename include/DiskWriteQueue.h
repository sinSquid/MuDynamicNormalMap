#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace Mus {
    // Optional work only: full/paused queues reject instead of delaying publication.
    template<class Resource>
    class DiskWriteQueue {
    public:
        using Write = std::function<void(std::uint64_t, Resource)>;
        explicit DiskWriteQueue(Write write, std::size_t limit = 8, std::uint64_t bytes = 128ull * 1024 * 1024)
            : write(std::move(write)), limit(limit), byteLimit(bytes) {}
        ~DiskWriteQueue() { Stop(); }
        DiskWriteQueue(const DiskWriteQueue&) = delete;
        DiskWriteQueue& operator=(const DiskWriteQueue&) = delete;

        bool Submit(std::uint64_t hash, Resource resource, std::uint64_t bytes) {
            std::lock_guard lock(mutex);
            if (stopped || paused || bytes > byteLimit) return false;
            for (auto& item : pending) {
                if (item.hash != hash) continue;
                if (bytes > byteLimit - activeBytes - (pendingBytes - item.bytes)) return false;
                pendingBytes = pendingBytes - item.bytes + bytes;
                item = {hash, std::move(resource), bytes};
                return true;
            }
            if (pending.size() + (active ? 1 : 0) >= limit || bytes > byteLimit - activeBytes - pendingBytes)
                return false;
            // A failed thread allocation must not strand a queued resource.
            if (!worker.joinable()) worker = std::thread([this] { Run(); });
            pending.push_back({hash, std::move(resource), bytes});
            pendingBytes += bytes;
            cv.notify_one();
            return true;
        }
        bool Busy() const {
            std::lock_guard lock(mutex);
            return active || !pending.empty();
        }
        // Caller serializes Pause/Resume/Stop with its lifecycle operations.
        void PauseAndDiscard() {
            std::unique_lock lock(mutex);
            paused = true;
            pending.clear();
            pendingBytes = 0;
            cv.wait(lock, [this] { return !active; });
        }
        void Resume() {
            std::lock_guard lock(mutex);
            paused = false;
            cv.notify_one();
        }
        void Stop() {
            {
                std::lock_guard lock(mutex);
                stopped = true;
                pending.clear();
                pendingBytes = 0;
            }
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }
    private:
        struct Item { std::uint64_t hash; Resource resource; std::uint64_t bytes; };
        void Run() {
            for (;;) {
                Item item;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this] { return stopped || (!paused && !pending.empty()); });
                    if (stopped) return;
                    item = std::move(pending.front());
                    pending.pop_front();
                    pendingBytes -= item.bytes;
                    active = true;
                    activeBytes = item.bytes;
                }
                try { write(item.hash, std::move(item.resource)); }
                catch (...) { /* Optional cache failure never kills the writer. */ }
                // Drop the last worker-owned resource before reporting idle.
                item.resource = {};
                {
                    std::lock_guard lock(mutex);
                    active = false;
                    activeBytes = 0;
                }
                cv.notify_all();
            }
        }
        Write write;
        const std::size_t limit;
        const std::uint64_t byteLimit;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::deque<Item> pending;
        std::uint64_t pendingBytes = 0, activeBytes = 0;
        bool active = false, paused = false, stopped = false;
        std::thread worker;
    };
}
