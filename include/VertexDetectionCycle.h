#pragma once
#include <cstdint>
#include <iterator>
#include <optional>

namespace Mus {
    // One ordered pass may span frames. New high IDs cannot extend an active
    // pass indefinitely; insertions behind the cursor are visited next pass.
    class VertexDetectionCycle {
    public:
        bool Active() const { return active; }
        void Reset() { active = false; cursor.reset(); }
        template <class Registry>
        void Begin(const Registry& registry) {
            cursor.reset();
            active = !registry.empty();
            if (active) ceiling = registry.rbegin()->first;
        }
        template <class Registry>
        auto TakeNext(Registry& registry) {
            if (!active) return registry.end();
            auto it = cursor ? registry.upper_bound(*cursor) : registry.begin();
            if (it == registry.end() || it->first > ceiling) {
                Reset();
                return registry.end();
            }
            cursor = it->first;
            auto next = std::next(it);
            if (next == registry.end() || next->first > ceiling) active = false;
            return it;
        }
    private:
        bool active = false;
        std::optional<std::uint32_t> cursor;
        std::uint32_t ceiling = 0;
    };
}
