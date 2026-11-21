#include "../include/VertexDetectionCycle.h"
#include <cassert>
#include <map>
#include <vector>

int main() {
    Mus::VertexDetectionCycle cycle;
    std::map<std::uint32_t, int> actors{{10, 0}, {20, 0}, {30, 0}};
    cycle.Begin(actors);
    std::vector<std::uint32_t> seen;
    for (int budget = 0; budget < 4 && cycle.Active(); ++budget)
        seen.push_back(cycle.TakeNext(actors)->first);
    assert((seen == std::vector<std::uint32_t>{10, 20, 30}));
    assert(!cycle.Active());

    cycle.Begin(actors);
    auto first = cycle.TakeNext(actors);
    assert(first->first == 10);
    actors.erase(first);
    actors.emplace(5, 0); // Behind cursor: next pass.
    actors.emplace(15, 0);
    actors.emplace(40, 0); // Above fixed ceiling: next pass.
    assert(cycle.TakeNext(actors)->first == 15);
    // Pause does not consume entries; resume continues at the next ID.
    assert(cycle.Active());
    assert(cycle.TakeNext(actors)->first == 20);
    actors.erase(30); // Removing the end boundary cannot prolong this pass.
    assert(cycle.TakeNext(actors) == actors.end());
    assert(!cycle.Active());
    cycle.Begin(actors);
    seen.clear();
    while (cycle.Active()) {
        auto it = cycle.TakeNext(actors);
        if (it != actors.end()) seen.push_back(it->first);
    }
    assert((seen == std::vector<std::uint32_t>{5, 15, 20, 40}));

    // Continuous high-ID registrations cannot starve completion.
    actors.clear();
    for (std::uint32_t id = 1; id <= 13; ++id) actors.emplace(id, 0);
    cycle.Begin(actors);
    seen.clear();
    unsigned frames = 0;
    while (cycle.Active()) {
        ++frames;
        for (int budget = 0; budget < 4 && cycle.Active(); ++budget) {
            auto it = cycle.TakeNext(actors);
            assert(it != actors.end());
            seen.push_back(it->first);
        }
        actors.emplace(100 + frames, 0);
        assert(frames <= 4);
    }
    assert(seen.size() == 13 && seen.front() == 1 && seen.back() == 13 && frames == 4);

    cycle.Begin(actors);
    assert(cycle.TakeNext(actors)->first == 1);
    cycle.Reset(); // Loading/config rebuild abandons the old cursor.
    actors = {{2, 0}, {3, 0}};
    assert(cycle.TakeNext(actors) == actors.end());
    cycle.Begin(actors);
    assert(cycle.TakeNext(actors)->first == 2);
    assert(cycle.TakeNext(actors)->first == 3 && !cycle.Active());
    actors.clear();
    cycle.Begin(actors);
    assert(!cycle.Active() && cycle.TakeNext(actors) == actors.end());
}
