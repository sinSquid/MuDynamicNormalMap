#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <unordered_set>
#include <vector>

namespace Mus {
    // Tickets may outlive the owning TaskManager during static destruction.
    // Keep their synchronization and actor state independently owned.
    class ActorWorkState {
    public:
        bool Any() const {
            std::lock_guard lock(mutex);
            return !actors.empty();
        }
        bool Contains(std::uint32_t id) const {
            std::lock_guard lock(mutex);
            return actors.find(id) != actors.end();
        }
        void Set(std::uint32_t id, bool busy) {
            std::lock_guard lock(mutex);
            if (busy) actors.insert(id);
            else actors.erase(id);
        }
        void Clear() {
            std::lock_guard lock(mutex);
            actors.clear();
        }
        static std::shared_ptr<void> Claim(const std::shared_ptr<ActorWorkState>& state, std::uint32_t id) {
            auto ticket = std::shared_ptr<void>(nullptr, [state, id](void*) { state->Set(id, false); });
            state->Set(id, true);
            return ticket;
        }
    private:
        mutable std::mutex mutex;
        std::unordered_set<std::uint32_t> actors;
    };

    // A delay of N preserves the old N wrappers + one execution-pump semantics.
    // Callbacks are detached under the lock and invoked by the caller without it.
    class FrameTaskQueue {
    public:
        void Enqueue(std::int16_t delay, std::function<void()> callback) {
            std::lock_guard lock(mutex);
            const auto ticks = delay > 0 ? static_cast<std::uint64_t>(delay) : 0;
            pending[frame + ticks + 1].push_back(std::move(callback));
        }

        std::vector<std::function<void()>> TakeReady() {
            std::vector<std::function<void()>> ready;
            std::lock_guard lock(mutex);
            ++frame;
            const auto end = pending.upper_bound(frame);
            std::size_t count = 0;
            for (auto it = pending.begin(); it != end; ++it)
                count += it->second.size();
            ready.reserve(count);
            for (auto it = pending.begin(); it != end; ++it) {
                auto& callbacks = it->second;
                ready.insert(ready.end(), std::make_move_iterator(callbacks.begin()),
                    std::make_move_iterator(callbacks.end()));
            }
            pending.erase(pending.begin(), end);
            return ready;
        }

    private:
        std::mutex mutex;
        std::uint64_t frame = 0;
        std::map<std::uint64_t, std::vector<std::function<void()>>> pending;
    };

    class SingleFlightGate {
    public:
        class Ticket {
        public:
            explicit Ticket(std::atomic<bool>& flag) : busy(flag) {}
            ~Ticket() { busy.store(false); }
            Ticket(const Ticket&) = delete;
            Ticket& operator=(const Ticket&) = delete;
        private:
            std::atomic<bool>& busy;
        };

        bool Busy() const { return busy.load(); }
        std::shared_ptr<Ticket> TryAcquire() {
            bool expected = false;
            if (!busy.compare_exchange_strong(expected, true))
                return {};
            try {
                return std::make_shared<Ticket>(busy);
            } catch (...) {
                busy.store(false);
                throw;
            }
        }
    private:
        std::atomic<bool> busy = false;
    };
}
