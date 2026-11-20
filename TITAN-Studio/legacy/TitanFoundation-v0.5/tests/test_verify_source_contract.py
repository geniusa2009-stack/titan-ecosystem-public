import json
import tempfile
import unittest
from pathlib import Path
from tools.verify_ue427_source_contract import REQUIRED, verify


class ContractTests(unittest.TestCase):
    def make(self, folder, version=(4, 27)):
        root = Path(folder)
        path = root / 'Engine/Build/Build.version'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'MajorVersion': version[0], 'MinorVersion': version[1], 'PatchVersion': 2}))
        for rel, tokens in REQUIRED.items():
            file = root / rel
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('// mock signatures\n' + '\n'.join(tokens))
        return root

    def test_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = verify(self.make(tmp))
            self.assertTrue(report['compatible_at_source_signature_level'])
            self.assertEqual(len(report['verified_file_paths']), 3)

    def test_wrong_engine(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(verify(self.make(tmp, (5, 8)))['compatible_at_source_signature_level'])

    def test_missing_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.make(tmp)
            (root / next(iter(REQUIRED))).write_text('// missing required API')
            self.assertFalse(verify(root)['compatible_at_source_signature_level'])

    def test_empty_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(verify(Path(tmp))['compatible_at_source_signature_level'])
