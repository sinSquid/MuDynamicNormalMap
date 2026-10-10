#include <cassert>
#include <map>
#include <set>
#include <vector>

int main() {
    for (const auto& invalid : std::vector<std::set<unsigned>>{
        {}, {1}, {2}, {3}, {1, 2, 3}}) {
        std::map<unsigned, bool> active{{1, true}, {2, true}, {3, true}};
        std::set<unsigned> removed;
        for (auto it = active.begin(); it != active.end();) {
            if (invalid.contains(it->first)) {
                const auto actorID = it->first;
                it = active.erase(it);
                removed.insert(actorID);
                continue;
            }
            ++it;
        }
        assert(removed == invalid);
        assert(active.size() == 3 - invalid.size());
    }
}
