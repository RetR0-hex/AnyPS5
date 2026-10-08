import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(__file__).resolve().parents[1] / 'debug_run.py'
sys.path.insert(0, str(RUNNER.parent))
from debug_run import collect_runtime_artifacts, runtime_artifacts


class DebugRunTests(unittest.TestCase):
    def test_runtime_files_exclude_stale_artifacts_and_unrelated_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'runtime'
            destination = Path(temporary) / 'capture'
            source.mkdir()
            destination.mkdir()
            (source / 'draw_old.regs').write_text('old registers')
            (source / 'frame_000.bmp').write_bytes(b'old frame')
            previous = runtime_artifacts(source)
            (source / 'frame_000.bmp').write_bytes(b'new frame data')
            (source / 'draw_new.regs').write_text('new registers')
            (source / 'save.dat').write_bytes(b'unrelated game data')
            copied, errors = collect_runtime_artifacts(source, destination, previous)
            self.assertEqual(set(copied), {'frame_000.bmp', 'draw_new.regs'})
            self.assertEqual(errors, [])
            self.assertEqual((destination / 'frame_000.bmp').read_bytes(), b'new frame data')
            self.assertFalse((destination / 'draw_old.regs').exists())
            self.assertFalse((destination / 'save.dat').exists())

    def test_heavy_capture_stops_launched_process_and_preserves_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'artifacts'
            child = ('import os,time; '
                     'print(os.getenv("APS5_PROFILE_DRAW"), os.getenv("APS5_TRACE_MEMORY"), flush=True); '
                     'time.sleep(30)')
            run = subprocess.run([sys.executable, str(RUNNER), '--debug-heavy', '--seconds', '0.3',
                                  '--output', str(output), sys.executable, '-c', child],
                                 env={**os.environ, 'APS5_TRACE_MEMORY': 'custom'},
                                 capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual((output / 'game.log').read_text().strip(), '1 custom')
            manifest = json.loads((output / 'manifest.json').read_text())
            self.assertTrue(manifest['stopped_at_limit'])
            self.assertLess(manifest['elapsed_seconds'], 5)
            self.assertEqual(manifest['trace_environment']['APS5_TRACE_MEMORY'], 'custom')

    def test_early_crash_preserves_status_and_summarizes_rejections(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'artifacts'
            child = ('print("[draw] target 0x1000 mask 0xf failed: unsupported state"); '
                     'print("[draw] target 0x1000 mask 0xf failed: unsupported state"); '
                     'print("FATAL: diagnostic fixture"); raise SystemExit(3)')
            run = subprocess.run([sys.executable, str(RUNNER), '--output', str(output),
                                  sys.executable, '-c', child], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 1, run.stderr)
            manifest = json.loads((output / 'manifest.json').read_text())
            self.assertEqual(manifest['exit_code'], 3)
            self.assertFalse(manifest['stopped_at_limit'])
            summary = json.loads((output / 'summary.json').read_text())
            self.assertEqual(summary['draw_rejections'], {'unsupported state': 2})
            self.assertEqual(summary['errors'], ['FATAL: diagnostic fixture'])


if __name__ == '__main__':
    unittest.main()
