"""Explicit offline /MTd dependency preparation for Raynium's pinned RadRay source."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

from native import compiler_console, msvc_environment, read_json, run, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parallel', type=int, default=8)
    parser.add_argument('--clean', action='store_true')
    parser.add_argument('--radray-source', type=Path)
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error('--parallel must be positive')
    root = Path(__file__).resolve().parent.parent
    radray = (args.radray_source or root / 'external/RadRay').resolve()
    output = root / 'build/native-dependencies/windows-x64-msvc-mtd'
    names = ('fmt', 'magic_enum', 'eigen', 'sigslot', 'stdexec', 'spdlog', 'xxHash', 'yyjson',
             'pugixml', 'mimalloc', 'Vulkan-Headers', 'volk', 'VulkanMemoryAllocator',
             'DirectX-Headers', 'D3D12MemoryAllocator', 'googletest')
    catalog = {entry['name']: entry for entry in read_json(radray / 'cmake/DependencyCatalog.json')}
    for name in names:
        if not (radray / 'third_party' / catalog[name]['source']).is_dir():
            raise RuntimeError(f'Missing {name} source; explicitly restore RadRay third-party sources first.')
        if any(dependency not in names[:names.index(name)] for dependency in catalog[name]['requires']):
            raise RuntimeError(f'Unsupported dependency order for {name}; update the Raynium preparation list.')
    # Only this dedicated /MTd tree is owned by the script; reject links before recursive cleanup.
    if output.resolve() != output.absolute() or not output.resolve().is_relative_to(root / 'build'):
        raise RuntimeError(f'Refusing dependency output through a link or outside build/: {output}')
    if args.clean and output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    identity = output / 'dependency-build.json'
    identity.unlink(missing_ok=True)
    env = msvc_environment()
    for name in names:
        entry = catalog[name]
        build = output / 'build' / name
        prefix = output / 'sdk' / name
        build.mkdir(parents=True, exist_ok=True)
        options = {key: {'${WIN32}': 'ON', '${RADRAY_SHARED_CRT}': 'OFF'}.get(value, value)
                   for key, value in entry['options'].items()}
        if name == 'mimalloc':
            # The Debug CRT owns its debug heap; explicit mi_* APIs remain available.
            options['MI_OVERRIDE'] = 'OFF'
        commands = [
            ['cmake', '-S', str(root / 'native/cmake/debug-dependency'), '-B', str(build), '-G', 'Ninja',
             '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', '-DCMAKE_BUILD_TYPE=Debug',
             '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug', '-DCMAKE_CXX_STANDARD=20',
             '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_INSTALL_PREFIX=' + prefix.as_posix(),
             '-DCMAKE_PREFIX_PATH=' + ';'.join((output / 'sdk' / dependency).as_posix() for dependency in entry['requires']),
             '-DRAYNIUM_DEPENDENCY_NAME=' + name,
             '-DRAYNIUM_DEPENDENCY_SOURCE=' + (radray / 'third_party' / entry['source']).as_posix(),
             *[f'-D{key}={value}' for key, value in options.items()]],
            ['cmake', '--build', str(build), '--parallel', str(args.parallel)],
            ['cmake', '--install', str(build)],
        ]
        print(f'Building {name}: Debug /MTd', flush=True)
        for phase, command in zip(('configure', 'build', 'install'), commands):
            log = build / (phase + '.log')
            with log.open('w', encoding='utf-8') as stream:
                result = subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, **compiler_console())
            if result.returncode:
                raise RuntimeError(f'{name} {phase} failed; see {log}')
    revision = run(['git', '-C', str(radray), 'rev-parse', 'HEAD'], root, env, True).stdout.strip()
    write_json(identity, {'crt': 'MTd', 'configuration': 'Debug', 'radrayCommit': revision, 'packages': list(names)})
    print(f'Debug /MTd dependencies ready: {output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as failure:
        print(f'error: {failure}', file=sys.stderr)
        sys.exit(1)
