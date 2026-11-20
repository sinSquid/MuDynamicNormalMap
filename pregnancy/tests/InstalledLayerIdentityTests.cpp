#include "Rendering/InstalledLayerIdentity.h"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace PNS::Rendering;
namespace {
    void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    struct Wrapper { std::uintptr_t texture, view; };
}
int main() {
    auto muCache = std::make_shared<Wrapper>(Wrapper{100, 101});
    auto base = std::make_shared<Wrapper>(*muCache);
    auto pregnancy = std::make_shared<Wrapper>(Wrapper{200, 201});
    auto normal = pregnancy;
    auto identity = [&] {
        return InstalledLayerIdentity{1, 2, reinterpret_cast<std::uintptr_t>(normal.get()), normal->view, 3, "oskin/a/body.dds"};
    };
    const auto installed = identity();
    int restored = 0;
    auto tryRestore = [&](const InstalledLayerIdentity& expected, const InstalledLayerIdentity& current) {
        if (!OwnsInstalledLayer(expected, current)) return false;
        normal = base; ++restored;
        return true;
    };
    muCache->texture = 300; muCache->view = 301;
    Check(tryRestore(installed, identity()), "owned layer did not restore");
    Check(normal == base && base->texture == 100 && base->view == 101, "saved base changed with cache");
    Check(muCache->texture == 300 && muCache->view == 301, "restoration mutated shared wrapper");
    Check(!tryRestore(installed, identity()) && restored == 1, "double restoration");
    normal = pregnancy;
    pregnancy->view = 202;
    Check(!tryRestore(installed, identity()), "same wrapper with external SRV overwritten");
    pregnancy->view = 201;
    auto externalMaterial = identity(); externalMaterial.material = 9;
    Check(!tryRestore(installed, externalMaterial), "external material overwritten");
    auto externalPath = identity(); externalPath.normalPath = "oskin/b/body.dds";
    Check(!tryRestore(installed, externalPath), "changed source overwritten");
    auto externalSet = identity(); externalSet.textureSet = 10;
    Check(!tryRestore(installed, externalSet), "external texture set overwritten");
    auto otherGeometry = identity(); otherGeometry.geometry = 10;
    Check(!tryRestore(installed, otherGeometry), "different geometry overwritten");
    normal = std::make_shared<Wrapper>(Wrapper{200, 201});
    Check(!tryRestore(installed, identity()), "same SRV in external wrapper overwritten");
    const auto replacement = identity();
    Check(tryRestore(replacement, identity()) && restored == 2, "repeated installation not restorable");
    Check(!OwnsInstalledLayer({}, {}), "empty layer treated as installed");
    // External normal replacement does not transfer ownership of our debug
    // diffuse. A subsequent private material clone must preserve that distinction.
    auto externalNormal = installed;
    externalNormal.texture = 900;
    externalNormal.view = 901;
    Check(!OwnsInstalledLayer(installed, externalNormal), "foreign normal claimed");
    Check(OwnsDebugDiffuse(installed, externalNormal, installed.texture, installed.view),
        "own debug diffuse lost when normal changed");
    auto clonedRecord = installed;
    auto clonedCurrent = externalNormal;
    clonedRecord.material = clonedCurrent.material = 50;
    Check(!OwnsInstalledLayer(clonedRecord, clonedCurrent), "clone claimed foreign normal");
    Check(OwnsDebugDiffuse(clonedRecord, clonedCurrent, installed.texture, installed.view),
        "clone lost independent debug ownership");
    Check(!OwnsDebugDiffuse(clonedRecord, clonedCurrent, 800, installed.view), "foreign diffuse wrapper claimed");
    Check(!OwnsDebugDiffuse(clonedRecord, clonedCurrent, installed.texture, 801), "foreign diffuse SRV claimed");
    auto changedSkin = clonedCurrent;
    changedSkin.normalPath = "oskin/b/body.dds";
    Check(!OwnsDebugDiffuse(clonedRecord, changedSkin, installed.texture, installed.view), "external skin overwritten by debug restore");
    // The saved diffuse is a distinct pre-debug resource, never the base normal.
    auto originalDiffuse = std::make_shared<Wrapper>(Wrapper{400, 401});
    auto diffuse = pregnancy;
    if (OwnsDebugDiffuse(clonedRecord, clonedCurrent, installed.texture, installed.view))
        diffuse = originalDiffuse;
    Check(diffuse == originalDiffuse && diffuse != base && clonedCurrent.texture == 900,
        "debug restore replaced the original diffuse or foreign normal");
    std::cout << "PASS: ownership, shared-wrapper isolation, external takeovers, repeated/zero restoration, independent debug diffuse\n";
}
