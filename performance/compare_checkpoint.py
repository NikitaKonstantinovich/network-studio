"""Read-only comparison of schema-4 checkpoints; never loads or resumes a Simulator.

Only the length-prefixed build ID and the dependent final FNV-1a checksum may
differ. Both original checksums are verified, and every other byte is compared.
This proves equality of the serialized payload, not cross-build resumability.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

MAGIC = b"NETWORK-STUDIO-CHECKPOINT\n"
SCHEMA = 4
MAX_BYTES = 512 * 1024 * 1024
CHUNK = 1024 * 1024


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def read_checkpoint(path):
    if not len(MAGIC) + 48 <= path.stat().st_size <= MAX_BYTES:
        raise ValueError(f"{path}: invalid checkpoint size")
    data = path.read_bytes()
    end = len(data) - 8
    if not data.startswith(MAGIC):
        raise ValueError(f"{path}: incorrect checkpoint magic")
    position = len(MAGIC)

    def u64():
        nonlocal position
        if position + 8 > end:
            raise ValueError(f"{path}: truncated header")
        value = struct.unpack_from("<Q", data, position)[0]
        position += 8
        return value

    def text(maximum):
        nonlocal position
        size = u64()
        if size > maximum or size > end - position:
            raise ValueError(f"{path}: invalid header string length")
        result = data[position:position + size]
        position += size
        return result

    schema = u64()
    if schema != SCHEMA:
        raise ValueError(f"{path}: unsupported schema {schema}; comparator supports {SCHEMA}")
    algorithm = text(256).decode("utf-8")
    build_start = position
    build = text(256).decode("ascii")
    build_end = position
    if not re.fullmatch(r"[0-9a-f]{64}", build):
        raise ValueError(f"{path}: expected a 64-character SHA-256 build ID")
    config = text(64 * 1024 * 1024)
    if not algorithm or not config or position == end:
        raise ValueError(f"{path}: empty algorithm/configuration or missing state payload")
    stored = struct.unpack_from("<Q", data, end)[0]
    calculated = fnv1a(memoryview(data)[:end])
    if stored != calculated:
        raise ValueError(f"{path}: original integrity checksum does not match")
    return {"data": data, "build_start": build_start, "build_end": build_end,
            "config": config, "algorithm": algorithm,
            "metadata": {"path": str(path), "bytes": len(data), "schema": schema,
                         "algorithm": algorithm, "build_id": build,
                         "config_sha256": hashlib.sha256(config).hexdigest(),
                         "file_sha256": hashlib.sha256(data).hexdigest(),
                         "payload_sha256": hashlib.sha256(memoryview(data)[build_end:end]).hexdigest(),
                         "stored_checksum_u64_hex": f"{stored:016x}",
                         "original_checksum_valid": True,
                         "build_field_byte_range": [build_start, build_end],
                         "checksum_byte_range": [end, len(data)]}}


def first_payload_difference(a, b):
    left = memoryview(a["data"])[a["build_end"]:-8]
    right = memoryview(b["data"])[b["build_end"]:-8]
    common = min(len(left), len(right))
    for start in range(0, common, CHUNK):
        stop = min(common, start + CHUNK)
        x, y = bytes(left[start:stop]), bytes(right[start:stop])
        if x != y:
            return start + next(i for i, pair in enumerate(zip(x, y)) if pair[0] != pair[1])
    return common if len(left) != len(right) else None


def compare(reference, actual):
    a, b = read_checkpoint(reference), read_checkpoint(actual)
    same_algorithm = a["algorithm"] == b["algorithm"]
    same_config = a["config"] == b["config"]
    same_header = a["data"][:a["build_start"]] == b["data"][:b["build_start"]]
    first = first_payload_difference(a, b)
    same_payload = first is None
    result = {"equal_except_build_metadata": same_header and same_payload and same_algorithm and same_config,
              "checkpoint_files_byte_equal": a["data"] == b["data"],
              "same_schema": True, "same_algorithm": same_algorithm,
              "same_serialized_config_bytes": same_config,
              "same_non_build_header_bytes": same_header,
              "same_complete_payload_bytes": same_payload,
              "reference": a["metadata"], "actual": b["metadata"],
              "build_id_differs": a["metadata"]["build_id"] != b["metadata"]["build_id"],
              "checksum_differs": a["data"][-8:] != b["data"][-8:],
              "first_payload_difference": None if first is None else {
                  "relative_to_config_length_field": first,
                  "reference_file_offset": a["build_end"] + first,
                  "actual_file_offset": b["build_end"] + first},
              "excluded_fields": ["length-prefixed build ID", "dependent final integrity checksum"],
              "guarantee": "Every other serialized byte is identical, including full parameters, adaptive step, next sample, armed thresholds, states, traces, histories, delivery queues and runtime protocols. Both original integrity checksums were verified.",
              "limits": "Byte comparison only: no Simulator was constructed, no checkpoint was rewritten, and no loader compatibility check was bypassed. This does not grant cross-build resumability or replace native semantic snapshot validation. A positive guarantee applies only when equal_except_build_metadata is true."}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--out", type=Path, help="Optional JSON report; checkpoint files are never modified")
    args = parser.parse_args()
    reference, actual = args.reference.resolve(), args.actual.resolve()
    if args.out and (args.out.resolve() in {reference, actual} or args.out.suffix.lower() != ".json"):
        parser.error("--out must be a separate .json report, never an input checkpoint")
    try:
        report = compare(reference, actual)
        code = 0 if report["equal_except_build_metadata"] else 1
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        report = {"equal_except_build_metadata": False, "error": str(error)}
        code = 2
    encoded = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
