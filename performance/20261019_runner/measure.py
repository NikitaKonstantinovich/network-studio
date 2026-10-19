"""Sequential, non-overlapping measurement plan. Existing output directories are never overwritten."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
old = root / "performance/20261019_single_thread"
out = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent
new = root / "build/runner/Release"
plan = [
    ("gui_before", "gui", old/"optimized-release/network_studio.exe", old/"profile_fixture.json", 3, old/"short_after/run_1", []),
    ("gui_after", "gui", new/"network_studio.exe", old/"profile_fixture.json", 3, old/"short_after/run_1", ["--gui-display-hz", "10"]),
    ("direct_short", "cli", new/"network_studio_benchmark.exe", old/"profile_fixture.json", 3, old/"short_after/run_1", []),
    ("runner_full", "runner", new/"network_studio_runner_benchmark.exe", old/"fixture.json", 3, old/"after/run_1", ["--hz", "10"]),
    ("runner_hz1", "runner", new/"network_studio_runner_benchmark.exe", old/"profile_fixture.json", 1, old/"short_after/run_1", ["--hz", "1"]),
    ("runner_hz60", "runner", new/"network_studio_runner_benchmark.exe", old/"profile_fixture.json", 1, old/"short_after/run_1", ["--hz", "60"]),
    ("runner_slow_consumer", "runner", new/"network_studio_runner_benchmark.exe", old/"profile_fixture.json", 1, old/"short_after/run_1", ["--hz", "10", "--consumer-ms", "500"]),
]
for name, mode, exe, fixture, repeat, reference, extra in plan:
    cmd=[sys.executable,str(root/"performance/run_runner.py"),"--mode",mode,"--exe",str(exe),"--fixture",str(fixture),"--out",str(out/name),"--repeat",str(repeat),"--reference",str(reference),*extra]
    print("Starting " + name, flush=True)
    subprocess.run(cmd,cwd=root,check=True)
