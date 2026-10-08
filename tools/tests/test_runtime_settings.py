import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime_settings import MSAA_OVERRIDE, UNITY_SETTINGS, prepare_msaa_override


class RuntimeSettingsTests(unittest.TestCase):
    def test_override_reused_invalidated_and_original_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / UNITY_SETTINGS
            source.parent.mkdir(parents=True)
            source.write_bytes(b'original quality')
            calls = []
            def transform(data):
                calls.append(data)
                return b'no msaa: ' + data
            target, reused = prepare_msaa_override(root, transform)
            self.assertFalse(reused)
            self.assertEqual(target, (root / MSAA_OVERRIDE).resolve())
            self.assertEqual(source.read_bytes(), b'original quality')
            self.assertEqual(prepare_msaa_override(root, transform), (target, True))
            self.assertEqual(len(calls), 1)
            source.write_bytes(b'updated quality')
            self.assertFalse(prepare_msaa_override(root, transform)[1])
            self.assertEqual(target.read_bytes(), b'no msaa: updated quality')
            target.write_bytes(b'corrupted')
            self.assertFalse(prepare_msaa_override(root, transform)[1])
            self.assertEqual(source.read_bytes(), b'updated quality')

    def test_invalid_source_and_failed_transform_do_not_replace_existing_override(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, 'Unity'):
                prepare_msaa_override(root)
            source = root / UNITY_SETTINGS
            source.parent.mkdir(parents=True)
            source.write_bytes(b'original')
            target, _ = prepare_msaa_override(root, lambda data: b'working')
            source.write_bytes(b'changed')
            def fail(data):
                raise ValueError('invalid serialized data')
            with self.assertRaisesRegex(ValueError, 'invalid serialized'):
                prepare_msaa_override(root, fail)
            self.assertEqual(target.read_bytes(), b'working')
            self.assertEqual(source.read_bytes(), b'changed')


if __name__ == '__main__':
    unittest.main()
