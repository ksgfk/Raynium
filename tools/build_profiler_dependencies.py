"""Prepare a Tracy client with explicit lifetime for the unloadable bridge DLL."""

import argparse
from pathlib import Path
import subprocess
import sys

from native import msvc_environment, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Release')
    parser.add_argument('--parallel', type=int, default=8)
    parser.add_argument('--radray-source', type=Path)
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error('--parallel must be positive')
    root = Path(__file__).resolve().parent.parent
    source = (args.radray_source or root / 'external/RadRay').resolve() / 'third_party/tracy'
    if not (source / 'CMakeLists.txt').is_file():
        raise RuntimeError('Explicitly restore the pinned Tracy source before building.')
    output = root / 'build/native-dependencies/tracy' / args.config
    env = msvc_environment()
    build = output / 'build-manual-lifetime'
    run(['cmake', '-S', str(root / 'native/cmake/profiler-dependency'), '-B', str(build), '-G', 'Ninja',
         '-DTRACY_SOURCE_DIR=' + source.as_posix(),
         '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
         '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
         '-DCMAKE_BUILD_TYPE=' + args.config,
         '-DCMAKE_MSVC_RUNTIME_LIBRARY=' + ('MultiThreadedDebug' if args.config == 'Debug' else 'MultiThreaded'),
         '-DCMAKE_INSTALL_PREFIX=' + (output / 'sdk').as_posix(), '-DCMAKE_INSTALL_LIBDIR=lib',
         '-DTRACY_ENABLE=ON', '-DTRACY_ON_DEMAND=ON', '-DTRACY_STATIC=ON',
         '-DTRACY_ONLY_LOCALHOST=ON', '-DTRACY_NO_BROADCAST=ON', '-DTRACY_NO_CRASH_HANDLER=ON',
         '-DTRACY_MANUAL_LIFETIME=ON'], root, env)
    run(['cmake', '--build', str(build), '--parallel', str(args.parallel)], root, env)
    run(['cmake', '--install', str(build)], root, env)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as failure:
        print(f'error: {failure}', file=sys.stderr)
        sys.exit(1)
