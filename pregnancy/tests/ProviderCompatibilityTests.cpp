#include "Compat/ProviderMorphs.h"
#include "Compat/SkinPaths.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace PNS::ProviderMorphs;

void Check(bool passed, const char* description) {
    if (!passed) {
        std::cerr << description << '\n';
        std::exit(EXIT_FAILURE);
    }
}

int main() {
    using namespace PNS::SkinPaths;
    Check(!PregnancySource("ABBA.esp",false,{},{}) && !PregnancySource("ArousedBodyMorphs.esp",false,{},{}) &&
          !PregnancySource("DinoRMF_Preview",false,{},{}) && !PregnancySource("OBody",false,{},{}),
          "Cosmetic and preview morph values alone cannot mark an actor pregnant");
    Check(PregnancySource("ABBA.esp",false,{"ABBA.esp"},{}) && !PregnancySource("BodyGen",false,{"PregMod"},{}) &&
          !PregnancySource("ABBA.esp",false,{"ABBA.esp"},{"ABBA.esp"}),"Explicit pregnancy sources and ignore priority");
    Check(!Stale("Data/Textures/OSkin/A/body.dds","oskin\\a\\BODY.dds"),"Case and game path prefixes match");
    Check(Stale("textures/oskin/A/body.dds","textures/oskin/B/body.dds"),"A pending result cannot cross skin packs");
    Check(Stale("OSkin/A/body.dds","actors/body.dds"),"Game Skin restoration rejects an older pack result");
    Check(!IsPackPath("textures/oskin-other/body.dds"),"Similar directory names are independent");
    const Providers all{.obody = true, .abba = true, .abm = true};
    Snapshot empty, preset, afterRefit;
    preset.Add("PregnancyBelly", "OBody", 0.7f, all);
    Check(preset.ChangedSlots(empty) == allSlots, "Applying a body preset refreshes enabled slots");
    afterRefit = preset;
    afterRefit.Add("Chest", "OClothe", -0.25f, all);
    Check(afterRefit.ChangedSlots(preset) == allSlots, "An outfit-only refit is detectable");
    Check(empty.ChangedSlots(afterRefit) == allSlots, "Removing a preset restores geometry normals");
    Check(IsPresetKey("OBody") && IsPresetKey("OClothe") && !IsPresetKey("PregnancyMod.esp"),
          "Pregnancy filtering recognizes only cosmetic preset keys");

    Snapshot first, reordered, swapped;
    first.Add("NipplePerkiness", "ABBA.esp", 0.25f, all);
    first.Add("NipplePerkiness", "ArousedBodyMorphs.esp", 0.75f, all);
    Check(first.ChangedSlots(empty) == bodySlots, "Both adjusters can own the same slider");
    reordered.Add("NipplePerkiness", "ArousedBodyMorphs.esp", 0.75f, all);
    reordered.Add("NipplePerkiness", "ABBA.esp", 0.25f, all);
    Check(reordered.ChangedSlots(first) == 0, "An unchanged actor does not refresh on another poll");
    swapped.Add("NipplePerkiness", "ArousedBodyMorphs.esp", 0.25f, all);
    swapped.Add("NipplePerkiness", "ABBA.esp", 0.75f, all);
    Check(swapped.ChangedSlots(first) == bodySlots, "Per-provider changes survive equal totals");

    const Providers abmOnly{.abm = true};
    Snapshot filtered, patched;
    filtered.Add("NippleErection", "ABBA.esp", 1, abmOnly);
    filtered.Add("Chest", "OBody", 1, abmOnly);
    filtered.Add("DoubleMelon", "ArousedBodyMorphs.esp", 0, abmOnly);
    filtered.Add("BadSlider", "ArousedBodyMorphs.esp", std::numeric_limits<float>::quiet_NaN(), abmOnly);
    Check(filtered.Empty(), "Inactive sources, zero and invalid values are ignored");
    patched.Add("NipplePuffyAreola", "ArousedBodyMorphs.esp", 0.4f, abmOnly);
    patched.Add("CustomImportedSlider", "ArousedBodyMorphs.esp", -0.2f, abmOnly);
    Check(patched.ChangedSlots(empty) == bodySlots && patched.adjustment.size() == 2,
          "The BHUNP table and custom slider names require no compiled allowlist");
    Check(filtered.ChangedSlots(patched) == bodySlots, "Clearing ABM restores the affected slots");
    Snapshot forwarded;
    forwarded.Add("Breasts", "Consolidated key", 0.5f, Providers{.abba = true, .slif = true});
    Check(forwarded.ChangedSlots(empty) == bodySlots, "SLIF can aggregate ABBA into another key");
    return EXIT_SUCCESS;
}

