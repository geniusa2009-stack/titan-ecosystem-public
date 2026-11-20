#!/usr/bin/env python3
"""Disk and toolchain preflight for each step of building the TITAN fork of Unreal Engine 5.8.4 on macOS.

Original TITAN code. Measures (does not guess) everything it can:
  * the exact dependency bytes still missing, from Engine/Build/Commit.gitdeps.xml, using the same
    exclusion semantics as GitDependencies (Program.cs IsExcludedFolder),
  * free space of every storage pool a step writes to: the engine tree, the derived data cache
    (resolved the way UE 5.8.4's ZenServerInterface.cpp DetermineDataPath does; by default
    ~/Library/Application Support/Epic/UnrealEngine/Common/Zen/Data, outside the engine tree), the
    Local file-system DDC that Unreal falls back to when Zen is unavailable, the per-user temporary
    directory and the swap volume. APFS volumes in one container share free space, so they form one
    pool and their needs are added up,
  * how much of each engine tree is APFS-private (unshared clone blocks), Xcode, CPU, memory, swap.

What a step will write cannot be measured before it runs, so each --stage has explicit, documented
estimates (override with --build-gb/--ddc-gb/--temp-gb/--swap-gb):

  stage         needs (on top of the dependencies still missing)
  deps          temp 1 GB (GitDependencies with --no-cache writes files in place)
  plan          UBT/UHT binaries + rules assemblies 1 GB, temp 1 GB
  bridge-check  UHT output + the PCHs and objects of the TITAN modules 6 GB, temp 1 GB, swap 2 GB
  build         editor build output 25 GB, DDC 10 GB for the first editor launch, temp 3 GB, swap
                growth 8 GB (= unified memory of the reference M3)

The build-output estimate is never reduced by what already exists. Files in Engine/Intermediate and
Engine/Binaries/Mac may be downloaded dependencies, git-tracked files, caches or leftovers of other
targets; even an untracked file is no proof that it belongs to this build. They are measured and reported
(present / untracked-and-not-downloaded), not deducted. Over-reserving by an earlier partial build is
the safe direction.

Every affected pool must keep --headroom-gb (35 GB, cannot be lowered) free after the step. The
build script's disk guard enforces the same floor on every pool while the step runs.

Exit codes: 0 = PASS, 3 = FAIL (not enough space), 2 = bad input.
Run with `python3 -I` (the engine tree is third-party data).
"""
import argparse
import ctypes
import ctypes.util
import json
import os
import platform
import plistlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

GB = 1_000_000_000
MIN_HEADROOM_GB = 35.0    # the project's disk-safety floor; never lowered

# Per-stage estimates (GB) for a project-scoped (DisableEnginePluginsByDefault) UnrealEditor Development
# build with -NoDebugInfo on Apple Silicon. Basis: docs/BUILD_INSTRUCTIONS.md, "Disk budget".
STAGES = {
    "deps":         {"build": 0.0,  "ddc": 0.0,  "temp": 1.0, "swap": 0.0},
    "plan":         {"build": 1.0,  "ddc": 0.0,  "temp": 1.0, "swap": 0.0},
    "bridge-check": {"build": 6.0,  "ddc": 0.0,  "temp": 1.0, "swap": 2.0},
    "build":        {"build": 25.0, "ddc": 10.0, "temp": 3.0, "swap": 8.0},
}
# Where an editor build writes. Measured for the report only; never deducted from the estimate.
BUILD_OUTPUT_DIRS = ("Engine/Intermediate", "Engine/Binaries/Mac")


