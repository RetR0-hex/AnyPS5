import argparse
from collections import Counter
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from windows_debug import capture_window, snapshot_threads
from profile_report import build_report
from runtime_settings import prepare_msaa_override


ARTIFACT_PATTERNS = ('draw_*.regs', 'frame_*.bmp', 'target_*.raw', 'capture-trace.log')


def runtime_artifacts(directory):
    return {path: (path.stat().st_mtime_ns, path.stat().st_size)
            for pattern in ARTIFACT_PATTERNS for path in directory.glob(pattern) if path.is_file()}


def collect_runtime_artifacts(source, destination, previous):
    copied, errors = [], []
    for path, stamp in runtime_artifacts(source).items():
        if previous.get(path) != stamp:
            try:
                shutil.copy2(path, destination / path.name)
                copied.append(path.name)
            except OSError as error:
                errors.append(f'{path.name}: {error}')
    return copied, errors


def trace_flags(root):
    names = {'APS5_PROFILE_DRAW', 'APS5_PROFILE_GPU'}
    # Shader front-end stalls happen before runtime IR dumps; include its trace switches too.
    for source in (root / 'core' / 'libs', root / 'core' / 'shader'):
        for path in source.rglob('*.cpp'):
            names.update(re.findall(r'"(APS5_TRACE_[A-Z0-9_]+)"', path.read_text(encoding='utf-8')))
    return sorted(names)


def summarize(path):
    reasons, errors, draws, submissions = Counter(), [], 0, 0
    with path.open(encoding='utf-8', errors='replace') as log:
        for line in log:
            line = line.rstrip()
            match = re.search(r'\[draw\] target .* failed: (.*)', line)
            if match:
                reasons[match[1]] += 1
            if re.search(r'\[draw\] target .* ok$', line):
                draws += 1
            if re.search(r'\[gpu\].*execute serial=', line):
                submissions += 1
            if len(errors) < 30 and ('FATAL:' in line or 'uncaught exception' in line or 'FAIL:' in line):
                errors.append(line)
    return {'accepted_draws': draws, 'gpu_submissions': submissions,
            'draw_rejections': dict(reasons.most_common()), 'errors': errors[:30]}


