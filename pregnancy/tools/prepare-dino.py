"""Build inputs for the optional DinoRMF 1.4.1 compatibility DLL."""
import hashlib
import shutil
import sys
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
DIGEST = "868cf4167b82a48c84b8904e671970a3fc7d9f316b2fbd9d32e61290ea24903a"
PREFIX = "SKSE/Plugins/DinoRMF_Source/"


def replace(file, old, new):
    text = file.read_text(encoding="utf-8-sig")
    if text.count(old) != 1:
        raise ValueError(f"DinoRMF source mismatch in {file.name}: {old!r}")
    file.write_text(text.replace(old, new), encoding="utf-8")


def prepare(archive, out):
    with archive.open("rb") as contents:
        if hashlib.file_digest(contents, "sha256").hexdigest() != DIGEST:
            raise ValueError("Expected the original DinoRMF 1.4.1 ZIP")
    with ZipFile(archive) as package:
        for name in package.namelist():
            if name.startswith(PREFIX) and not name.endswith("/"):
                target = out / name.removeprefix(PREFIX)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(package.read(name))
    main = out / "src/main.cpp"
    shutil.copyfile(ROOT / "patches/dino/PregnancyBridge.h", out / "src/PregnancyBridge.h")
    replace(main, '#include "Body.h"', '#include "Body.h"\n#include "PregnancyBridge.h"')
    replace(main, "struct ActorState\n\t{", "struct ActorState\n\t{\n\t\tPregnancyBridge::Layer pregnancy;\n\t\tstd::string bakedSkin;")
    replace(main, 'if (GetModuleHandleW(L"MuDynamicNormalMap.dll")) {',
            'if (auto mu = GetModuleHandleW(L"MuDynamicNormalMap.dll"); mu && !PregnancyBridge::Connect(mu)) {')
    replace(main, '"Not installed (good)"', '(PregnancyBridge::compose ? "Pregnancy Normal Map++ bridge active" : "Not installed")')
    replace(main, "if (s.orig && !SameTexture(s.orig.get(), curTex))",
            "if (s.orig && !SameTexture(s.orig.get(), curTex)) {\n\t\t\t\ts.applied = false;")
    replace(main, "s.orig.reset(curTex);", "}\n\t\t\ts.orig.reset(curTex);")
    # Dino reads DDS files through BSResource. Mu encodes each source path inside its generated name.
    replace(main, "return v;\n\t}\n\n\tbool SameTexture", """if (v.starts_with("[mdnm]")) {
            const auto separator = v.find("::textures\\\\");
            if (separator != std::string::npos) v.erase(0, separator + 11);
        }
        return v;
    }

    bool SameTexture""")
    replace(main, "SetSlotD3D(g_slots[slot], gtex, gsrv, outSize, outMips);",
            "s.bakedSkin = TexKey(s.orig.get());\n\t\ts.pregnancy.Capture(gtex, gsrv);\n\t\tSetSlotD3D(g_slots[slot], gtex, gsrv, outSize, outMips);")
    replace(main, "if (normal && s.geom) items.push_back({ s.geom.get(), normal, nullptr });", """
            if (normal && s.geom && PregnancyBridge::compose) {
                auto mat = MaterialOf(s.geom.get());
                if (!mat || TexKey(mat->normalTexture.get()) != s.bakedSkin) normal = nullptr;
            }
            if (normal && s.geom) {
                s.pregnancy.Update(id, s.geom.get(), "textures\\\\" + s.bakedSkin,
                    [&](auto* tex, auto* view, unsigned size, unsigned mips) {
                        SetSlotD3D(g_slots[s.slot], tex, view, size, mips);
                    });
                items.push_back({ s.geom.get(), normal, nullptr });
            }""")
    replace(main, "s.geom.reset(); s.orig.reset(); s.applied = false; s.bakedSig = 0;",
            "s.pregnancy = {}; s.geom.reset(); s.orig.reset(); s.applied = false; s.bakedSig = 0;")
    replace(out / "src/plugin.cpp", "v.UsesUpdatedStructs();", "v.UsesNoStructs(); // CommonLib cross-runtime accessors")
    replace(out / "src/plugin.cpp", "a_info->version = 0x01040100;", "a_info->version = SKSEPlugin_Version.pluginVersion;")
    with (out / "src/plugin.cpp").open("a", encoding="utf-8") as plugin:
        plugin.write('\nextern "C" __declspec(dllexport) unsigned DinoRMF_PregnancyBridgeV1() { return 1; }\n')
    # Record the full corresponding source beside the optional binary in its distribution.
    (out / "PNS-CHANGES.txt").write_text(
        "Pregnancy Normal Map++ compatibility build of FlightDino's DinoRMF 1.4.1.\n"
        "Modified main.cpp and plugin.cpp and added PregnancyBridge.h. See the matching "
        "Pregnancy Normal Map++ source package for the CommonLib pin and build scripts.\n", encoding="utf-8")


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))

