#!/usr/bin/env python3
"""Record reproducible native-store and end-to-end protocol timings as JSON.

This is a benchmark driver, not part of the editor runtime. Results describe
this machine and workload only; they do not compare cpymacs with other editors.
"""
from __future__ import annotations
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


def protocol_benchmark(binary: Path, directory: Path, mib: int, iterations: int = 300):
    target = directory / f'benchmark-{mib}.py'
    line = b'value = 123  # a representative short line for viewport rendering\n'
    target.write_bytes((line * ((mib * 1024 * 1024 // len(line)) + 1))[:mib*1024*1024])
    errors = tempfile.TemporaryFile()
    process = subprocess.Popen([str(binary), '--batch', '--no-user-config'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors)
    def request(op, **kw):
        process.stdin.write(json.dumps({'op': op, **kw}).encode() + b'\n')
        process.stdin.flush()
        answer = json.loads(process.stdout.readline())
        if not answer['ok']:
            raise RuntimeError(answer)
        return answer
    try:
        request('hello', rows=40, cols=120)
        begin = time.perf_counter()
        request('open', path=str(target))
        open_seconds = time.perf_counter()-begin
        request('point', byte=target.stat().st_size//2)
        for _ in range(10):
            request('key', key='x')
        elapsed = []
        payload_sizes = []
        for _ in range(iterations):
            begin = time.perf_counter_ns()
            answer = request('key', key='x')
            elapsed.append((time.perf_counter_ns()-begin)/1e6)
            payload_sizes.append(len(json.dumps(answer).encode()))
        elapsed.sort()
        return {'file_bytes': target.stat().st_size, 'iterations': iterations,
                'viewport_rows': 40, 'viewport_columns': 120,
                'open_seconds_excluding_startup': open_seconds,
                'median_key_roundtrip_ms': statistics.median(elapsed),
                'p95_key_roundtrip_ms': elapsed[int(len(elapsed)*0.95)],
                'mean_response_bytes': statistics.mean(payload_sizes),
                'includes': 'Python driver serialization/parsing, local pipe IPC, native editing and viewport generation; excludes display painting'}
    finally:
        process.stdin.close()
        code = process.wait(timeout=10)
        process.stdout.close()
        errors.close()
        if code:
            raise RuntimeError(f'Backend exited with status {code}')


def main():
    build = Path(sys.argv[1] if len(sys.argv)>1 else 'build-release').resolve()
    cpu = 'unknown'
    info = Path('/proc/cpuinfo')
    if info.exists():
        for line in info.read_text().splitlines():
            if line.startswith('model name'):
                cpu=line.split(':',1)[1].strip()
                break
    runs = [json.loads(subprocess.check_output([str(build/'bench_text'),'64','20000'])) for _ in range(3)]
    metrics = ('load_seconds','edit_pair_microseconds','seek_pair_microseconds',
               'typing_microseconds_per_character','stream_to_dev_null_seconds','peak_rss_kib')
    with tempfile.TemporaryDirectory(prefix='cpymacs-bench-') as temporary:
        protocol = [protocol_benchmark(build/'cpymacs',Path(temporary),size) for size in (1,64)]
    result = {
        'schema': 1,
        'environment': {'system': platform.platform(), 'cpu':cpu,
                        'python_driver':platform.python_version(),
                        'compiler': subprocess.check_output(['cc','--version'],text=True).splitlines()[0],
                        'build': 'CMake Release, default LTO enabled when supported',
                        'timing_clock': 'CLOCK_MONOTONIC / perf_counter'},
        'native_runs': runs,
        'native_medians': {name: statistics.median(run[name] for run in runs) for name in metrics},
        'protocol_runs': protocol,
        'limitations': ['No controlled comparison with simplepypad or GNU Emacs.',
                       'Warm local container; timings are not hardware-independent guarantees.',
                       'Native-store benchmark excludes editor undo history, Python and rendering.',
                       'Peak RSS includes transient input duplication during load.',
                       'Streaming output goes to /dev/null, not durable disk.',
                       'Syntax highlighting is deliberately disabled above 16 MiB.',
                       'Protocol roundtrip excludes actual GUI/terminal painting and input-method latency.']}
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