def read_excludes(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(line.replace("\\", "/").rstrip("/"))
    return out


def is_excluded(name, excludes):
    rooted = ("/" + name).lower()
    for e in excludes:
        el = e.lower()
        if el.startswith("/"):
            if rooted.startswith(el + "/"):
                return True
        elif ("/" + el + "/") in rooted:
            return True
    return False


def dependency_budget(engine, excludes, names_out=None):
    root = ET.parse(os.path.join(engine, "Engine/Build/Commit.gitdeps.xml")).getroot()
    blobs = {b.get("Hash"): (int(b.get("Size")), b.get("PackHash")) for b in root.iter("Blob")}
    packs = {p.get("Hash"): int(p.get("CompressedSize")) for p in root.iter("Pack")}
    total_files = selected = present = 0
    unpacked = missing = 0
    needed_packs = set()
    for f in root.iter("File"):
        total_files += 1
        name = f.get("Name")
        if names_out is not None:
            names_out.add(name)
        if is_excluded(name, excludes):
            continue
        size, pack = blobs[f.get("Hash")]
        selected += 1
        unpacked += size
        p = os.path.join(engine, name)
        try:
            if os.path.getsize(p) == size:
                present += 1
                continue
        except OSError:
            pass
        missing += size
        needed_packs.add(pack)
    return {
        "manifest_files": total_files,
        "selected_files": selected,
        "already_present_files": present,
        "selected_unpacked_bytes": unpacked,
        "missing_unpacked_bytes": missing,
        "download_bytes": sum(packs[p] for p in needed_packs),
    }


class AttrList(ctypes.Structure):
    _fields_ = [("bitmapcount", ctypes.c_ushort), ("reserved", ctypes.c_ushort),
                ("commonattr", ctypes.c_uint), ("volattr", ctypes.c_uint), ("dirattr", ctypes.c_uint),
                ("fileattr", ctypes.c_uint), ("forkattr", ctypes.c_uint)]


class PrivBuf(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("length", ctypes.c_uint32), ("private_size", ctypes.c_int64)]


def apfs_private_bytes(tree):
    """Sum of ATTR_CMNEXT_PRIVATESIZE: bytes not shared with any APFS clone."""
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    al = AttrList(5, 0, 0, 0, 0, 0, 0x00000008)  # ATTR_CMNEXT_PRIVATESIZE
    opts = 0x00000001 | 0x00000020               # FSOPT_NOFOLLOW | FSOPT_ATTR_CMN_EXTENDED
    buf = PrivBuf()
    logical = private = files = failures = 0
    for d, _, names in os.walk(tree):
        for n in names:
            p = os.path.join(d, n)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if not os.path.isfile(p) or os.path.islink(p):
                continue
            files += 1
            logical += st.st_size
            if libc.getattrlist(os.fsencode(p), ctypes.byref(al), ctypes.byref(buf), ctypes.sizeof(buf), opts) == 0:
                private += buf.private_size
            else:
                failures += 1
    return {"files": files, "logical_bytes": logical, "private_bytes": private, "failures": failures}


def tool_version(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def existing_ancestor(path):
    path = os.path.abspath(path)
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return path


EPIC_SETTINGS = "~/Library/Application Support/Epic"   # FApplePlatformProcess::ApplicationSettingsDir()


def stored_value(store, section, key, settings_dir=EPIC_SETTINGS):
    """FGenericPlatformMisc::GetStoredValue on macOS: <ApplicationSettingsDir>/<store>/KeyValueStore.ini."""
    path = os.path.join(os.path.expanduser(settings_dir), store, "KeyValueStore.ini")
    current = None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("[") and line.endswith("]"):
                    current = line[1:-1]
                elif current == section and "=" in line:
                    k, v = line.split("=", 1)
                    if k.strip() == key and v.strip():
                        return v.strip()
    except OSError:
        pass
    return None


def resolve_ddc(engine, env=None, settings_dir=EPIC_SETTINGS):
    """Where UE 5.8.4 will write derived data, following ZenServerInterface.cpp DetermineDataPath and
    DetermineLocalDataCachePath ([Zen.AutoLaunch] in BaseEngine.ini) and the Local FileSystem store
    (BaseEngine.ini [DerivedDataCacheStores] Local=, Path=%ENGINEDIR%DerivedDataCache).
    Returns {"zen": path, "zen_source": str, "fallback": path, "fallback_source": str}.
    Not modelled: -ZenDataPath= / -LocalDataCachePath= command-line overrides (TITAN never passes them)
    and the editor-preferences LocalDerivedDataCache setting (reported as a warning when present)."""
    env = os.environ if env is None else env
    root = os.path.realpath(engine)
    local, local_src = None, None
    if env.get("UE-LocalDataCachePath"):
        local, local_src = env["UE-LocalDataCachePath"], "environment UE-LocalDataCachePath"
    stored = stored_value("Epic Games", "GlobalDataCachePath", "UE-LocalDataCachePath", settings_dir)
    if stored:
        local, local_src = stored, "KeyValueStore GlobalDataCachePath"
    if local and local.lower() == "none":
        local = None
    zen = None
    for value, src in ((env.get("UE-ZenSubprocessDataPath"), "environment UE-ZenSubprocessDataPath"),
                       (stored_value("Epic Games", "Zen", "DataPath", settings_dir), "KeyValueStore Zen DataPath"),
                       (env.get("UE-ZenDataPath"), "environment UE-ZenDataPath")):
        if value:
            zen, zen_src = value, src
            break
    if zen is None and local and not os.path.realpath(local).startswith(root + os.sep):
        zen, zen_src = os.path.join(local, "Zen"), f"{local_src} + /Zen"
    if zen is None:
        zen = os.path.join(os.path.expanduser(settings_dir), "UnrealEngine", "Common", "Zen", "Data")
        zen_src = "BaseEngine.ini [Zen.AutoLaunch] DataPath (default)"
    fallback = local or os.path.join(root, "Engine", "DerivedDataCache")
    return {"zen": os.path.abspath(zen), "zen_source": zen_src,
            "fallback": os.path.abspath(fallback),
            "fallback_source": local_src or "BaseEngine.ini Local Path=%ENGINEDIR%DerivedDataCache (default)"}


def default_temp_path():
    return tool_version(["getconf", "DARWIN_USER_TEMP_DIR"]) or os.environ.get("TMPDIR") or "/tmp"


def default_swap_path():
    return "/System/Volumes/VM" if os.path.isdir("/System/Volumes/VM") else "/"


def pool_of(path, _cache={}):
    """(pool id, mount point, free bytes) of the storage that `path` (or its nearest existing ancestor)
    lives on. APFS volumes are pooled by container (Data, VM and Preboot share one free-space figure);
    other file systems are pooled by device."""
    p = existing_ancestor(path)
    st = os.stat(p)
    if st.st_dev not in _cache:
        vfs = os.statvfs(p)
        free = vfs.f_bavail * vfs.f_frsize
        ident, mount = f"dev{st.st_dev}", p
        df = (tool_version(["df", "-P", p]) or "").splitlines()
        m = re.match(r"^(\S+)\s+\d+\s+\d+\s+\d+\s+\d+%\s+(.*)$", df[-1]) if len(df) >= 2 else None
        if m:
            device, mount = m.group(1), m.group(2)
            ident = device.removeprefix("/dev/")
            info = tool_version(["diskutil", "info", "-plist", device]) if device.startswith("/dev/") else None
            if info:
                try:
                    d = plistlib.loads(info.encode())
                    ident = d.get("APFSContainerReference") or d.get("DeviceIdentifier") or ident
                except Exception:
                    pass
        _cache[st.st_dev] = (ident, mount, free)
    return _cache[st.st_dev]


def evaluate(needs, locations, pools, headroom_bytes):
    """Pure budget check. needs: {location: bytes}; locations: {location: pool id};
    pools: {pool id: {"free": bytes, "mount": str}}. Every pool a step writes to must keep headroom."""
    rows = {}
    for loc, need in needs.items():
        if need <= 0:
            continue  # the step does not write there: that pool is not affected
        pid = locations[loc]
        r = rows.setdefault(pid, {"pool": pid, "mount": pools[pid]["mount"], "free_bytes": pools[pid]["free"],
                                  "need_bytes": 0, "needs": {}})
        r["need_bytes"] += need
        r["needs"][loc] = need
    for r in rows.values():
        r["projected_free_bytes"] = r["free_bytes"] - r["need_bytes"]
        r["headroom_bytes"] = headroom_bytes
        r["ok"] = r["projected_free_bytes"] >= headroom_bytes
        r["short_bytes"] = max(0, headroom_bytes - r["projected_free_bytes"])
    return list(rows.values()), bool(rows) and all(r["ok"] for r in rows.values())


def du_bytes(trees, skip=None):
    total = 0
    for t in trees:
        for d, _, names in os.walk(t):
            for n in names:
                p = os.path.join(d, n)
                if skip and skip(p):
                    continue
                try:
                    total += os.lstat(p).st_blocks * 512
                except OSError:
                    pass
    return total


def git_tracked(engine, dirs):
    """Set of git-tracked paths (relative, '/'-separated) under dirs, or None when git cannot tell."""
    try:
        r = subprocess.run(["git", "-C", engine, "ls-files", "-z", "--", *dirs],
                           capture_output=True, timeout=300)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    return {n.decode("utf-8", "surrogateescape") for n in r.stdout.split(b"\0") if n}


def existing_build_outputs(engine, manifest_names):
    """What is already in BUILD_OUTPUT_DIRS, for the report. Nothing of it is deducted from the estimate:
    an untracked file is not proof that this build produced it or will reuse it."""
    present = du_bytes(os.path.join(engine, s) for s in BUILD_OUTPUT_DIRS)
    tracked = git_tracked(engine, BUILD_OUTPUT_DIRS)
    untracked = None
    if tracked is not None:
        known = {n.lower() for n in manifest_names} | {n.lower() for n in tracked}
        rel = lambda p: os.path.relpath(p, engine).replace(os.sep, "/").lower()
        untracked = du_bytes((os.path.join(engine, s) for s in BUILD_OUTPUT_DIRS), skip=lambda p: rel(p) in known)
    return {"present_bytes": present, "untracked_not_downloaded_bytes": untracked, "deducted_bytes": 0,
            "basis": "nothing deducted (conservative)"}


def memory_state():
    out = {"memory_bytes": int(tool_version(["sysctl", "-n", "hw.memsize"]) or 0)}
    swap = tool_version(["sysctl", "-n", "vm.swapusage"]) or ""
    parts = swap.replace("=", " ").split()
    for key in ("total", "used", "free"):
        if key in parts:
            v = parts[parts.index(key) + 1]
            out[f"swap_{key}_bytes"] = int(float(v.rstrip("MG")) * (1 << 20 if v.endswith("M") else 1 << 30))
    mp = tool_version(["memory_pressure", "-Q"]) or ""
    for line in mp.splitlines():
        if "free percentage" in line:
            out["memory_free_percent"] = int(line.rsplit(":", 1)[1].strip().rstrip("%"))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True, help="UE 5.8.4 working copy that will be built")
    ap.add_argument("--exclude-file", required=True)
    ap.add_argument("--stage", choices=sorted(STAGES), default="build",
                    help="step to budget for (default: build, the most demanding)")
    ap.add_argument("--headroom-gb", type=float, default=MIN_HEADROOM_GB,
                    help=f"free space every affected pool must keep (minimum {MIN_HEADROOM_GB:g})")
    ap.add_argument("--build-gb", type=float)
    ap.add_argument("--ddc-gb", type=float)
    ap.add_argument("--temp-gb", type=float)
    ap.add_argument("--swap-gb", type=float)
    ap.add_argument("--ddc-path", help="testing only: budget derived data here instead of where Unreal will "
                    "write it (Unreal is not told; the report marks it)")
    ap.add_argument("--temp-path", help="temporary directory (default: DARWIN_USER_TEMP_DIR)")
    ap.add_argument("--swap-path", help="swap volume (default: /System/Volumes/VM)")
    ap.add_argument("--simulate-free-gb", type=float,
                    help="testing: treat every pool as having at most this much free space (can only lower it)")
    ap.add_argument("--apfs-tree", action="append", default=[], help="tree to measure APFS-private bytes for")
    ap.add_argument("--out", help="write the JSON report here")
    ap.add_argument("--guard-out", help="write one existing path per affected pool here (for the disk guard)")
    a = ap.parse_args(argv)

    if a.headroom_gb < MIN_HEADROOM_GB:
        print(f"preflight: --headroom-gb {a.headroom_gb:g} is below the {MIN_HEADROOM_GB:g} GB safety floor",
              file=sys.stderr)
        return 2
    engine = os.path.realpath(a.engine)
    if not os.path.isfile(os.path.join(engine, "Engine/Build/Commit.gitdeps.xml")):
        print("preflight: not a UE source tree: " + engine, file=sys.stderr)
        return 2
    excludes = read_excludes(a.exclude_file)
    manifest_names = set()
    deps = dependency_budget(engine, excludes, manifest_names)

    est = dict(STAGES[a.stage])
    for k in est:
        v = getattr(a, f"{k}_gb")
        if v is not None:
            if v < 0:
                print(f"preflight: --{k}-gb must not be negative", file=sys.stderr)
                return 2
            est[k] = v
    credit = existing_build_outputs(engine, manifest_names) if est["build"] else \
        {"present_bytes": 0, "untracked_not_downloaded_bytes": 0, "deducted_bytes": 0, "basis": "no build output"}
    existing_build = credit["deducted_bytes"]   # always 0: see the module docstring
    build_bytes = est["build"] * GB

    ddc = resolve_ddc(engine)
    if a.ddc_path:
        ddc["zen"], ddc["zen_source"] = os.path.abspath(a.ddc_path), "--ddc-path (preflight only; Unreal does not use it)"
    paths = {
        "engine": engine,
        "ddc": ddc["zen"],
        "ddc_fallback": ddc["fallback"],
        "temp": os.path.abspath(a.temp_path or default_temp_path()),
        "swap": os.path.abspath(a.swap_path or default_swap_path()),
    }
    needs = {
        "engine": deps["missing_unpacked_bytes"] + build_bytes,
        "ddc": est["ddc"] * GB,
        "ddc_fallback": 0.0,
        "temp": est["temp"] * GB,
        "swap": est["swap"] * GB,
    }
    # Unreal writes the Local file-system DDC when Zen is unavailable. On the same pool as Zen that is
    # the same budget; on another pool it must be able to take the whole DDC estimate too.
    if est["ddc"] and pool_of(paths["ddc_fallback"])[0] != pool_of(paths["ddc"])[0]:
        needs["ddc_fallback"] = est["ddc"] * GB
    locations, pools, guard = {}, {}, {}
    for loc, p in paths.items():
        pid, mount, free = pool_of(p)
        if a.simulate_free_gb is not None:
            free = min(free, int(a.simulate_free_gb * GB))
        locations[loc] = pid
        entry = pools.setdefault(pid, {"free": free, "mounts": []})
        if mount not in entry["mounts"]:
            entry["mounts"].append(mount)
        entry["mount"] = ", ".join(entry["mounts"])
        if needs[loc] > 0:
            guard.setdefault(pid, existing_ancestor(p))
    rows, ok = evaluate(needs, locations, pools, a.headroom_gb * GB)
    engine_row = next((r for r in rows if r["pool"] == locations["engine"]),
                      {"free_bytes": pools[locations["engine"]]["free"],
                       "projected_free_bytes": pools[locations["engine"]]["free"]})

    report = {
        "stage": a.stage,
        "engine": engine,
        "paths": paths,
        "dependencies": deps,
        "dependencies_complete": deps["missing_unpacked_bytes"] == 0,
        "exclude_count": len(excludes),
        "estimates_gb": est,
        "existing_build_output_bytes": existing_build,
        "build_output_credit": credit,
        "ddc": ddc,
        "needs_bytes": needs,
        "pools": rows,
        "headroom_required_bytes": a.headroom_gb * GB,
        "simulated_free_gb": a.simulate_free_gb,
        # Kept for older readers: the engine pool's figures.
        "volume_free_bytes": engine_row["free_bytes"],
        "projected_free_after_build_bytes": engine_row["projected_free_bytes"],
        "result": "PASS" if ok else "FAIL",
        "machine": {"arch": platform.machine(), "cpus": os.cpu_count(), "macos": platform.mac_ver()[0],
                    "xcode": tool_version(["xcodebuild", "-version"]), **memory_state()},
        "apfs": {t: apfs_private_bytes(t) for t in a.apfs_tree},
    }

    g = lambda b: f"{b / GB:.1f} GB"
    print(f"stage:                      {a.stage}")
    print(f"dependencies selected:      {deps['selected_files']} files, {g(deps['selected_unpacked_bytes'])} unpacked"
          f" ({len(excludes)} exclusions)")
    print(f"dependencies still missing: {g(deps['missing_unpacked_bytes'])} unpacked, {g(deps['download_bytes'])} download")
    print(f"estimates:                  build {g(build_bytes)} ({g(credit['present_bytes'])} already present,"
          f" {credit['basis']}), ddc {g(needs['ddc'])},"
          f" temp {g(needs['temp'])}, swap growth {g(needs['swap'])}")
    print(f"derived data (Zen):         {ddc['zen']} [{ddc['zen_source']}]")
    print(f"derived data fallback:      {ddc['fallback']} [{ddc['fallback_source']}]"
          + (f", budgeted {g(needs['ddc_fallback'])} (other pool)" if needs["ddc_fallback"] else ", same pool"))
    for r in rows:
        locs = ", ".join(f"{k} {g(v)}" for k, v in r["needs"].items())
        print(f"pool {r['pool']} ({r['mount']}): free {g(r['free_bytes'])}, needs {g(r['need_bytes'])} [{locs}],"
              f" projected {g(r['projected_free_bytes'])} -> {'ok' if r['ok'] else 'SHORT ' + g(r['short_bytes'])}"
              f" (headroom {g(a.headroom_gb * GB)})")
    m = report["machine"]
    if "swap_used_bytes" in m:
        print(f"memory:                     {g(m['memory_bytes'])} RAM, swap {g(m['swap_used_bytes'])} used"
              f" of {g(m.get('swap_total_bytes', 0))}, {m.get('memory_free_percent', '?')}% free")
    if a.simulate_free_gb is not None:
        print(f"SIMULATED: free space capped at {a.simulate_free_gb:g} GB per pool")
    for t, r in report["apfs"].items():
        print(f"APFS {t}: {r['files']} files, logical {g(r['logical_bytes'])}, private {g(r['private_bytes'])}")
    print(f"preflight ({a.stage}): " + report["result"])
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    if a.guard_out:
        with open(a.guard_out, "w", encoding="utf-8") as f:
            f.write("".join(p + "\n" for p in guard.values()))
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
