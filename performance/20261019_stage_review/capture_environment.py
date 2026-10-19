"""Read-only Windows hardware/power inventory; never runs a solver or modifies power settings."""
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import winreg


def registry(path, names):
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as key:
        result = {}
        for name in names:
            try:
                result[name] = winreg.QueryValueEx(key, name)[0]
            except OSError as error:
                result[name] = {"unavailable": str(error)}
        return result


def main():
    here = Path(__file__).resolve().parent
    class Memory(ctypes.Structure):
        _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32)] + [(name, ctypes.c_uint64) for name in
                    ("total_physical", "available_physical", "total_pagefile", "available_pagefile", "total_virtual", "available_virtual", "available_extended_virtual")]
    memory = Memory(); memory.length = ctypes.sizeof(memory)
    api = ctypes.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx
    api.argtypes = [ctypes.POINTER(Memory)]; api.restype = ctypes.c_int
    if not api(ctypes.byref(memory)):
        raise ctypes.WinError(ctypes.get_last_error())
    power = subprocess.run(["powercfg", "/getactivescheme"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    guid = re.search(rb"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", power.stdout)
    report = {"captured_utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
              "logical_processors": os.cpu_count(), "total_physical_bytes": memory.total_physical,
              "cpu": registry(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", ["ProcessorNameString", "VendorIdentifier", "Identifier"]),
              "os": registry(r"SOFTWARE\Microsoft\Windows NT\CurrentVersion", ["ProductName", "EditionID", "DisplayVersion", "CurrentBuildNumber", "UBR"]),
              "power_scheme_guid": guid.group().decode("ascii") if guid else None, "power_query_exit_code": power.returncode,
              "previous_inventory": "../20261019_runner/environment.json",
              "limits": "Inventory only, not an idle-system or thermal measurement. No power/affinity/priority setting changed. Registry ProductName can lag the Windows version. Benchmark peak process memory is recorded separately."}
    target = here / "environment.json"
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(target)


if __name__ == "__main__":
    main()
