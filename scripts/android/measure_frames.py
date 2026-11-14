#!/usr/bin/env python3
"""Measure a running native game without changing its settings or clearing logs.

Exit 0 meets the FPS/p95 frame budget, 1 misses it, 2 is a measurement failure.
CPU percentages use one core = 100%; GPU busy is device-wide, not per app.
Raw measurements go to --out; do not commit captures containing game/private data.
"""
import argparse
import collections
import concurrent.futures
import json
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import time


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def frame_budget(stamps, duration, target_fps, trailing_stall_s=0):
    if len(stamps) < 3 or duration <= 0:
        raise RuntimeError('Insufficient new presented buffers; no FPS verdict')
    gaps = [(right - left) / 1e6 for left, right in zip(stamps, stamps[1:])]
    fps = len(stamps) / duration
    p95 = percentile(gaps, .95)
    longest = max(max(gaps), trailing_stall_s * 1000)
    return {'frames': len(stamps), 'fps': fps, 'median_ms': statistics.median(gaps),
            'p95_ms': p95, 'p99_ms': percentile(gaps, .99), 'longest_gap_ms': longest,
            'over_50ms': sum(gap > 50 for gap in gaps), 'trailing_stall_lower_bound_s': trailing_stall_s,
            'meets_budget': fps >= target_fps * .90 and p95 <= 1500 / target_fps and longest <= 100}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', default='dev.hookshot.oot')
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--target-fps', type=float, default=60)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 5 or args.target_fps <= 0:
        parser.error('Use at least five seconds and a positive FPS target')
    args.out.mkdir(parents=True, exist_ok=False)
    prefix = ['adb', '-s', args.serial]

    def adb(*command):
        return subprocess.run(prefix + list(command), capture_output=True, text=True,
                              check=True, timeout=15).stdout

    def app_script(script):
        return adb('shell', 'run-as ' + shlex.quote(args.package) + ' sh -c ' + shlex.quote(script))

    pid = int(adb('shell', 'pidof', args.package).strip())
    ticks_per_second = int(adb('shell', 'getconf', 'CLK_TCK').strip())
    listing = adb('shell', 'dumpsys', 'SurfaceFlinger', '--list')
    layers = re.findall(re.escape(args.package) + r'/[^\s{}]+#\d+', listing)
    layers = sorted(set(layers), key=lambda name: int(name.rsplit('#', 1)[1]), reverse=True)

    def latency(layer):
        raw = adb('shell', 'dumpsys', 'SurfaceFlinger', '--latency', layer)
        rows = []
        for line in raw.splitlines():
            fields = line.split()
            if len(fields) == 3 and all(field.isdigit() for field in fields):
                row = tuple(map(int, fields))
                if 0 < row[1] < 2**63 - 1:
                    rows.append(row)
        return raw, rows

    layer = None
    for candidate in layers:
        _, rows = latency(candidate)
        if rows:
            layer = candidate
            break
    if layer is None:
        raise RuntimeError('No active game buffer layer with presentation timestamps')

    process_script = f'''
head -n 1 -v /proc/{pid}/task/*/stat /proc/{pid}/task/*/schedstat
printf 'MOVIES\n'
ls -l /proc/{pid}/fd 2>/dev/null | sed -n '/\\.webm/p'
'''
    hardware_script = '''
printf 'GPU_BUSY '; cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null
for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq; do
 printf 'FREQ %s ' "$p"; cat "$p" 2>/dev/null
 done
'''

    def process_snapshot():
        raw = app_script(process_script)
        threads = {}
        sched_tid = None
        for line in raw.splitlines():
            header = re.match(r"^==> /proc/\d+/task/(\d+)/schedstat <==$", line)
            if header:
                sched_tid = header[1]
                continue
            if sched_tid and re.fullmatch(r"\d+ \d+ \d+", line):
                run_ns, wait_ns, slices = map(int, line.split())
                if sched_tid in threads:
                    threads[sched_tid].update(run_ns=run_ns, wait_ns=wait_ns, slices=slices)
                sched_tid = None
                continue
            match = re.match(r'^(\d+) \((.*)\) (.*)$', line)
            if match:
                fields = match[3].split()
                threads[match[1]] = {'name': match[2], 'state': fields[0],
                                       'ticks': int(fields[11]) + int(fields[12]), 'cpu': int(fields[36])}
            match = re.match(r'^SCHED (\d+) (\d+) (\d+) (\d+)$', line)
            if match and match[1] in threads:
                threads[match[1]].update(run_ns=int(match[2]), wait_ns=int(match[3]), slices=int(match[4]))
        if not threads:
            raise RuntimeError('Game exited or thread statistics are inaccessible')
        return {'threads': threads, 'movies': [line.split(' -> ', 1)[-1]
                                             for line in raw.splitlines() if '.webm' in line]}

    samples, frames = [], {}
    last_progress = 0.0
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        while True:
            request_time = time.monotonic()
            frame_future = pool.submit(latency, layer)
            proc_future = pool.submit(process_snapshot)
            hardware_future = pool.submit(adb, 'shell', hardware_script)
            raw, rows = frame_future.result()
            sample = proc_future.result()
            sample.update(time=request_time - started, hardware=hardware_future.result(), latency=raw)
            sample["poll_duration_s"] = time.monotonic() - request_time
            current_stamps = {row[1] for row in rows}
            if samples and not current_stamps.intersection(previous_stamps):
                raise RuntimeError("Presentation history lost overlap; poll too slow or layer restarted")
            if not samples or max(current_stamps, default=0) > max(previous_stamps, default=0):
                last_progress = sample["time"]
            previous_stamps = current_stamps
            if not samples:
                initial_max = max(row[1] for row in rows)
            for row in rows:
                if row[1] > initial_max:
                    frames[row[1]] = row
            samples.append(sample)
            if sample['time'] >= args.seconds:
                break
            time.sleep(min(0.5, max(0, args.seconds - (time.monotonic() - started))))
    (args.out / 'samples.json').write_text(json.dumps(samples))
    (args.out / 'frames.json').write_text(json.dumps(sorted(frames.values())))
    (args.out / 'thermal.txt').write_text(adb('shell', 'dumpsys', 'thermalservice'))
    (args.out / 'game-log-tail.txt').write_text(app_script('tail -n 120 files/hookshot/user/hookshot.log'))
    stamps = sorted(frames)

    duration = samples[-1]['time'] - samples[0]['time']
    totals = collections.defaultdict(lambda: collections.Counter())
    for previous, current in zip(samples, samples[1:]):
        for tid, thread in current['threads'].items():
            before = previous['threads'].get(tid)
            if before is None:
                continue  # Unknown start time: never count lifetime CPU as interval CPU.
            total = totals[thread['name']]
            for key in ('ticks', 'run_ns', 'wait_ns'):
                if key in before and key in thread:
                    total[key] += max(0, thread[key] - before[key])
    cpu = [{'name': name, 'cpu_percent': counts['ticks'] / ticks_per_second / duration * 100,
            'run_ms_per_second': counts['run_ns'] / 1e6 / duration,
            'runqueue_wait_ms_per_second': counts['wait_ns'] / 1e6 / duration}
           for name, counts in totals.items()]
    cpu.sort(key=lambda entry: entry['cpu_percent'], reverse=True)
    gpu = []
    for sample in samples:
        match = re.search(r'GPU_BUSY\s+(\d+)\s+(\d+)', sample['hardware'])
        if match and int(match[2]) > 0:
            gpu.append(100 * int(match[1]) / int(match[2]))
    budget = frame_budget(stamps, duration, args.target_fps, samples[-1]['time'] - last_progress)
    summary = {'package': args.package, 'pid': pid, 'layer': layer, 'seconds': duration,
               'target_fps': args.target_fps, **budget,
               'cpu_percent': sum(entry['cpu_percent'] for entry in cpu), 'threads': cpu[:15],
               'gpu_device_busy_median_percent': statistics.median(gpu) if gpu else None,
               'gpu_device_busy_max_percent': max(gpu) if gpu else None,
               'movies': sorted(set(movie for sample in samples for movie in sample['movies'])),
               'limits': ['SurfaceFlinger buffers are not unique game/decoded frames.',
                          'GPU busy is device-wide; counter window semantics are driver-dependent.',
                          'CPU runqueue delay is not a frame critical-path measurement.']}
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if budget['meets_budget'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError, ValueError, OSError) as error:
        print('MEASUREMENT ERROR:', error)
        raise SystemExit(2)
