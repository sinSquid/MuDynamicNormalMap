#include "Compat/MaterialAssignment.h"
#include <stdexcept>
#include <iostream>
using namespace PNS::OSkin;
namespace {
    void Check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}
int main() {
    const MaterialAssignment original{1, 10, "oskin/a/body.dds"};
    const MaterialAssignment fastReattach{1, 11, "oskin/a/body.dds"};
    const MaterialAssignment completion{1, 12, "oskin/a/body.dds"};
    std::vector<MaterialAssignment> observed{original};
    // The fast cache reattach and the async result both clone a material. Neither
    // should look like a fresh OSkin assignment at the next provider poll.
    AcknowledgeClone(observed, original, fastReattach);
    Check(observed == std::vector{fastReattach}, "fast reattach retriggers OSkin");
    AcknowledgeClone(observed, fastReattach, completion);
    Check(observed == std::vector{completion}, "completion retriggers OSkin");
    for (std::uintptr_t material = 13; material != 30; ++material) {
        const auto source = observed.front();
        const MaterialAssignment clone{1, material, "oskin/a/body.dds"};
        AcknowledgeClone(observed, source, clone);
        Check(observed == std::vector{clone}, "repeated own clone feedback loop");
    }
    const auto stable = observed;
    // A real OSkin material swap, including diffuse-only (same normal path),
    // must not be hidden just because Mu clones it before the next poll.
    const MaterialAssignment externalSameNormal{1, 40, "oskin/a/body.dds"};
    AcknowledgeClone(observed, externalSameNormal, {1, 41, "oskin/a/body.dds"});
    Check(observed == stable && observed != std::vector{externalSameNormal}, "external swap hidden");
    AcknowledgeClone(observed, stable.front(), {1, 42, "oskin/b/body.dds"});
    Check(observed == stable, "changed skin source hidden");
    AcknowledgeClone(observed, {1, std::get<1>(stable.front()), "oskin/b/body.dds"},
        {1, 43, "oskin/b/body.dds"});
    Check(observed == stable, "unobserved path change hidden");
    AcknowledgeClone(observed, stable.front(), {2, 44, "oskin/a/body.dds"});
    Check(observed == stable, "different geometry accepted");
    std::vector<MaterialAssignment> otherActor{{2, 10, "oskin/a/body.dds"}};
    AcknowledgeClone(otherActor, original, fastReattach);
    Check(otherActor == std::vector<MaterialAssignment>{{2, 10, "oskin/a/body.dds"}}, "actor isolation");
    std::vector<MaterialAssignment> unobserved;
    AcknowledgeClone(unobserved, original, fastReattach);
    Check(unobserved.empty(), "first provider observation suppressed");
    std::cout << "PASS: own clones stable; external swaps, changed paths, first observation and actors remain distinct\n";
}
