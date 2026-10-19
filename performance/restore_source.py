"""Restore an archived measurement variant into a NEW directory; never edit the checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--variant", choices=("baseline", "stage1", "optimized"), required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
archive = Path(__file__).resolve().parent / "20261019_single_thread"
manifest = json.loads((archive / "source_archives.json").read_text(encoding="utf-8"))
overlays = {"baseline": "baseline_instrumentation.zip", "stage1": "stage1_sources.zip",
            "optimized": "optimized_sources.zip"}
names = ["original-source.zip", overlays[a.variant]]
for name in names:
    if hashlib.sha256((archive / name).read_bytes()).hexdigest() != manifest[name]["sha256"]:
        raise RuntimeError("Archive checksum mismatch: " + name)
destination = a.out.resolve()
destination.mkdir(parents=True, exist_ok=False)
for name in names:
    with zipfile.ZipFile(archive / name) as z:
        for member in z.infolist():
            if not (destination / member.filename).resolve().is_relative_to(destination):
                raise RuntimeError("Unsafe archive path")
        z.extractall(destination)
print(destination)
