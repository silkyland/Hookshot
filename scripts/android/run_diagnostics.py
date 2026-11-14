#!/usr/bin/env python3
"""Run the installed isolated diagnostics app, fail promptly on app exit, and save JSON."""
import argparse
import json
from pathlib import Path
import subprocess
import time

PACKAGE = 'dev.hookshot.diagnostics'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--mode', choices=['controller', 'benchmark'], required=True)
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--delay', type=int, default=0)
    p.add_argument('--mask', type=lambda value: int(value, 16), default=255)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    if not 0 < args.mask <= 255 or not 0 <= args.threads <= 256 or not 0 <= args.delay <= 256:
        p.error('mask must select CPUs0-7; threads/delay must be within0-256')
    prefix = ['adb', '-s', args.serial]

    def adb(*cmd):
        return subprocess.run(prefix + list(cmd), capture_output=True, text=True, timeout=20)

    adb('shell', 'am', 'force-stop', PACKAGE).check_returncode()
    result_file = 'selftest.json' if args.mode == 'controller' else 'last.json'
    adb('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/' + result_file).check_returncode()
    intent = ['shell', 'am', 'start', '-W', '-n', PACKAGE + '/.ProbeActivity']
    if args.mode == 'controller':
        intent += ['--ez', 'selftest', 'true']
    else:
        intent += ['--ez', 'benchmark', 'true', '--ez', 'controls', 'false', '--ei', 'threads', str(args.threads),
                   '--ei', 'delay', str(args.delay), '--ei', 'mask', str(args.mask)]
    launch = adb(*intent)
    launch.check_returncode()
    if 'Error' in launch.stdout + launch.stderr:
        raise RuntimeError(launch.stdout + launch.stderr)
    deadline = time.monotonic() + (15 if args.mode == 'controller' else 100)
    while time.monotonic() < deadline:
        response = adb('shell', 'run-as', PACKAGE, 'cat', 'files/' + result_file)
        try:
            result = json.loads(response.stdout)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result, indent=2))
            return 1 if 'error' in result or result.get('passed') is False else 0
        except json.JSONDecodeError:
            if not adb('shell', 'pidof', PACKAGE).stdout.strip():
                raise RuntimeError('Diagnostics app exited; inspect adb logcat -b crash')
            time.sleep(.3)
    raise RuntimeError('Diagnostics did not finish before timeout')


if __name__ == '__main__':
    raise SystemExit(main())
