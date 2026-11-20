#!/usr/bin/env python3
"""TITAN <-> Unreal Engine 5.8.4 source contract check. Original TITAN code.

Static, read-only verification that the supplied engine tree provides everything the TITAN
Unreal-side code depends on:
  * engine version is 5.8.x (decision D-1, 2026-11-05),
  * the local Xcode is inside the engine's supported range (Engine/Config/Apple/Apple_SDK.json),
  * every engine #include used by TITAN plugins resolves to a header in the tree,
  * every module named in TITAN *.Build.cs files exists,
  * the specific API signatures TITAN calls are present.

It does not compile anything; it is a fast pre-flight before a multi-hour UE build.
Usage: verify_ue_source_contract.py --engine <UE root> --titan <TITAN repo> [--xcode 26.1.1] [--out report.json]
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

PLUGIN_DIRS = ["unreal/Plugins", "legacy/TitanFoundation-v0.5/TitanEngine/Plugins"]

# (header relative to Engine/Source, exact text that must appear) — what TITAN code calls.
SIGNATURES = [
    ("Runtime/Core/Public/Containers/Ticker.h", "FDelegateHandle AddTicker(const TCHAR* InName, float InDelay, TUniqueFunction<bool(float)>&& InFunction)"),
    ("Runtime/Core/Public/Containers/Ticker.h", "static CORE_API void RemoveTicker(FDelegateHandle Handle)"),
    ("Runtime/Core/Public/Hash/CityHash.h", "uint64 CityHash64("),
    # Found by the first real bridge compile (2026-11-06): the condensed writer policy has its own header,
    # and FJsonObject keys are UE::FSharedString in 5.8 (TitanBridgeSession.cpp handles both key types).
    ("Runtime/Json/Public/Policies/CondensedJsonPrintPolicy.h", "struct TCondensedJsonPrintPolicy"),
    ("Runtime/Json/Public/Dom/JsonObject.h", "using FStringType = UE::FSharedString;"),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "const FGuid& GetActorGuid() const"),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "bool SetActorLocation("),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "bool SetActorRotation(FRotator"),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "void SetActorScale3D("),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "GetAttachParentActor()"),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "SetActorLabel("),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "SetIsTemporarilyHiddenInEditor("),
    ("Runtime/Engine/Classes/GameFramework/Actor.h", "IsHiddenEd()"),
    ("Editor/UnrealEd/Classes/Editor/EditorEngine.h", "FWorldContext &GetEditorWorldContext(bool bEnsureIsGWorld = false)"),
    ("Editor/UnrealEd/Classes/Editor/EditorEngine.h", "bool UndoTransaction(bool bCanRedo = true)"),
    ("Editor/UnrealEd/Classes/Editor/EditorEngine.h", "bool RedoTransaction()"),
    ("Editor/UnrealEd/Classes/Editor/UnrealEdEngine.h", "virtual void SelectActor("),
    ("Editor/UnrealEd/Classes/Editor/UnrealEdEngine.h", "virtual void SelectNone("),
    ("Editor/UnrealEd/Public/FileHelpers.h", "static UNREALED_API bool SaveCurrentLevel()"),
    ("Editor/UnrealEd/Public/ScopedTransaction.h", "class FScopedTransaction"),
    ("Runtime/Engine/Classes/Engine/LevelStreaming.h", "SetShouldBeLoaded("),
    ("Runtime/Engine/Classes/Engine/LevelStreaming.h", "SetShouldBeVisible("),
    ("Runtime/Engine/Classes/Engine/LevelStreaming.h", "IsLevelLoaded()"),
    ("Runtime/Engine/Classes/Engine/LevelStreaming.h", "IsLevelVisible()"),
    ("Runtime/Engine/Classes/Kismet/GameplayStatics.h", "GetStreamingLevel("),
    ("Runtime/Engine/Classes/Engine/World.h", "GetStreamingLevels()"),
]


def version_tuple(s):
    return tuple(int(x) for x in re.findall(r"\d+", s)[:3]) + (0,) * (3 - len(re.findall(r"\d+", s)[:3]))


def local_xcode():
    try:
        out = subprocess.run(["xcodebuild", "-version"], capture_output=True, text=True, timeout=30).stdout
        m = re.search(r"Xcode\s+([\d.]+)", out)
        return m.group(1) if m else None
    except (OSError, subprocess.SubprocessError):
        return None


def index_headers(engine):
    """Map of normalized header paths (relative to any include root) for suffix matching."""
    headers = set()
    for top in ("Engine/Source", "Engine/Plugins"):
        for dirpath, _dirs, files in os.walk(engine / top):
            for f in files:
                if f.endswith((".h", ".inl")):
                    headers.add((Path(dirpath) / f).relative_to(engine).as_posix())
    return headers


def resolves(include, headers):
    suffix = "/" + include
    return any(h.endswith(suffix) for h in headers)


def verify(engine, titan, xcode):
    issues, checked = [], {}
    version = json.loads((engine / "Engine/Build/Build.version").read_text(encoding="utf-8"))
    ver = f'{version["MajorVersion"]}.{version["MinorVersion"]}.{version["PatchVersion"]}'
    checked["engineVersion"] = ver
    if (version["MajorVersion"], version["MinorVersion"]) != (5, 8):
        issues.append(f"engine is {ver}; TITAN targets 5.8.x (decision D-1)")

    sdk_text = (engine / "Engine/Config/Apple/Apple_SDK.json").read_text(encoding="utf-8")
    sdk = {k: v for k, v in re.findall(r'"(MainVersion|MinVersion|MaxVersion)":\s*"([\d.]+)"', sdk_text)}
    checked["appleSdk"] = sdk
    checked["localXcode"] = xcode
    if xcode is None:
        issues.append("local Xcode version unknown")
    elif not (version_tuple(sdk["MinVersion"]) <= version_tuple(xcode) <= version_tuple(sdk["MaxVersion"])):
        issues.append(f"Xcode {xcode} outside UE range [{sdk['MinVersion']}, {sdk['MaxVersion']}]")

    sources = [p for d in PLUGIN_DIRS for p in (titan / d).rglob("*") if p.suffix in (".h", ".cpp", ".cs")]
    includes, modules = set(), set()
    for p in sources:
        text = p.read_text(encoding="utf-8", errors="replace")
        if p.suffix == ".cs":
            for block in re.findall(r"ModuleNames\.AddRange\(new string\[\]\s*\{([^}]*)\}", text):
                modules.update(re.findall(r'"([A-Za-z0-9_]+)"', block))
        else:
            includes.update(re.findall(r'#include\s+"([^"]+)"', text))
    own = {p.stem for p in sources}
    engine_includes = sorted(i for i in includes if not i.endswith(".generated.h") and Path(i).stem not in own)
    headers = index_headers(engine)
    missing_includes = [i for i in engine_includes if not resolves(i, headers)]
    checked["engineIncludes"] = len(engine_includes)
    issues += [f"include not found in engine: {i}" for i in missing_includes]

    titan_modules = {p.name[: -len(".Build.cs")] for p in sources if p.name.endswith(".Build.cs")}
    build_cs = {p.name[: -len(".Build.cs")] for top in ("Engine/Source", "Engine/Plugins")
                for p in (engine / top).rglob("*.Build.cs")}
    engine_modules = sorted(modules - titan_modules)
    checked["engineModules"] = engine_modules
    issues += [f"module not found in engine: {m}" for m in engine_modules if m not in build_cs]

    sig_ok = 0
    for rel, text in SIGNATURES:
        f = engine / "Engine/Source" / rel
        if not f.is_file():
            issues.append(f"missing header {rel}")
        elif text not in f.read_text(encoding="utf-8", errors="replace"):
            issues.append(f"{rel}: signature not found: {text}")
        else:
            sig_ok += 1
    checked["signaturesVerified"] = f"{sig_ok}/{len(SIGNATURES)}"
    return {"ok": not issues, "checked": checked, "issues": issues,
            "limitations": "static source check only; not a compile, link or runtime test"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, type=Path)
    ap.add_argument("--titan", required=True, type=Path)
    ap.add_argument("--xcode")
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    result = verify(a.engine.resolve(), a.titan.resolve(), a.xcode or local_xcode())
    text = json.dumps(result, indent=2, ensure_ascii=False)
    if a.out:
        a.out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["ok"] else 2


if __name__ == "__main__":
    sys.exit(main())