def main():
    parser = argparse.ArgumentParser(description='Run a converted executable and collect its diagnostic artifacts.')
    parser.add_argument('--debug-heavy', action='store_true', help='Enable trace/profile flags and periodic Windows thread snapshots.')
    parser.add_argument('--disable-msaa', action='store_true', help='Disable Unity MSAA using a separate runtime settings override.')
    parser.add_argument('--seconds', type=float, default=45, help='Stop the launched process after this many seconds.')
    parser.add_argument('--snapshot-interval', type=float, default=15, help='Seconds between thread snapshots and window captures.')
    parser.add_argument('--output', type=Path, help='Artifact directory; defaults to build/debug-runs/<timestamp>.')
    # Sampling profiler: thread stacks every --profile-interval seconds after --profile-warmup,
    # summarized into profile.txt (profile_report.py), plus the driver's own APS5_PROFILE_DRAW /
    # APS5_PROFILE_GPU summaries in game.log. No timers in the driver are needed to find a hot spot.
    parser.add_argument('--profile', action='store_true', help='Sample thread stacks and write profile.txt; enables the driver profile summaries.')
    parser.add_argument('--profile-warmup', type=float, default=30, help='Seconds before profile sampling starts (loading and first shader compiles are skipped).')
    parser.add_argument('--profile-interval', type=float, default=0.1, help='Seconds between profile samples.')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='Executable path followed by its arguments. Put runner options first.')
    arguments = parser.parse_args()
    if not arguments.command or arguments.seconds <= 0 or arguments.snapshot_interval <= 0 or arguments.profile_interval <= 0:
        parser.error('Supply an executable and positive run and snapshot durations.')
    if arguments.profile and os.name != 'nt':
        parser.error('--profile samples thread stacks through the Windows debugging API.')
    executable = Path(arguments.command[0]).resolve()
    if not executable.is_file():
        parser.error(f'Executable does not exist: {executable}')
    root = Path(__file__).resolve().parent.parent
    directory = arguments.output or root / 'build' / 'debug-runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    if arguments.disable_msaa:
        try:
            prepare_msaa_override(executable.parent)
        except (OSError, ValueError, RuntimeError) as error:
            parser.error(str(error))
        environment['ANYPS5_DISABLE_MSAA'] = '1'
    flags = trace_flags(root) if arguments.debug_heavy else []
    if arguments.profile:
        flags = sorted(set(flags) | {'APS5_PROFILE_DRAW', 'APS5_PROFILE_GPU'})
    for flag in flags:
        environment.setdefault(flag, '1')
    if arguments.debug_heavy:
        environment.setdefault('APS5_DUMP_QUEUE', '0')
        environment.setdefault('APS5_CAPTURE_TRACE', '1')
        environment.setdefault('APS5_DUMP_FRAMES', '8')
        environment.setdefault('APS5_DUMP_FRAMES_EVERY', '60')
        flags.extend(['APS5_CAPTURE_TRACE', 'APS5_DUMP_FRAMES', 'APS5_DUMP_FRAMES_EVERY'])
    manifest = {'command': [str(executable), *arguments.command[1:]], 'cwd': str(executable.parent),
                'disable_msaa': environment.get('ANYPS5_DISABLE_MSAA') == '1',
                'debug_heavy': arguments.debug_heavy, 'profile': arguments.profile,
                'trace_environment': {name: environment[name] for name in flags}, 'snapshots': []}
    if arguments.debug_heavy:
        manifest['trace_environment']['APS5_DUMP_QUEUE'] = environment['APS5_DUMP_QUEUE']
    log_path = directory / 'game.log'
    previous_artifacts = runtime_artifacts(executable.parent)
    started, stopped, interrupted = time.monotonic(), False, False
    with log_path.open('wb') as log:
        process = subprocess.Popen(manifest['command'], cwd=executable.parent, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        manifest['pid'] = process.pid
        print(f'Process {process.pid}; artifacts: {directory}', flush=True)
        next_snapshot = min(5, arguments.snapshot_interval)
        profile_directory = directory / 'profile'
        next_sample, samples = arguments.profile_warmup, 0
        if arguments.profile:
            profile_directory.mkdir()
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if arguments.debug_heavy and os.name == 'nt' and elapsed >= next_snapshot:
                    entry = {'seconds': round(elapsed, 2)}
                    try:
                        text, modules = snapshot_threads(process.pid)
                        filename = f'threads-{elapsed:06.1f}.txt'
                        (directory / filename).write_text(text, encoding='utf-8')
                        entry['threads'] = filename
                        manifest['modules'] = modules
                    except OSError as error:
                        entry['error'] = str(error)
                    try:
                        filename = f'window-{elapsed:06.1f}.png'
                        if capture_window(process.pid, directory / filename):
                            entry['window'] = filename
                    except OSError as error:
                        entry['capture_error'] = str(error)
                    manifest['snapshots'].append(entry)
                    print(f'Snapshot at {elapsed:.1f}s', flush=True)
                    next_snapshot += arguments.snapshot_interval
                if arguments.profile and elapsed >= next_sample:
                    try:
                        text, _ = snapshot_threads(process.pid)
                        (profile_directory / f's{samples:05d}.txt').write_text(text, encoding='utf-8')
                        samples += 1
                    except OSError:
                        # A thread that exits mid-snapshot fails the read; the next sample retries.
                        pass
                    next_sample = time.monotonic() - started + arguments.profile_interval
                if elapsed >= arguments.seconds:
                    if os.name == 'nt':
                        try:
                            manifest['final_window'] = capture_window(process.pid, directory / 'window.png')
                        except OSError as error:
                            manifest['capture_error'] = str(error)
                    stopped = True
                    break
                time.sleep(min(0.2, arguments.profile_interval) if arguments.profile else 0.2)
        except KeyboardInterrupt:
            interrupted = True
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    manifest.update({'elapsed_seconds': round(time.monotonic() - started, 2),
                     'stopped_at_limit': stopped, 'interrupted': interrupted,
                     'exit_code': process.returncode})
    summary = summarize(log_path)
    artifacts, errors = collect_runtime_artifacts(executable.parent, directory, previous_artifacts)
    manifest['runtime_artifacts'] = artifacts
    if errors:
        manifest['artifact_errors'] = errors
    if arguments.profile:
        manifest['profile_samples'] = samples
        (directory / 'profile.txt').write_text(build_report(profile_directory), encoding='utf-8')
        print(f'profile: {samples} samples, report {directory / "profile.txt"}', flush=True)
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    status = ('interrupted' if interrupted else 'stopped at time limit' if stopped
              else f'exited {process.returncode & 0xffffffff:#x}')
    print(f'{status}; {summary["accepted_draws"]} accepted draws, {sum(summary["draw_rejections"].values())} rejected draws', flush=True)
    return 130 if interrupted else 0 if stopped or process.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
