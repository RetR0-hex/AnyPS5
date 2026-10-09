"""Sampling-profile report for a converted title (debug_run.py --profile).

debug_run.py --profile saves a thread snapshot (windows_debug.snapshot_threads) every
--profile-interval seconds into <run>/profile/. This module turns those samples into a ranked text
report, so finding a hot spot needs no timers in the driver: for each busy thread it lists how often
the thread ran or sat in the OS (waits, syscalls), the innermost named functions, the functions on
the stack (inclusive), and the most common call chains.

Symbols come from the modules themselves (MinGW builds keep their COFF symbol table), read with
nm/objdump from PATH or ANYPS5_MINGW_BIN (default C:\\winlibs\\mingw64\\bin). Windows system DLLs
have no such table and stay as module+offset.

Frames come from two sources in a snapshot: the frame-pointer chain (exact while every caller keeps
a frame pointer) and "stack candidates", return-address-like values scanned from the stack (they
may be stale). Chains prefer frame pointers; a sample whose frame-pointer chain names no function
falls back to the candidates, and the report counts how many did.

Usage: python tools/profile_report.py <run directory or profile directory> [--threads N]
"""
import argparse
import bisect
from collections import Counter, defaultdict
import os
from pathlib import Path
import re
import shutil
import subprocess

# Modules whose frames mean "inside the OS": a thread whose innermost frame is in one of them is
# counted as waiting or in a syscall rather than running its own code.
SYSTEM_MODULES = {'ntdll.dll', 'win32u.dll', 'kernelbase.dll', 'kernel32.dll'}
FRAME = re.compile(r'([A-Za-z0-9_.+-]+\.(?:dll|prx|exe))\+0x([0-9a-f]+)')


def tool(name):
    """A MinGW binutils program, from PATH or the WinLibs install."""
    found = shutil.which(name)
    if found:
        return found
    directory = Path(os.environ.get('ANYPS5_MINGW_BIN', r'C:\winlibs\mingw64\bin'))
    candidate = directory / (name + '.exe')
    return str(candidate) if candidate.is_file() else None


class Symbols:
    """Function lookup for one PE module: sorted (address, name) from its symbol table."""

    def __init__(self, path):
        self.base, self.addresses, self.names = 0, [], []
        nm, objdump = tool('nm'), tool('objdump')
        if nm is None or objdump is None or not Path(path).is_file():
            return
        header = subprocess.run([objdump, '-p', str(path)], capture_output=True, text=True, errors='replace').stdout
        match = re.search(r'ImageBase\s+([0-9a-fA-F]+)', header)
        if match is None:
            return
        self.base = int(match[1], 16)
        listing = subprocess.run([nm, '-C', '--defined-only', str(path)], capture_output=True, text=True, errors='replace').stdout
        entries = []
        for line in listing.splitlines():
            parts = line.split(' ', 2)
            if len(parts) == 3 and parts[1] in ('t', 'T'):
                entries.append((int(parts[0], 16), parts[2]))
        entries.sort()
        self.addresses = [address for address, _ in entries]
        self.names = [shorten(name) for _, name in entries]

    def name(self, offset):
        index = bisect.bisect_right(self.addresses, self.base + offset) - 1
        if index < 0:
            return None
        name = self.names[index]
        # Section markers (the exception tables follow .text) are not functions.
        return None if name.startswith('.') else name


def shorten(name):
    """A demangled name without argument lists and template arguments, which only add noise."""
    # Drop "(anonymous namespace)::" before the argument list: its parenthesis would otherwise cut
    # every function of an anonymous namespace down to its enclosing namespace.
    name = name.replace('(anonymous namespace)::', '')
    name = re.sub(r'\(.*', '', name)
    for _ in range(6):
        name = re.sub(r'<[^<>]*>', '', name)
    return name[-90:]


def parse_sample(text):
    """Module paths and per-thread frames of one snapshot."""
    modules, threads, current = {}, [], None
    for line in text.splitlines():
        module = re.match(r'  (\S+): 0x[0-9a-f]+\+0x[0-9a-f]+ (.+)$', line)
        if module and current is None:
            modules[module[1].lower()] = module[2]
            continue
        thread = re.match(r'Thread (\d+): (\S+)', line)
        if thread:
            current = {'tid': thread[1], 'top': thread[2], 'pointers': [], 'candidates': []}
            threads.append(current)
            continue
        if current is None:
            continue
        if line.startswith('  frame pointers:'):
            current['pointers'] = FRAME.findall(line)
        elif line.startswith('  stack candidates:'):
            current['candidates'] = FRAME.findall(line)
    return modules, threads


