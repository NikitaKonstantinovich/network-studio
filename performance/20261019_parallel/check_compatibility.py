"""Verify that the ordinary loader still rejects a checkpoint from the prior build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=HERE / 'old_checkpoint_rejection.json')
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    exe = ROOT / 'build/parallel/Release/network_studio_cli.exe'
    checkpoint = ROOT / 'performance/20261019_runner/runner_full/run_1/final.nscp'
    command = [str(exe), '--resume', str(checkpoint), '--threads', '8']
    if not args.execute:
        print(json.dumps(command, indent=2)); return
    if args.out.exists():
        raise RuntimeError('Choose a new output; previous evidence is preserved')
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    message = result.stdout + result.stderr
    expected = result.returncode != 0 and 'incompatible solver build' in message
    report = {'command': command, 'returncode': result.returncode, 'output': message,
              'rejected_as_expected': expected, 'integrated_seconds': 0,
              'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'checkpoint_sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest()}
    args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not expected:
        raise RuntimeError('Old checkpoint did not fail with the expected compatibility error')
    print(message.strip())


if __name__ == '__main__':
    main()
