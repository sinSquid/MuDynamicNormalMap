#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Mus {
// Session-only negative cache. Invalidating model events explicitly remove entries.
class GeometryRetry {
public:
    using Clock = std::chrono::steady_clock;
    enum class State { Ready, CoolingDown, Blacklisted };
    State Check(std::uint32_t id, std::uintptr_t root, std::uint32_t race,
                Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex);
        auto it = entries.find(id);
        if (it == entries.end()) return State::Ready;
        if (it->second.root != root || it->second.race != race) {
            entries.erase(it);
            return State::Ready;
        }
        if (it->second.failures >= 3) return State::Blacklisted;
        // Expiration permits the next probe but must retain the failure count.
        return now < it->second.until ? State::CoolingDown : State::Ready;
    }
    bool Blocked(std::uint32_t id, std::uintptr_t root, std::uint32_t race,
                 Clock::time_point now = Clock::now()) {
        return Check(id, root, race, now) != State::Ready;
    }
    bool Failed(std::uint32_t id, std::uintptr_t root, std::uint32_t race,
                Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex);
        auto found = entries.find(id);
        if (found != entries.end() && found->second.root == root && found->second.race == race) {
            auto& entry = found->second;
            // Repeated calls in the same cooldown must not count as new probes.
            if (entry.failures >= 3) return true;
            if (now >= entry.until) {
                ++entry.failures;
                entry.until = now + std::chrono::seconds(5);
            }
            return entry.failures >= 3;
        }
        if (!entries.contains(id) && entries.size() >= 512) {
            auto oldest = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.until < oldest->second.until) oldest = it;
            entries.erase(oldest);
        }
        entries[id] = {root, race, now + std::chrono::seconds(5), 1};
        return false;
    }
    void Forget(std::uint32_t id) {
        std::lock_guard lock(mutex);
        entries.erase(id);
    }
    void Clear() {
        std::lock_guard lock(mutex);
        entries.clear();
    }
private:
    struct Entry { std::uintptr_t root; std::uint32_t race; Clock::time_point until; unsigned failures; };
    std::mutex mutex;
    std::unordered_map<std::uint32_t, Entry> entries;
};
}
