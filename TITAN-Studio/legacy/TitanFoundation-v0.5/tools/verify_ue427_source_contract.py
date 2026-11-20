#!/usr/bin/env python3
"""Non-executing validation for UE4.27 API compatibility with TITAN plugin.

Only reads local text source files. Does not invoke setup scripts, modify source,
access the network or claim to verify build compatibility.
"""
import argparse
import json
from pathlib import Path

REQUIRED = {
    'Engine/Source/Runtime/Engine/Classes/Engine/LevelStreaming.h': [
        'SetShouldBeLoaded(', 'SetShouldBeVisible(', 'IsLevelLoaded()', 'IsLevelVisible()'
    ],
    'Engine/Source/Runtime/Engine/Classes/Kismet/GameplayStatics.h': [
        'GetStreamingLevel('
    ],
    'Engine/Source/Runtime/Engine/Classes/Engine/World.h': [
        'GetStreamingLevels()'
    ],
}


def verify(root):
    root = Path(root)
    issues = []
    version_path = root / 'Engine/Build/Build.version'
    if not version_path.is_file():
        issues.append(f'missing: {version_path.relative_to(root)}')
        version = {}
    else:
        try:
            version = json.loads(version_path.read_text(encoding='utf-8'))
        except (OSError, UnicodeError, ValueError) as err:
            issues.append(f'bad Build.version: {err}')
            version = {}
    if (version.get('MajorVersion'), version.get('MinorVersion')) != (4, 27):
        issues.append('expected UE4.27')
    checked = []
    for relative, tokens in REQUIRED.items():
        file_path = root / relative
        if not file_path.is_file():
            issues.append(f'missing: {relative}')
            continue
        content = file_path.read_text(encoding='utf-8', errors='replace')
        missing = [t for t in tokens if t not in content]
        if missing:
            issues.append(f'{relative}: missing signatures {missing}')
        checked.append(relative)
    return {
        'compatible_at_source_signature_level': len(issues) == 0,
        'engine': '.'.join(str(version.get(k, '?')) for k in ('MajorVersion','MinorVersion','PatchVersion')),
        'verified_file_paths': checked,
        'issues': issues,
        'limitations': 'Source signatures only: not a UE compile/runtime/permissions test'
    }


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--engine', required=True, type=Path)
    cli.add_argument('--out', type=Path)
    args = cli.parse_args()
    result = verify(args.engine)
    output = json.dumps(result, indent=2, ensure_ascii=False)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output + '\n', encoding='utf-8')
    print(output)
    raise SystemExit(0 if result['compatible_at_source_signature_level'] else 2)
