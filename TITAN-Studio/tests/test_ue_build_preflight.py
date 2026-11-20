#!/usr/bin/env python3
"""Regression tests for tools/ue_build_preflight.py and the disk guard in scripts/ue_build.sh.
Original TITAN code.

usage: python3 -I tests/test_ue_build_preflight.py <titan repo root>

Uses a fake engine tree (a tiny Commit.gitdeps.xml and a fake GitDependencies) in a temporary
directory; never touches a real engine. Low free space is simulated with the test-only knobs, which
can only lower the measured figure. Cases that need the step to pass on this host's real free space
are skipped (with the reason) when the host has less than 40 GB free.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True  # keep the source tree clean (the module is loaded from tools/)
ROOT = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
SCRIPT = os.path.join(ROOT, "tools", "ue_build_preflight.py")
BUILD_SH = os.path.join(ROOT, "scripts", "ue_build.sh")
EXCLUDES = os.path.join(ROOT, "unreal", "BuildConfig", "gitdeps-macos.exclude")
GB = 1_000_000_000

spec = importlib.util.spec_from_file_location("ue_build_preflight", SCRIPT)
pf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pf)

MISSING_BYTES = 2 * GB  # what the fake manifest says is still to be downloaded


def make_engine(root, gitdeps_body="exit 0"):
    """Fake UE tree: one present file, one missing 2 GB file, one excluded Win64 file."""
    os.makedirs(os.path.join(root, "Engine/Build"))
    os.makedirs(os.path.join(root, "Engine/Binaries/ThirdParty/Present"))
    with open(os.path.join(root, "Engine/Binaries/ThirdParty/Present/a.bin"), "wb") as f:
        f.write(b"x" * 10)
    manifest = f"""<?xml version="1.0" encoding="utf-8"?>
<DependencyManifest BaseUrl="https://example.invalid">
  <Files>
    <File Name="Engine/Binaries/ThirdParty/Present/a.bin" Hash="h1" />
    <File Name="Engine/Binaries/ThirdParty/Missing/b.bin" Hash="h2" />
    <File Name="Engine/Binaries/Win64/c.dll" Hash="h3" />
  </Files>
  <Blobs>
    <Blob Hash="h1" Size="10" PackHash="p1" PackOffset="0" />
    <Blob Hash="h2" Size="{MISSING_BYTES}" PackHash="p2" PackOffset="0" />
    <Blob Hash="h3" Size="{50 * GB}" PackHash="p3" PackOffset="0" />
  </Blobs>
  <Packs>
    <Pack Hash="p1" Size="10" CompressedSize="5" RemotePath="x" />
    <Pack Hash="p2" Size="{MISSING_BYTES}" CompressedSize="{GB}" RemotePath="x" />
    <Pack Hash="p3" Size="{50 * GB}" CompressedSize="{20 * GB}" RemotePath="x" />
  </Packs>
