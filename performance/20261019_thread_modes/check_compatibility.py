"""Prove old build rejection without editing checkpoint headers/checksums."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from measure import ROOT, sha, write

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--old-cli", type=Path, required=True)
p.add_argument("--new-cli", type=Path, default=ROOT / "build/Release/network_studio_cli.exe")
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
report = {"runs": []}
started = time.perf_counter()
commands = [
    ("old", [str(a.old_cli.resolve()), "--preset", "4", "--duration", ".003", "--checkpoint", str(a.out / "old.nscp"), "--output", str(a.out / "old")]),
    ("rejected", [str(a.new_cli.resolve()), "--resume", str(a.out / "old.nscp"), "--threads", "16", "--output", str(a.out / "rejected")]),
    ("new", [str(a.new_cli.resolve()), "--preset", "4", "--duration", ".003", "--threads", "16", "--checkpoint", str(a.out / "new.nscp"), "--output", str(a.out / "new")]),
]
for name, command in commands:
    row_start = time.perf_counter()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    log = result.stdout.decode("utf-8", errors="replace")
    (a.out / (name + ".log")).write_text(log, encoding="utf-8")
    report["runs"].append({"name": name, "command": command, "exit_code": result.returncode,
        "seconds": time.perf_counter() - row_start, "exe_sha256": sha(Path(command[0])), "output": log})
    if name == "rejected":
        assert result.returncode != 0 and "build" in log.lower(), log
        assert not list(a.out.glob("rejected_*.csv")), "Rejected checkpoint generated output"
    else:
        assert result.returncode == 0, log
report["csv"] = {}
for old in sorted(a.out.glob("old_*.csv")):
    new = a.out / ("new" + old.name[3:])
    assert old.read_bytes() == new.read_bytes(), old.name
    report["csv"][old.name] = {"identical": True, "sha256": sha(old)}
report["old_checkpoint_sha256"] = sha(a.out / "old.nscp")
report["elapsed_seconds"] = time.perf_counter() - started
report["note"] = "Old checkpoint rejected by normal loader. No build metadata or checksum edits. 3 ms preset 4 serial fallback CSV regression only; not a large-network performance measurement."
write(a.out / "report.json", report)
print(json.dumps(report, indent=2))
