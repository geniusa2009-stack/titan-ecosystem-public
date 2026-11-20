#!/usr/bin/env python3
"""Install the independent TITAN plugin inside an existing UE4.27.2 source tree.

Safe by default: validates and prints planned actions without writing anything.
Nothing is fetched or executed from an untrusted engine source tree.
"""
from __future__ import annotations
import argparse
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
SOURCE = HERE / 'TitanEngine' / 'Plugins' / 'TitanFoundation'
REL_DEST = Path('Engine/Plugins/Runtime/TitanFoundation')

class InstallError(Exception):
    pass


def validate_engine_root(path: Path):
    path = path.expanduser().resolve()
    file = path / 'Engine/Build/Build.version'
    if not file.is_file():
        raise InstallError('Not a UE source checkout: Engine/Build/Build.version missing')
    if not (path / 'Engine/Source/Runtime/Engine/Classes/Engine/LevelStreaming.h').is_file():
        raise InstallError('Expected UE4.27 streaming source header missing')
    try:
        obj = json.loads(file.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, ValueError) as exc:
        raise InstallError(f'Invalid Build.version: {exc}') from exc
    if (obj.get('MajorVersion'), obj.get('MinorVersion')) != (4, 27):
        raise InstallError('This integration only targets Unreal Engine 4.27.x')
    return path, obj


def install(engine: Path, apply: bool = False):
    engine, ver = validate_engine_root(engine)
    if not (SOURCE / 'TitanFoundation.uplugin').is_file():
        raise InstallError('TITAN plugin source missing in the package')
    dest = engine / REL_DEST
    if dest.exists() or dest.is_symlink():
        raise InstallError('TITAN destination exists. Refusing to overwrite; back it up or choose a clean checkout.')
    info = {
        'engine': str(engine),
        'detected_version': '.'.join(str(ver.get(k)) for k in ('MajorVersion','MinorVersion','PatchVersion')),
        'source_plugin': str(SOURCE),
        'destination': str(dest),
        'mode': 'install' if apply else 'dry-run',
        'writes_to_existing_unreal_files': False,
    }
    if apply:
        dest.parent.mkdir(parents=True, exist_ok=True)
        # Explicitly fail rather than replacing files in an existing checkout.
        shutil.copytree(SOURCE, dest, symlinks=False)
    return info


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine', type=Path, required=True, help='Path to existing UE4.27 source root')
    p.add_argument('--apply', action='store_true', help='Copy TITAN plugin after all checks pass')
    args = p.parse_args(argv)
    try:
        print(json.dumps(install(args.engine, args.apply), indent=2, ensure_ascii=False))
    except InstallError as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    return 0

if __name__ == '__main__':
    sys.exit(main())