def build_report(directory, thread_limit=6):
    files = sorted(Path(directory).glob('s*.txt'))
    if not files:
        return f'no profile samples in {directory}\n'
    symbols, paths = {}, {}

    def name(module, offset):
        key = module.lower()
        if key in SYSTEM_MODULES or key not in paths:
            return None
        if key not in symbols:
            symbols[key] = Symbols(paths[key])
        return symbols[key].name(int(offset, 16))

    samples, running, fallback = Counter(), Counter(), Counter()
    leaves, inclusive, chains = defaultdict(Counter), defaultdict(Counter), defaultdict(Counter)
    for path in files:
        modules, threads = parse_sample(path.read_text(encoding='utf-8', errors='replace'))
        paths.update(modules)
        for thread in threads:
            tid = thread['tid']
            top = FRAME.match(thread['top'])
            top_module = top[1].lower() if top else '?'
            frames = []
            if top:
                frames.append((top[1], top[2]))
            frames += thread['pointers']
            named = [n for n in (name(m, o) for m, o in frames) if n]
            if not named and thread['candidates']:
                named = [n for n in (name(m, o) for m, o in thread['candidates']) if n]
                if named:
                    fallback[tid] += 1
            if not named:
                continue
            # Collapse recursion and repeated frames so a chain reads caller by caller.
            chain = []
            for item in named:
                if not chain or chain[-1] != item:
                    chain.append(item)
            state = 'os' if top_module in SYSTEM_MODULES else 'run'
            samples[tid] += 1
            if state == 'run':
                running[tid] += 1
            where = top_module if state == 'os' else 'own code'
            leaves[tid][(state, where, chain[0])] += 1
            for item in set(chain[:16]):
                inclusive[tid][(state, item)] += 1
            chains[tid][(state, ' <- '.join(chain[:6]))] += 1

    total = len(files)
    # Symbols are read from the module files now; a module rebuilt after sampling has moved its
    # functions, so the names would be wrong. debug_run.py writes the report as the run ends.
    sampled = files[0].stat().st_mtime
    stale = sorted(module for module in symbols if Path(paths[module]).is_file() and Path(paths[module]).stat().st_mtime > sampled)
    lines = [f'profile: {total} samples from {directory}',
             *([f'WARNING: rebuilt since sampling, names are unreliable: {", ".join(stale)}'] if stale else []),
             'state "run": the thread was executing its own code; "os": its innermost frame was in '
             'ntdll/win32u/kernel (a wait or syscall).',
             f'threads with named frames, by samples: ' + ', '.join(f'{tid} {count} ({running[tid]} running)' for tid, count in samples.most_common(12)), '']
    busiest = sorted(samples, key=lambda tid: (-running[tid], -samples[tid]))[:thread_limit]
    for tid in busiest:
        share = 100.0 * running[tid] / total
        lines.append(f'=== thread {tid}: running in {running[tid]} of {total} samples ({share:.0f}%), '
                     f'in the OS in {samples[tid] - running[tid]}; {fallback[tid]} chains from stack candidates')
        lines.append('  innermost named function (state, top module, function):')
        lines += [f'  {count:5d}  {state:3s} {where:14s} {function}' for (state, where, function), count in leaves[tid].most_common(15)]
        lines.append('  on the stack (inclusive):')
        lines += [f'  {count:5d}  {state:3s} {function}' for (state, function), count in inclusive[tid].most_common(25)]
        lines.append('  call chains (innermost first):')
        lines += [f'  {count:5d}  {state:3s} {chain}' for (state, chain), count in chains[tid].most_common(12)]
        lines.append('')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description='Summarize debug_run.py --profile samples.')
    parser.add_argument('directory', type=Path, help='A debug run directory or its profile/ folder.')
    parser.add_argument('--threads', type=int, default=6, help='Busiest threads to report.')
    arguments = parser.parse_args()
    directory = arguments.directory
    if (directory / 'profile').is_dir():
        directory = directory / 'profile'
    print(build_report(directory, arguments.threads), end='')


if __name__ == '__main__':
    main()
