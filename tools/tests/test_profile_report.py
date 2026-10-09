from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profile_report import build_report, parse_sample, shorten


# One snapshot as windows_debug.snapshot_threads writes it: the module table, then per thread the
# instruction pointer, registers, the frame-pointer chain and the scanned stack candidates.
SAMPLE = '''Process 7
Modules:
  ntdll.dll: 0x7ff000000000+0x1f8000 C:\\Windows\\SYSTEM32\\ntdll.dll
  libSceAgcDriver.prx: 0x7ff100000000+0x89e000 Z:\\missing\\libSceAgcDriver.prx
Thread 11: ntdll.dll+0x9e104 rsp=0x10 rbp=0x20
  registers: rax=0000000000000000
  frame pointers: libSceAgcDriver.prx+0x1234 -> libSceAgcDriver.prx+0x5678
  stack candidates: [rsp+0x28] libSceAgcDriver.prx+0x9abc
Thread 12: libSceAgcDriver.prx+0x42 rsp=0x30 rbp=0x40
'''


class ProfileReportTests(unittest.TestCase):
    def test_sample_lists_modules_and_each_threads_frames(self):
        modules, threads = parse_sample(SAMPLE)
        self.assertEqual(modules['libsceagcdriver.prx'], 'Z:\\missing\\libSceAgcDriver.prx')
        self.assertEqual([thread['tid'] for thread in threads], ['11', '12'])
        self.assertEqual(threads[0]['top'], 'ntdll.dll+0x9e104')
        self.assertEqual(threads[0]['pointers'], [('libSceAgcDriver.prx', '1234'), ('libSceAgcDriver.prx', '5678')])
        self.assertEqual(threads[0]['candidates'], [('libSceAgcDriver.prx', '9abc')])
        self.assertEqual(threads[1]['pointers'], [])

    def test_names_drop_arguments_templates_and_anonymous_namespaces(self):
        self.assertEqual(shorten('AgcDriver::Graphics::(anonymous namespace)::recordDraw(Context const&, std::span<int, 4ull>)'), 'AgcDriver::Graphics::recordDraw')
        self.assertEqual(shorten('std::vector<std::pair<int, int>, std::allocator<int> >::push_back(int)'), 'std::vector::push_back')

    def test_report_without_samples_or_symbols_names_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.assertIn('no profile samples', build_report(directory))
            (directory / 's00000.txt').write_text(SAMPLE, encoding='utf-8')
            report = build_report(directory)
            # The module file is missing, so no frame can be named and no thread is reported.
            self.assertIn('profile: 1 samples', report)
            self.assertNotIn('=== thread', report)


if __name__ == '__main__':
    unittest.main()
