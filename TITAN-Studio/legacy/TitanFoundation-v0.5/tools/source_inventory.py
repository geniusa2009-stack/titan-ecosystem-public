#!/usr/bin/env python3
"""Read-only, local source tree inventory for comparing engine architecture without copying code."""
import argparse
import collections
import json
from pathlib import Path

EXTS = {'.h', '.hpp', '.inl', '.cpp', '.cc', '.c', '.rs', '.cs', '.gd', '.py', '.glsl', '.hlsl', '.shader', '.metal'}
SKIP = {'.git', '.svn', 'build', 'bin', 'obj', 'node_modules', '__pycache__', '.venv', '.godot', 'Intermediate', 'DerivedDataCache'}
CANDIDATE_DIRS = {
    'Unreal': ['Engine/Source/Runtime', 'Engine/Source/Editor', 'Engine/Shaders', 'Engine/Plugins'],
    'Godot': ['core', 'scene', 'servers', 'drivers', 'platform'],
    'Bevy': ['crates/bevy_ecs', 'crates/bevy_render', 'crates/bevy_pbr', 'crates/bevy_asset'],
    'O3DE': ['Gems/Atom', 'Code/Framework', 'Gems/EMotionFX', 'Gems/PhysX'],
    'Wicked': ['WickedEngine'],
    'Stride': ['sources/engine/Stride.Rendering', 'sources/engine/Stride.Graphics', 'sources/engine/Stride.Engine'],
}

def inventory(root: Path):
    counts = collections.Counter()
    total = 0
    sample = []
    for path in root.rglob('*'):
        if not path.is_file() or path.suffix.lower() not in EXTS:
            continue
        if any(part in SKIP for part in path.relative_to(root).parts):
            continue
        counts[path.suffix.lower()] += 1
        total += 1
        if len(sample) < 20:
            sample.append(str(path.relative_to(root)))
    matches = {name: [folder for folder in paths if (root / folder).is_dir()]
               for name, paths in CANDIDATE_DIRS.items()}
    return {
        'root': str(root.resolve()), 'total_source_files': total,
        'files_by_extension': dict(sorted(counts.items())),
        'identified_subsystem_paths': {k: v for k,v in matches.items() if v},
        'sample_paths': sample,
    }

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('repos', nargs='+', type=Path, help='Local engine repository folders (read-only)')
    p.add_argument('--output', type=Path, default=Path('reports/source_inventory.json'))
    args = p.parse_args()
    for repo in args.repos:
        if not repo.is_dir():
            p.error('Not a directory: ' + str(repo))
    output = [inventory(p) for p in args.repos]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for item in output:
        print(f"{item['root']} => {item['total_source_files']} source files; {list(item['identified_subsystem_paths'])}")
    print('Report:', args.output)

if __name__ == '__main__':
    main()
