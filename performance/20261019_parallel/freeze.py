"""Freeze the tested Release into NEW artifacts; never build, run tests or replace a reference.

Plan only by default. After all builds, tests and measurements have stopped:
  python performance/20261019_parallel/freeze.py --execute \
      --test-report performance/20261019_parallel/ctest_final.xml
Supply multiple --test-report arguments when the final validation used several suites.
The read-only Ninja dry run must report that every target is up to date.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PREVIOUS = ROOT / "performance/20261019_runner"
PROTECTED = {"lab1", "lab2", "lab3", "reproducibility", "model_audit"}
SOURCE_EXTENSIONS = {".cpp", ".c", ".h", ".hpp", ".cuh", ".inc", ".py", ".ps1", ".cmd", ".cmake", ".md"}
REQUIRED_EXES = {"network_studio.exe", "network_studio_cli.exe", "network_studio_benchmark.exe",
                 "network_studio_runner_benchmark.exe", "network_studio_parallel_tests.exe"}


def fail(message):
    raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def stable_read(path):
    """Reject a concurrent build/edit instead of freezing a mixed snapshot."""
    before = path.stat()
    data = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        fail(f"File changed while being read: {path}")
    return data


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_new(path, data):
    with path.open("xb") as stream:
        stream.write(data)


def read_json(path):
    return json.loads(stable_read(path).decode("utf-8-sig"))


def run_readonly(arguments, cwd=ROOT):
    result = subprocess.run([str(v) for v in arguments], cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    if result.returncode:
        fail(f"Read-only command failed: {arguments}\n{result.stderr.decode('utf-8', errors='replace')}")
    return result.stdout


def project_file(name):
    name = name.replace("\\", "/")
    pure = PurePosixPath(name)
    if pure.is_absolute() or ".." in pure.parts or ":" in name:
        fail(f"Unsafe source inventory path: {name}")
    path = (ROOT / name).resolve()
    if not path.is_relative_to(ROOT):
        fail(f"Source resolves outside the project: {name}")
    if not path.is_file():
        fail(f"Previous source/dependency is missing: {name}")
    return path


def inventory(previous):
    # Keep every old dependency/fixture exactly in its original relative path.
    # Do not recursively collect performance run outputs or build directories.
    names = {name.replace("\\", "/") for name in previous}
    for folder in (ROOT, ROOT / "numerics", ROOT / "performance", HERE):
        for path in folder.iterdir():
            if path.is_file() and (path.suffix.lower() in SOURCE_EXTENSIONS or
                                   path.name in {"CMakeLists.txt", ".gitignore"}):
                names.add(path.relative_to(ROOT).as_posix())
    return sorted(names)


def cmake_cache(text):
    values = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[^=]*=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    return values


def cmake_sets(text):
    values = {}
    for match in re.finditer(r"^set\((\w+)\s+(?:\"([^\"]*)\"|([^\s)]+))\)", text, re.M):
        values[match[1]] = match[2] if match[2] is not None else match[3]
    return values


def build_identity(build, sources):
    cache_path = build / "CMakeCache.txt"
    cache_data = stable_read(cache_path)
    cache = cmake_cache(cache_data.decode("utf-8-sig"))
    if cache.get("CMAKE_BUILD_TYPE") != "Release" or cache.get("STUDIO_PROFILE") != "OFF":
        fail("Freeze requires a non-profile Release build")
    if cache.get("CMAKE_GENERATOR") != "Ninja":
        fail("The freshness check currently supports the measured Ninja build only")
    compiler_files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(compiler_files) != 1:
        fail("Expected one configured CMake compiler description")
    compiler_path = compiler_files[0]
    system_path = compiler_path.parent / "CMakeSystem.cmake"
    compiler_data, system_data = stable_read(compiler_path), stable_read(system_path)
    compiler = cmake_sets(compiler_data.decode("utf-8-sig"))
    system = cmake_sets(system_data.decode("utf-8-sig"))
    variables = {**cache, **compiler, **system}
    variables["CMAKE_SIZEOF_VOID_P"] = compiler["CMAKE_CXX_SIZEOF_DATA_PTR"]
    cmake = sources["CMakeLists.txt"].decode("utf-8-sig")
    template = re.search(r'set\(CHECKPOINT_SIGNATURE\s+"([^"]+)"\)', cmake)
    loop = re.search(r"foreach\(source\s+([^)]*)\)", cmake)
    if not template or not loop:
        fail("CMake checkpoint signature recipe changed; review freeze.py")
    inputs = loop[1].split()
    for name in ("numerics/worker_pool.h", "numerics/worker_pool.cpp"):
        if name not in inputs:
            fail(f"Parallel kernel input is absent from the checkpoint fingerprint: {name}")
    signature = re.sub(r"\$\{([^}]+)\}", lambda m: variables[m[1]], template[1])
    hashes = {}
    for name in inputs:
        if name not in sources:
            fail(f"Fingerprint input missing from source archive: {name}")
        hashes[name] = sha(sources[name])
        signature += f"|{name}:{hashes[name]}"
    calculated = sha(signature.encode("utf-8"))
    header_data = stable_read(build / "checkpoint_build.h")
    header = header_data.decode("utf-8-sig")
    match = re.search(r'STUDIO_CHECKPOINT_BUILD_ID\s+"([0-9a-f]{64})"', header)
    if not match or match[1] != calculated:
        fail("Current sources/toolchain do not match checkpoint_build.h; finish the final build first")
    ninja_data = stable_read(build / "build.ninja")
    ninja = ninja_data.decode("utf-8-sig")
    block = re.search(r"^build [^\n]*network\.cpp\.obj:[^\n]*\n((?:[ \t]+[^\n]*\n)*)", ninja, re.M)
    flag_match = re.search(r"^\s+FLAGS = (.*)$", block[1], re.M) if block else None
    if not flag_match:
        fail("Cannot read actual network.cpp compile flags from build.ninja")
    flags = flag_match[1].strip()
    if re.search(r"(?:/fp:fast|-ffast-math|-Ofast)(?:\s|$)", flags, re.I):
        fail("Refusing to label a fast-math build as the strict numerical Release")
    dry_run = run_readonly([cache["CMAKE_MAKE_PROGRAM"], "-C", build, "-n"]).decode("utf-8", errors="replace")
    if "ninja: no work to do." not in dry_run or re.search(r"^\[\d+/", dry_run, re.M):
        fail("Release has pending build work (nothing was executed):\n" + dry_run)
    facts = {"compiler": compiler["CMAKE_CXX_COMPILER_ID"] + " " + compiler["CMAKE_CXX_COMPILER_VERSION"],
             "compiler_path": compiler["CMAKE_CXX_COMPILER"],
             "architecture": compiler.get("CMAKE_CXX_COMPILER_ARCHITECTURE_ID"),
             "system_name": system["CMAKE_SYSTEM_NAME"], "system_processor": system["CMAKE_SYSTEM_PROCESSOR"],
             "pointer_bytes": int(variables["CMAKE_SIZEOF_VOID_P"]), "generator": cache["CMAKE_GENERATOR"],
             "cmake_version": compiler_path.parent.name, "configuration": "Release", "profile": False,
             "flags": flags, "runtime": "MultiThreaded (/MT)" if re.search(r"(?:^|\s)[/-]MT(?:\s|$)", flags) else "See actual flags",
             "checkpoint_build_id": calculated, "checkpoint_build_header": header.strip(),
             "numeric_build_input_count": len(inputs), "numeric_build_inputs": hashes,
             "freshness_check": {"command": [cache["CMAKE_MAKE_PROGRAM"], "-C", str(build), "-n"], "output": dry_run.strip()},
             "source_binding_limit": "Fingerprint verifies core sources and flags; Ninja dry run verifies local build dependencies. No executable, compiler, build or test is run by freezing."}
    evidence = {"CMakeCache.txt": cache_data, "CMakeCXXCompiler.cmake": compiler_data,
                "CMakeSystem.cmake": system_data, "checkpoint_build.h": header_data,
                "build.ninja": ninja_data, "checkpoint_signature.txt": signature.encode("utf-8")}
    return facts, evidence


def test_evidence(paths):
    if not paths:
        fail("--execute requires at least one --test-report from the completed final validation")
    report = []; payload = {}; names = set()
    for index, path in enumerate(paths, 1):
        data = stable_read(path); tree = ET.fromstring(data)
        cases = list(tree.iter("testcase"))
        if not cases or any(case.find("failure") is not None or case.find("error") is not None or
                            case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"} for case in cases):
            fail(f"Test report has no completed cases or contains failure/error/skip: {path}")
        if any(int(node.get("failures", "0")) or int(node.get("errors", "0")) for node in tree.iter("testsuite")):
            fail(f"Test report records a failing suite: {path}")
        case_names = [case.get("name", "") for case in cases]; names.update(case_names)
        target = f"test_report_{index}_{path.name}"
        payload[target] = data
        report.append({"source": str(path.resolve()), "copy": "freeze_evidence/" + target,
                       "sha256": sha(data), "tests": case_names, "recorded_timestamp": tree.get("timestamp")})
    return {"reports": report, "unique_test_count": len(names), "unique_tests": sorted(names),
            "limits": "Reports are preserved evidence supplied by the caller, not rerun or a claim that every copied executable was itself a test target."}, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/parallel")
    parser.add_argument("--out", type=Path, default=HERE)
    parser.add_argument("--test-report", action="append", type=Path, default=[])
    parser.add_argument("--execute", action="store_true", help="Create new immutable artifacts after all runs and edits have stopped")
    args = parser.parse_args(); build = args.build_dir.resolve(); out = args.out.resolve()
    if not out.is_relative_to(ROOT / "performance"):
        fail("Output must be a new artifact location inside configurator/performance")
    outputs = ["source_snapshot.zip", "source_files.json", "working_tree.patch", "build_manifest.json", "parallel-release", "freeze_evidence"]
    if not args.execute:
        print(json.dumps({"mode": "plan_only", "build": str(build), "out": str(out), "new_outputs": outputs,
                          "previous_inventory": str(PREVIOUS / "source_files.json"),
                          "action": "After final tests and benchmarks, rerun with --execute and --test-report PATH. No files hashed or copied."}, indent=2))
        return
    for name in outputs:
        if (out / name).exists():
            fail(f"Refusing to overwrite frozen artifact: {out / name}; use a fresh --out directory")
    # Measurement tools share this lock. An existing lock must be investigated;
    # freezing never removes it or attempts to stop a running process.
    if (ROOT / "performance/.benchmark.lock").exists():
        fail("Benchmark lock exists; wait for measurements to finish before freezing")
    previous_manifest_data = stable_read(PREVIOUS / "build_manifest.json")
    previous_manifest = json.loads(previous_manifest_data.decode("utf-8-sig"))
    previous_inventory_data = stable_read(PREVIOUS / "source_files.json")
    previous = json.loads(previous_inventory_data.decode("utf-8-sig"))
    previous_archive = PREVIOUS / previous_manifest["source_archive"]
    previous_patch = PREVIOUS / previous_manifest["working_tree_patch"]
    previous_archive_data = stable_read(previous_archive)
    archive_hash = sha(previous_archive_data); patch_data = stable_read(previous_patch)
    if archive_hash != previous_manifest["source_archive_sha256"] or sha(patch_data) != previous_manifest["patch_sha256"]:
        fail("Previous source archive/working-tree patch changed; preservation cannot be confirmed")
    with zipfile.ZipFile(io.BytesIO(previous_archive_data)) as baseline:
        for name, record in previous.items():
            data = baseline.read(name)
            if sha(data) != record["sha256"] or len(data) != record["size"]:
                fail(f"Previous source inventory does not match its verified archive: {name}")
    names = inventory(previous)
    source_data = {name: stable_read(project_file(name)) for name in names}
    source_manifest = {name: {"sha256": sha(data), "size": len(data)} for name, data in source_data.items()}
    protected = sorted(name for name in previous if PurePosixPath(name).parts[0] in PROTECTED)
    modified_protected = [name for name in protected if source_manifest[name]["sha256"] != previous[name]["sha256"]]
    if modified_protected:
        fail("Protected laboratory/audit files changed since the runner freeze: " + ", ".join(modified_protected))
    build_facts, evidence = build_identity(build, source_data)
    tests, test_payload = test_evidence([p.resolve() for p in args.test_report]); evidence.update(test_payload)
    evidence.update({"previous_build_manifest.json": previous_manifest_data, "previous_source_files.json": previous_inventory_data,
                     "previous_working_tree.patch": patch_data})
    for name in ("environment.json", "provenance.json", "crossover_provenance.json",
                 "crossover_provenance_v2.json", "old_checkpoint_rejection.json",
                 "native_close_parallel.json", "native_close_serial.json"):
        if (HERE / name).is_file():
            evidence[name] = stable_read(HERE / name)
    executables = sorted((build / "Release").glob("*.exe"))
    if not REQUIRED_EXES.issubset({path.name for path in executables}):
        fail("Final Release directory is missing required GUI/CLI/benchmark/parallel test executables")
    executable_data = {path.name: stable_read(path) for path in executables}
    for name in REQUIRED_EXES - {"network_studio_parallel_tests.exe"}:
        if build_facts["checkpoint_build_id"].encode("ascii") not in executable_data[name]:
            fail(f"Final core build ID not found in {name}; executable may be stale")
    latest_test_time = max(path.resolve().stat().st_mtime_ns for path in args.test_report)
    if max(path.stat().st_mtime_ns for path in executables) > latest_test_time:
        fail("An executable is newer than all supplied test reports; finish final validation before freezing")
    git_head = run_readonly(["git", "rev-parse", "HEAD"]).decode().strip()
    git_branch = run_readonly(["git", "branch", "--show-current"]).decode().strip()
    git_status = run_readonly(["git", "status", "--porcelain=v1", "--untracked-files=all", "--", "."])
    patch = run_readonly(["git", "diff", "--binary", "HEAD", "--", "."])
    evidence["git_status_before_freeze.txt"] = git_status
    # Do not make a mixed snapshot if a source changed after initial validation.
    for name, expected in source_manifest.items():
        if sha(stable_read(project_file(name))) != expected["sha256"]:
            fail(f"Source changed during freeze validation: {name}")
    for path in executables:
        if sha(stable_read(path)) != sha(executable_data[path.name]):
            fail(f"Executable changed during freeze validation: {path}")
    out.mkdir(parents=True, exist_ok=True)
    release = out / "parallel-release"; release.mkdir(exist_ok=False)
    evidence_dir = out / "freeze_evidence"; evidence_dir.mkdir(exist_ok=False)
    for name, data in executable_data.items():
        write_new(release / name, data)
    for name, data in evidence.items():
        write_new(evidence_dir / name, data)
    write_new(out / "working_tree.patch", patch)
    write_new(out / "source_files.json", json_bytes(source_manifest))
    archive = out / "source_snapshot.zip"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for name, data in source_data.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 19, 0, 0, 0)); info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            bundle.writestr(info, data)
    manifest = {"frozen_utc": datetime.now(timezone.utc).isoformat(), "git_head": git_head, "branch": git_branch,
                "dirty": bool(git_status.strip()), "configuration": "Release", "parallel_solver": True,
                "source_archive": archive.name, "source_archive_sha256": sha(stable_read(archive)),
                "source_file_count": len(source_manifest), "source_files": "source_files.json",
                "source_files_sha256": sha(json_bytes(source_manifest)), "working_tree_patch": "working_tree.patch", "patch_sha256": sha(patch),
                "build": build_facts, "checkpoint_build_header": build_facts["checkpoint_build_header"],
                "executables": {name: {"path": "parallel-release/" + name, "sha256": sha(data), "size": len(data)} for name, data in executable_data.items()},
                "previous_state": {"manifest": "freeze_evidence/previous_build_manifest.json", "manifest_sha256": sha(previous_manifest_data),
                                   "git_head": previous_manifest["git_head"], "branch": previous_manifest["branch"], "dirty": previous_manifest["dirty"],
                                   "source_archive": str(previous_archive), "source_archive_sha256": archive_hash,
                                   "working_tree_patch": "freeze_evidence/previous_working_tree.patch", "patch_sha256": sha(patch_data)},
                "preservation_checks": {"previous_archive_and_patch_unchanged": True, "previous_source_files_all_present": True,
                                        "protected_files_unchanged": len(protected), "protected_roots": sorted(PROTECTED),
                                        "changed_source_paths_since_previous_freeze": sorted(name for name in previous if source_manifest[name]["sha256"] != previous[name]["sha256"]),
                                        "new_source_paths": sorted(set(source_manifest) - set(previous)),
                                        "limits": "Checks cover the previous manifest, not unlisted large laboratory CSV/PDF/PNG. Existing changes are preserved in the verified previous archive and patch; intentional source edits are listed, not called byte-identical."},
                "tests": tests,
                "evidence": {name: {"sha256": sha(data), "size": len(data)} for name, data in evidence.items()},
                "archive_scope": f"All {len(previous)} previous source/dependency/fixture entries plus current root/numerics/performance/parallel-stage source tools and docs. No recursive collection of benchmark outputs, EXE, archives, checkpoints, or build trees.",
                "restore": {"source_layout": "Extract source_snapshot.zip into a NEW directory; paths are project-relative, no overlay is required.",
                            "configure_and_build": "From the extracted directory: ./performance/build.ps1 -Build build/parallel -Targets all",
                            "tests": "ctest --test-dir build/parallel --output-on-failure --parallel 1",
                            "limits": "Matching numerical checkpoint identity requires the recorded toolchain/platform/flags; a clean rebuild does not promise byte-identical PE timestamps. Never bypass checkpoint compatibility."},
                "completion_marker": "This manifest is written last. If absent, the freeze is incomplete; partial artifacts are never overwritten."}
    write_new(out / "build_manifest.json", json_bytes(manifest))
    print(f"Frozen {len(source_manifest)} sources and {len(executable_data)} executables into {out}")


if __name__ == "__main__":
    main()
