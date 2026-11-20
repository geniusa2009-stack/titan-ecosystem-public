import json
import tempfile
import unittest
from pathlib import Path
from scripts.install_into_ue427 import InstallError, install, validate_engine_root, REL_DEST


def fake_ue(root, major=4, minor=27):
    version = root / 'Engine/Build/Build.version'
    version.parent.mkdir(parents=True, exist_ok=True)
    version.write_text(json.dumps({'MajorVersion': major,'MinorVersion':minor,'PatchVersion':2}))
    h = root / 'Engine/Source/Runtime/Engine/Classes/Engine/LevelStreaming.h'
    h.parent.mkdir(parents=True, exist_ok=True)
    h.write_text('// simulated header')

class InstallTests(unittest.TestCase):
    def test_dry_run_no_changes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fake_ue(p)
            info=install(p)
            self.assertEqual(info['mode'], 'dry-run')
            self.assertFalse((p/REL_DEST).exists())
    def test_install_copies_only_plugin(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fake_ue(p)
            install(p, True)
            self.assertTrue((p/REL_DEST/'TitanFoundation.uplugin').is_file())
            self.assertTrue((p/'Engine/Build/Build.version').exists())
    def test_reinstall_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fake_ue(p)
            install(p,True)
            with self.assertRaises(InstallError):install(p,True)
    def test_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fake_ue(p,5,8)
            with self.assertRaises(InstallError):install(p, True)
    def test_missing_tree_refused(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(InstallError):install(Path(d),True)

if __name__=='__main__':unittest.main()
