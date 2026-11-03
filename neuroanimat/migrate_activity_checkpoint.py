"""Explicit copy-only migration for the audited 250k -> 2M activity-buffer build.

Only these two build fingerprints are supported. Numerical code and schema 8
are unchanged; network.h adds a capacity constant, stimulator.cpp uses it, and
checkpoint.cpp uses the same bound when reading/writing recent activity.
The regular application loader still rejects all foreign build fingerprints.
Validate the output with the destination CLI before continuing an experiment.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

OLD_BUILD = b"d43f543f4603e31b509651d91ea542a1e25ac8705f582a19465c895ba9f3ea94"
NEW_BUILD = b"081d95233f86ea2b0cd5ca2a67dd4d4496d8608eb026ab348db19340b427e6c4"
MAGIC = b"NETWORK-STUDIO-CHECKPOINT\n"


def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def migrate(data):
    if not 50 <= len(data) <= 512 * 1024 * 1024:
        raise ValueError("Invalid checkpoint size")
    if data[:len(MAGIC)] != MAGIC:
        raise ValueError("Wrong checkpoint magic")
    if checksum(memoryview(data)[:-8]) != struct.unpack_from("<Q", data, len(data)-8)[0]:
        raise ValueError("Checkpoint integrity mismatch")
    pos = len(MAGIC)
    if struct.unpack_from("<Q", data, pos)[0] != 8:
        raise ValueError("Only schema 8 is supported")
    pos += 8
    algorithm_size = struct.unpack_from("<Q", data, pos)[0]
    if algorithm_size > 256:
        raise ValueError("Invalid algorithm header")
    pos += 8 + algorithm_size
    build_size = struct.unpack_from("<Q", data, pos)[0]
    pos += 8
    if build_size != 64 or data[pos:pos+64] != OLD_BUILD:
        raise ValueError("This checkpoint is not from the audited predecessor build")
    result = bytearray(data)
    result[pos:pos+64] = NEW_BUILD
    struct.pack_into("<Q", result, len(result)-8, checksum(memoryview(result)[:-8]))
    assert result[:pos] == data[:pos] and result[pos+64:-8] == data[pos+64:-8]
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    data = args.source.read_bytes()
    result = migrate(data)
    # Exclusive creation: never overwrite the source or an earlier migration.
    with args.destination.open("xb") as output:
        output.write(result)
    print(json.dumps({"source": str(args.source), "destination": str(args.destination),
                      "source_sha256": hashlib.sha256(data).hexdigest(),
                      "destination_sha256": hashlib.sha256(result).hexdigest(),
                      "state_payload_byte_identical": True}, indent=2))