</DependencyManifest>
"""
    with open(os.path.join(root, "Engine/Build/Commit.gitdeps.xml"), "w") as f:
        f.write(manifest)
    bindir = os.path.join(root, "Engine/Binaries/DotNET/GitDependencies/osx-arm64")
    os.makedirs(bindir)
    exe = os.path.join(bindir, "GitDependencies")
    with open(exe, "w") as f:
        f.write(f"#!/bin/bash\ntouch \"{root}/gitdeps-ran\"\n{gitdeps_body}\n")
    os.chmod(exe, 0o755)
    return root


def load_json(path):
    import json
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def host_free_gb(path):
    v = os.statvfs(path)
    return v.f_bavail * v.f_frsize / GB


class Evaluate(unittest.TestCase):
    """The pure budget function: one pool per APFS container / device, every written pool checked."""

    def test_locations_on_one_pool_are_added_up(self):
        rows, ok = pf.evaluate({"engine": 30 * GB, "temp": 3 * GB, "swap": 8 * GB},
                               {"engine": "disk3", "temp": "disk3", "swap": "disk3"},
                               {"disk3": {"free": 75 * GB, "mount": "/"}}, 35 * GB)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["need_bytes"], 41 * GB)
        self.assertFalse(ok)  # 75 - 41 = 34 < 35, although each need alone would fit
        rows, ok = pf.evaluate({"engine": 30 * GB, "temp": 3 * GB, "swap": 8 * GB},
                               {"engine": "disk3", "temp": "disk3", "swap": "disk3"},
                               {"disk3": {"free": 76 * GB, "mount": "/"}}, 35 * GB)
        self.assertTrue(ok)
        self.assertEqual(rows[0]["projected_free_bytes"], 35 * GB)

    def test_each_pool_is_checked_independently(self):
        pools = {"disk3": {"free": 77 * GB, "mount": "/"}, "disk9": {"free": 40 * GB, "mount": "/Volumes/SSD"}}
        locs = {"engine": "disk3", "ddc": "disk9", "temp": "disk3"}
        rows, ok = pf.evaluate({"engine": 20 * GB, "ddc": 10 * GB, "temp": 1 * GB}, locs, pools, 35 * GB)
        by = {r["pool"]: r for r in rows}
        self.assertTrue(by["disk3"]["ok"])
        self.assertFalse(by["disk9"]["ok"])       # 40 - 10 = 30 < 35 on the external pool
        self.assertEqual(by["disk9"]["short_bytes"], 5 * GB)
        self.assertNotIn("ddc", by["disk3"]["needs"])  # not charged to the internal pool
        self.assertFalse(ok)

    def test_pool_the_step_does_not_write_to_is_not_affected(self):
        pools = {"disk3": {"free": 77 * GB, "mount": "/"}, "disk7": {"free": 9 * GB, "mount": "/Volumes/USB"}}
        rows, ok = pf.evaluate({"engine": 15 * GB, "ddc": 0}, {"engine": "disk3", "ddc": "disk7"}, pools, 35 * GB)
        self.assertEqual([r["pool"] for r in rows], ["disk3"])
        self.assertTrue(ok)

    def test_nothing_to_check_is_not_a_pass(self):
        self.assertEqual(pf.evaluate({}, {}, {}, 35 * GB), ([], False))


class Stages(unittest.TestCase):
    def test_estimates_grow_with_the_stage(self):
        total = {k: sum(v.values()) for k, v in pf.STAGES.items()}
        self.assertLess(total["deps"], total["plan"])
        self.assertLess(total["plan"], total["bridge-check"])
        self.assertLess(total["bridge-check"], total["build"])

    def test_deps_does_not_budget_build_outputs(self):
        # The original bug: `deps` used the full editor build estimates.
        d = pf.STAGES["deps"]
        self.assertEqual((d["build"], d["ddc"], d["swap"]), (0.0, 0.0, 0.0))
        self.assertGreater(d["temp"], 0)

    def test_full_build_keeps_its_allowances(self):
        b = pf.STAGES["build"]
        self.assertGreaterEqual(b["build"], 25.0)
        self.assertGreaterEqual(b["ddc"], 10.0)
        self.assertGreater(b["swap"], 0)

    def test_exclusions_match_gitdependencies(self):
        ex = ["Win64", "/Engine/Content/Localization/Engine/de"]
        self.assertTrue(pf.is_excluded("Engine/Binaries/Win64/x.dll", ex))
        self.assertTrue(pf.is_excluded("Engine/Binaries/win64/x.dll", ex))          # case-insensitive
        self.assertFalse(pf.is_excluded("Engine/Binaries/Win64x/x.dll", ex))        # whole folder names only
        self.assertTrue(pf.is_excluded("Engine/Content/Localization/Engine/de/a.locres", ex))
        self.assertFalse(pf.is_excluded("Other/Engine/Content/Localization/Engine/de/a", ex))  # rooted rule


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-preflight-")
        self.engine = make_engine(os.path.join(self.tmp, "engine"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_pf(self, *args):
        out = os.path.join(self.tmp, "pf.json")
        r = subprocess.run([sys.executable, "-I", SCRIPT, "--engine", self.engine, "--exclude-file", EXCLUDES,
                            "--ddc-path", os.path.join(self.tmp, "ddc"), "--temp-path", self.tmp,
                            "--swap-path", self.tmp, "--out", out, "--guard-out", out + ".guard", *args],
                           capture_output=True, text=True)
        import json
        rep = load_json(out) if os.path.exists(out) and r.returncode in (0, 3) else None
        return r, rep

    def test_dependency_bytes_are_measured_from_the_manifest(self):
        r, rep = self.run_pf("--stage", "deps", "--simulate-free-gb", "100")
        d = rep["dependencies"]
        self.assertEqual((d["selected_files"], d["already_present_files"]), (2, 1))  # Win64 file excluded
        self.assertEqual(d["missing_unpacked_bytes"], MISSING_BYTES)
        self.assertEqual(d["download_bytes"], GB)
        self.assertFalse(rep["dependencies_complete"])

    def test_simulated_low_space_fails_deps(self):
        # 36 GB free - 2 GB dependencies - 1 GB temp = 33 GB < 35 GB headroom.
        r, rep = self.run_pf("--stage", "deps", "--simulate-free-gb", "36")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertEqual(rep["result"], "FAIL")
        self.assertIn("SHORT", r.stdout)

    def test_deps_passes_where_the_full_build_fails(self):
        # The simulation can only lower free space, so "40 GB" needs a host with at least 40 GB free.
        if host_free_gb(tempfile.gettempdir()) < 40:
            self.skipTest("host has under 40 GB free, so a simulated 40 GB cannot be reached")
        r, rep = self.run_pf("--stage", "deps", "--simulate-free-gb", "40")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r, rep = self.run_pf("--stage", "build", "--simulate-free-gb", "40")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertEqual(rep["needs_bytes"]["ddc"], 10 * GB)
        self.assertEqual(rep["needs_bytes"]["swap"], 8 * GB)

    def test_simulation_can_only_lower_free_space(self):
        r, rep = self.run_pf("--stage", "deps", "--simulate-free-gb", "1000000")
        self.assertLess(rep["pools"][0]["free_bytes"], 100_000 * GB)  # the real figure, not the simulated one
        self.assertIsNotNone(rep["simulated_free_gb"])

    def test_headroom_cannot_be_lowered(self):
        r, _ = self.run_pf("--stage", "deps", "--headroom-gb", "20")
        self.assertEqual(r.returncode, 2)
        self.assertIn("safety floor", r.stderr)

    def test_negative_estimate_is_rejected(self):
        r, _ = self.run_pf("--stage", "build", "--build-gb", "-5")
        self.assertEqual(r.returncode, 2)

    def test_not_an_engine_is_rejected(self):
        r = subprocess.run([sys.executable, "-I", SCRIPT, "--engine", self.tmp + "/nope", "--exclude-file", EXCLUDES],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)



def add_downloaded_file(root, name, nbytes):
    """Writes a file and lists it in the fake Commit.gitdeps.xml, as GitDependencies would leave it."""
    path = os.path.join(root, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(os.urandom(nbytes))
    m = os.path.join(root, "Engine/Build/Commit.gitdeps.xml")
    with open(m) as f:
        x = f.read()
    h = "d" + str(abs(hash(name)))
    x = x.replace("</Files>", f'  <File Name="{name}" Hash="{h}" />\n  </Files>')
    x = x.replace("</Blobs>", f'  <Blob Hash="{h}" Size="{nbytes}" PackHash="p{h}" PackOffset="0" />\n  </Blobs>')
    x = x.replace("</Packs>", f'  <Pack Hash="p{h}" Size="{nbytes}" CompressedSize="{nbytes}" RemotePath="x" />\n  </Packs>')
    with open(m, "w") as f:
        f.write(x)
    return path


def write_file(root, name, nbytes):
    path = os.path.join(root, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(os.urandom(nbytes))
    return path


def allocated(path):
    return os.lstat(path).st_blocks * 512


@unittest.skipUnless(shutil.which("git"), "needs git to tell tracked files apart")
class BuildOutputCredit(unittest.TestCase):
    """Review on PR #2: bytes already in Engine/Binaries/Mac etc. may be downloaded dependencies or caches,
    not products of this build, and must not shrink the reserved build-output budget."""

    BUILD_GB = 0.05  # 50 MB estimate, so a few MB of fixture files would visibly change it
    MB = 1_000_000

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-credit-")
        self.engine = make_engine(os.path.join(self.tmp, "engine"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args):
        subprocess.run(["git", "-C", self.engine, *args], check=True, capture_output=True)

    def build_need(self):
        out = os.path.join(self.tmp, "pf.json")
        r = subprocess.run([sys.executable, "-I", SCRIPT, "--engine", self.engine, "--exclude-file", EXCLUDES,
                            "--stage", "build", "--build-gb", str(self.BUILD_GB), "--ddc-path",
                            os.path.join(self.tmp, "ddc"), "--temp-path", self.tmp, "--swap-path", self.tmp,
                            "--simulate-free-gb", "100", "--out", out], capture_output=True, text=True)
        self.assertIn(r.returncode, (0, 3), r.stdout + r.stderr)
        import json
        rep = load_json(out)
        return rep["needs_bytes"]["engine"] - rep["dependencies"]["missing_unpacked_bytes"], rep

    def test_large_downloaded_file_in_binaries_mac_does_not_reduce_the_budget(self):
        self.git("init", "-q")
        f = add_downloaded_file(self.engine, "Engine/Binaries/Mac/libDownloaded.dylib", 8 * self.MB)
        need, rep = self.build_need()
        self.assertGreaterEqual(rep["build_output_credit"]["present_bytes"], allocated(f))
        self.assertEqual(rep["existing_build_output_bytes"], 0)
        self.assertEqual(need, self.BUILD_GB * GB)

    def test_tracked_and_cache_files_do_not_reduce_the_budget(self):
        self.git("init", "-q")
        write_file(self.engine, "Engine/Binaries/Mac/Tracked.sh", 4 * self.MB)
        self.git("add", "Engine/Binaries/Mac/Tracked.sh")
        write_file(self.engine, "Engine/DerivedDataCache/cache.udd", 6 * self.MB)
        need, rep = self.build_need()
        self.assertEqual(rep["existing_build_output_bytes"], 0)
        self.assertEqual(need, self.BUILD_GB * GB)

    def test_untracked_files_are_reported_but_not_deducted(self):
        # An untracked file is no proof that this build produced it (review follow-up): report, don't deduct.
        self.git("init", "-q")
        add_downloaded_file(self.engine, "Engine/Binaries/Mac/libDownloaded.dylib", 8 * self.MB)
        g = write_file(self.engine, "Engine/Intermediate/Build/Mac/UnrealEditor/Module.o", 3 * self.MB)
        need, rep = self.build_need()
        self.assertEqual(rep["build_output_credit"]["untracked_not_downloaded_bytes"], allocated(g))
        self.assertEqual(rep["existing_build_output_bytes"], 0)
        self.assertEqual(need, self.BUILD_GB * GB)

    def test_no_deduction_when_git_cannot_list_tracked_files(self):
        write_file(self.engine, "Engine/Intermediate/Build/Mac/UnrealEditor/Module.o", 3 * self.MB)  # no repo
        need, rep = self.build_need()
        self.assertEqual(rep["existing_build_output_bytes"], 0)
        self.assertEqual(need, self.BUILD_GB * GB)


class DerivedDataLocation(unittest.TestCase):
    """The DDC is budgeted where UE 5.8.4 writes it (ZenServerInterface.cpp DetermineDataPath)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-ddc-")
        self.engine = make_engine(os.path.join(self.tmp, "engine"))
        self.settings = os.path.join(self.tmp, "Epic")
        os.makedirs(os.path.join(self.settings, "Epic Games"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def store(self, text):
        with open(os.path.join(self.settings, "Epic Games", "KeyValueStore.ini"), "w") as f:
            f.write(text)

    def resolve(self, **env):
        return pf.resolve_ddc(self.engine, env=env, settings_dir=self.settings)

    def test_default_is_the_engine_agnostic_zen_directory(self):
        d = self.resolve()
        self.assertEqual(d["zen"], os.path.join(self.settings, "UnrealEngine", "Common", "Zen", "Data"))
        self.assertEqual(d["fallback"], os.path.join(os.path.realpath(self.engine), "Engine", "DerivedDataCache"))

    def test_zen_data_path_environment_is_followed(self):
        self.assertEqual(self.resolve(**{"UE-ZenDataPath": "/Volumes/X/zen"})["zen"], "/Volumes/X/zen")

    def test_local_data_cache_outside_the_engine_moves_zen_and_the_fallback(self):
        d = self.resolve(**{"UE-LocalDataCachePath": "/Volumes/X/ddc"})
        self.assertEqual((d["zen"], d["fallback"]), ("/Volumes/X/ddc/Zen", "/Volumes/X/ddc"))

    def test_local_data_cache_inside_the_engine_leaves_zen_at_its_default(self):
        inside = os.path.join(self.engine, "DDC")
        d = self.resolve(**{"UE-LocalDataCachePath": inside})
        self.assertTrue(d["zen"].endswith(os.path.join("Common", "Zen", "Data")))
        self.assertEqual(d["fallback"], inside)

    def test_stored_value_wins_over_zen_data_path_environment(self):
        self.store("[Unreal Engine/Identifiers]\nX=1\n[Zen]\nDataPath=/Volumes/Stored/zen\n")
        self.assertEqual(self.resolve(**{"UE-ZenDataPath": "/Volumes/X/zen"})["zen"], "/Volumes/Stored/zen")
        self.assertEqual(self.resolve(**{"UE-ZenSubprocessDataPath": "/Volumes/S"})["zen"], "/Volumes/S")

    def test_preflight_only_override_is_marked_and_fallback_on_another_pool_is_budgeted(self):
        ddc = os.path.join(self.tmp, "ddc")
        real = pf.pool_of
        fake = lambda p, _c=None: ("poolB", "/B", 500 * GB) if os.path.abspath(p).startswith(ddc) \
            else ("poolA", "/A", 500 * GB)
        out = os.path.join(self.tmp, "pf.json")
        argv = ["--engine", self.engine, "--exclude-file", EXCLUDES, "--stage", "build", "--ddc-path", ddc,
                "--temp-path", self.tmp, "--swap-path", self.tmp, "--out", out]
        import contextlib, io, json
        pf.pool_of = fake
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                pf.main(argv)
        finally:
            pf.pool_of = real
        rep = load_json(out)
        self.assertIn("preflight only", rep["ddc"]["zen_source"])
        self.assertEqual(rep["needs_bytes"]["ddc_fallback"], 10 * GB)   # Engine/DerivedDataCache is on poolA
        self.assertEqual({r["pool"] for r in rep["pools"]}, {"poolA", "poolB"})


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("perl"), "scripts/ue_build.sh targets macOS")
class BuildScriptGuard(unittest.TestCase):
    """scripts/ue_build.sh: refuses to start on a failed preflight and stops a step at the floor."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-guard-")
        self.logs = os.path.join(self.tmp, "logs")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_sh(self, engine, *args, **env):
        e = dict(os.environ, TITAN_UE_LOGS=self.logs, **env)
        return subprocess.run(["bash", BUILD_SH, "deps", "--engine", engine, *args], env=e,
                              capture_output=True, text=True, timeout=120)

    def test_refuses_to_start_on_simulated_low_space(self):
        engine = make_engine(os.path.join(self.tmp, "engine"))
        r = self.run_sh(engine, TITAN_TEST_SIMULATE_FREE_GB="36")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(os.path.join(engine, "gitdeps-ran")), "GitDependencies must not start")
        self.assertIn("refused to start", r.stdout)

    def test_floor_cannot_be_lowered(self):
        engine = make_engine(os.path.join(self.tmp, "engine"))
        r = self.run_sh(engine, "--floor-gb", "10")
        self.assertEqual(r.returncode, 2)
        self.assertFalse(os.path.exists(os.path.join(engine, "gitdeps-ran")))

    def test_guard_stops_a_running_step_at_the_floor(self):
        if host_free_gb(self.tmp) < 40:
            self.skipTest("host has under 40 GB free, so the deps preflight cannot pass here")
        engine = make_engine(os.path.join(self.tmp, "engine"), gitdeps_body=f'sleep 30; touch "{self.tmp}/finished"')
        t0 = time.time()
        r = self.run_sh(engine, TITAN_TEST_GUARD_FREE_KB="1000", TITAN_TEST_GUARD_INTERVAL="1")
        self.assertEqual(r.returncode, 75, r.stdout + r.stderr)
        self.assertLess(time.time() - t0, 25, "the guard must stop the step, not wait for it")
        time.sleep(1)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "finished")), "the step must have been killed")
        log = [f for f in os.listdir(self.logs) if f.endswith("-deps.log")]
        with open(os.path.join(self.logs, log[0])) as f:
            text = f.read()
        self.assertIn("TITAN GUARD: disk floor 35 GB reached", text)
        self.assertIn("# guarded pools", text)

    def test_refuses_while_another_step_is_running(self):
        engine = make_engine(os.path.join(self.tmp, "engine"))
        os.makedirs(self.logs)
        sleeper = subprocess.Popen(["sleep", "30"])
        try:
            with open(os.path.join(self.logs, "20260101T000000Z-deps.log.pid"), "w") as f:
                f.write(str(sleeper.pid))
            r = self.run_sh(engine)
            self.assertEqual(r.returncode, 4, r.stdout + r.stderr)
            self.assertIn("still running", r.stderr)
        finally:
            sleeper.kill(); sleeper.wait()
        self.assertFalse(os.path.exists(os.path.join(engine, "gitdeps-ran")), "GitDependencies must not start")

    def hold_lock(self):
        """Another holder of the run lock: a process with an exclusive flock on ue_build.flock."""
        os.makedirs(self.logs, exist_ok=True)
        p = subprocess.Popen(["perl", "-MFcntl=:flock", "-e",
                              'open(my $f, ">>", $ARGV[0]) or die; flock($f, LOCK_EX) or die; '
                              '$| = 1; print "locked\\n"; sleep 60', os.path.join(self.logs, "ue_build.flock")],
                             stdout=subprocess.PIPE, text=True)
        self.assertEqual(p.stdout.readline().strip(), "locked")
        return p

    def test_refuses_while_the_lock_is_held(self):
        engine = make_engine(os.path.join(self.tmp, "engine"))
        holder = self.hold_lock()
        try:
            r = self.run_sh(engine)
        finally:
            holder.kill(); holder.wait()
        self.assertEqual(r.returncode, 4, r.stdout + r.stderr)
        self.assertIn("holds", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(engine, "gitdeps-ran")))

    def contenders(self, engine, n=2):
        env = dict(os.environ, TITAN_UE_LOGS=self.logs)
        procs = [subprocess.Popen(["bash", BUILD_SH, "deps", "--engine", engine], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(n)]
        out = [(p.wait(timeout=120), *p.communicate()) for p in procs]
        with open(os.path.join(engine, "runs")) as f:
            runs = f.read().split()
        return sorted(rc for rc, _, _ in out), runs, out

    def check_exclusive(self, engine, rounds=3):
        if host_free_gb(self.tmp) < 40:
            self.skipTest("host has under 40 GB free, so the deps preflight cannot pass here")
        for i in range(rounds):
            open(os.path.join(engine, "runs"), "w").close()
            rcs, runs, out = self.contenders(engine)
            self.assertEqual(rcs, [0, 4], f"round {i}: {out}")
            self.assertEqual(len(runs), 1, f"round {i}: both contenders ran GitDependencies: {out}")

    def test_two_concurrent_contenders_only_one_runs(self):
        engine = make_engine(os.path.join(self.tmp, "engine"), f'echo $$ >> "{self.tmp}/engine/runs"; sleep 3')
        self.check_exclusive(engine)

    def test_stale_lock_is_not_a_race(self):
        # Review on PR #2: two contenders that both find a stale lock must not both proceed.
        engine = make_engine(os.path.join(self.tmp, "engine"), f'echo $$ >> "{self.tmp}/engine/runs"; sleep 3')
        os.makedirs(self.logs)
        dead = subprocess.Popen(["true"]); dead.wait()
        with open(os.path.join(self.logs, "ue_build.flock"), "w") as f:
            f.write(str(dead.pid))                       # left behind by a holder that no longer exists
        os.makedirs(os.path.join(self.logs, "ue_build.lock"))  # and the old mkdir-style lock directory
        self.check_exclusive(engine)

    def test_killed_holder_releases_the_lock(self):
        engine = make_engine(os.path.join(self.tmp, "engine"))
        holder = self.hold_lock()
        holder.kill(); holder.wait()                     # SIGKILL: no cleanup code runs
        if host_free_gb(self.tmp) < 40:
            self.skipTest("host has under 40 GB free, so the deps preflight cannot pass here")
        r = self.run_sh(engine)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_step_runs_when_the_stage_preflight_passes(self):
        if host_free_gb(self.tmp) < 40:
            self.skipTest("host has under 40 GB free, so the deps preflight cannot pass here")
        engine = make_engine(os.path.join(self.tmp, "engine"))
        r = self.run_sh(engine)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.exists(os.path.join(engine, "gitdeps-ran")))


class BuildStepTargets(unittest.TestCase):
    """The build step must build ShaderCompileWorker with the editor (the editor cannot start without it)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-targets-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_build_step_builds_editor_and_shader_compile_worker(self):
        if host_free_gb(self.tmp) < 85:
            self.skipTest("host has under 85 GB free, so the build-stage preflight cannot pass here")
        engine = make_engine(os.path.join(self.tmp, "engine"))
        batch = os.path.join(engine, "Engine/Build/BatchFiles")
        os.makedirs(batch, exist_ok=True)
        argv_file = os.path.join(self.tmp, "ubt-argv")
        with open(os.path.join(batch, "RunUBT.sh"), "w") as f:  # fake UBT: records its arguments, one per line
            f.write(f'#!/bin/bash\nprintf "%s\\n" "$@" > "{argv_file}"\n')
        os.chmod(os.path.join(batch, "RunUBT.sh"), 0o755)
        project = os.path.join(self.tmp, "Host", "Host.uproject")
        os.makedirs(os.path.dirname(project))
        with open(project, "w") as f:
            f.write("{}")
        env = dict(os.environ, TITAN_UE_LOGS=os.path.join(self.tmp, "logs"))
        r = subprocess.run(["bash", BUILD_SH, "build", "--engine", engine, "--project", project, "--jobs", "2"],
                           env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(argv_file) as f:
            argv = f.read().splitlines()
        self.assertIn(f"-Target=TitanHostEditor Mac Development -Project={project}", argv)
        self.assertIn("-Target=ShaderCompileWorker Mac Development", argv)
        self.assertIn("-MaxParallelActions=2", argv)


class PluginSync(unittest.TestCase):
    """scripts/sync_ue_plugins.sh against a fake engine root; the repo's own plugin sources are only read."""

    SYNC_SH = os.path.join(ROOT, "scripts", "sync_ue_plugins.sh")

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="titan-sync-")
        self.engine = os.path.join(self.tmp, "UE")
        os.makedirs(os.path.join(self.engine, "Engine/Build"))
        with open(os.path.join(self.engine, "Engine/Build/Build.version"), "w") as f:
            f.write("{}")
        self.rel = "TitanStudioBridge/Source/TitanStudioBridge/Private/TitanBridgeFrames.cpp"
        self.src = os.path.join(ROOT, "unreal/Plugins", self.rel)
        self.dst = os.path.join(self.engine, "Engine/Plugins/TITAN", self.rel)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def sync(self):
        r = subprocess.run(["bash", self.SYNC_SH, self.engine], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_changed_file_is_newer_than_the_last_build(self):
        # Reproduces 20261109T132033Z-build.log: the engine copy holds previous content and an object was
        # just built from it; the repo file's own mtime is older than that build. After the sync the engine
        # copy must be at least as new as the sync itself, or UBT keeps the stale binary. (rsync -a gave it
        # the repo file's older mtime instead.)
        self.sync()
        with open(self.dst, "a") as f:
            f.write("\n// previous content\n")
        old = time.time() - 3600
        os.utime(self.dst, (old, old))
        time.sleep(1.1)  # the "build" happens strictly after the repo file was last modified
        start = time.time()
        self.assertLess(os.path.getmtime(self.src), start)
        self.sync()
        with open(self.src, "rb") as a, open(self.dst, "rb") as b:
            self.assertEqual(a.read(), b.read())
        self.assertGreaterEqual(os.path.getmtime(self.dst), start - 0.01)

    def test_unchanged_file_keeps_its_time(self):
        # A sync with nothing to do must not touch files, or every sync would force a bridge rebuild.
        self.sync()
        fixed = time.time() - 7200
        os.utime(self.dst, (fixed, fixed))
        self.sync()
        self.assertAlmostEqual(os.path.getmtime(self.dst), fixed, delta=1)

    def test_check_mode_reports_drift(self):
        self.sync()
        r = subprocess.run(["bash", self.SYNC_SH, "--check", self.engine], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(self.dst, "a") as f:
            f.write("// drift\n")
        r = subprocess.run(["bash", self.SYNC_SH, "--check", self.engine], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("DRIFT", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
