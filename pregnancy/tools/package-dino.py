"""Package the optional DLL override and matching DinoRMF source separately."""
import argparse
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


def package(build, dll, dist, version):
    source = build / "dino-compat-source"
    notices = ("LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "PNS-CHANGES.txt")
    # Check all mandatory inputs before opening/truncating either output archive.
    for required in (dll, *(source / name for name in notices)):
        if not required.is_file():
            raise FileNotFoundError(required)
    stem = "PregnancyNormalMapPlusPlus-" + version + "-DinoRMF-1.4.1"
    instructions = (
        "Install Dino's Regional Muscle and Fat 1.4.1, Pregnancy Normal Map++ " + version +
        ", and then this DLL override. Keep Dino's INIs and all its slot textures installed.\n"
        "This compatibility build modifies FlightDino's GPL DinoRMF. The matching Dino source "
        "is distributed in " + stem + "-source.zip. The accompanying Pregnancy Normal Map++ "
        "source includes the pinned dependency downloader and build instructions.\n"
    )
    dist.mkdir(parents=True, exist_ok=True)
    with ZipFile(dist / (stem + "-patch.zip"), "w", ZIP_DEFLATED) as z:
        z.write(dll, "SKSE/Plugins/DinoRMF.dll")
        z.writestr("docs/DinoRMF-compat/INSTALL.txt", instructions)
        for name in notices:
            z.write(source / name, "docs/DinoRMF-compat/" + name)
    with ZipFile(dist / (stem + "-source.zip"), "w", ZIP_DEFLATED) as z:
        for path in sorted(source.rglob("*")):
            if path.is_file():
                z.write(path, path.relative_to(source).as_posix())
        z.writestr("INSTALL.txt", instructions)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "dll", "dist"):
        parser.add_argument(name, type=Path)
    parser.add_argument("version")
    package(**vars(parser.parse_args()))

