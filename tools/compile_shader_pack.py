"""Explicitly build the offline RadRay compiler tool and regenerate the committed shader pack."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import json

from native import msvc_environment, run
from shader_pack import PROGRAMS


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--radray-source', type=Path, default=root / 'external/RadRay')
    parser.add_argument('--parallel', type=int, default=8)
    parser.add_argument('--clean', action='store_true', help='Reconfigure and rebuild the offline tool from scratch')
    parser.add_argument('--output', type=Path, default=root / 'src/client/resources/assets/raynium/shaderpacks/default.rnsp')
    parser.add_argument('--program', action='append', help='Program name=logical/source.hlsl; repeat for multiple programs')
    parser.add_argument('--vanilla-shaders', type=Path, default=root / 'build/vanilla/shaders', help='Converted vanilla HLSL directory (required for the default pack)')
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error('--parallel must be positive')
    radray = args.radray_source.resolve()
    sdk = radray / 'SDKs/radray_dxc/extracted'
    if not (sdk / 'bin/dxcompiler.dll').is_file():
        parser.error('Restore the pinned RadRay DXC SDK explicitly: python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc')
    env = msvc_environment()
    path_key = next(key for key in env if key.casefold() == 'path')
    env[path_key] = str(sdk / 'bin') + os.pathsep + env[path_key]
    binary = root / 'build/shader-tools/Release'
    run(['cmake', '--preset', 'win-x64-release', '-B', str(binary), *(['--fresh'] if args.clean else []),
         '-DRADRAY_SOURCE_DIR=' + str(radray), '-DRADRAY_DXC_ROOT=' + str(sdk),
         '-DRAYNIUM_BUILD_SHADER_TOOLS=ON', '-DRAYNIUM_ENABLE_PROFILER=OFF'], root / 'native', env)
    run(['cmake', '--build', str(binary), '--target', 'raynium_shader_pack', '--parallel', str(args.parallel),
         *(['--clean-first'] if args.clean else [])], root, env)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.shader-pack-', dir=output.parent) as directory:
        temporary = Path(directory) / output.name
        command = [str(binary / 'bin/raynium_shader_pack.exe'), '--root', str(root if not args.program else root / 'native/shaders'),
                   '--include', str(root / 'native/shaders'), '--include', str(radray / 'shaderlib'), '--output', str(temporary)]
        programs = args.program
        if programs is None:
            manifest = args.vanilla_shaders / 'interfaces.json'
            if not manifest.is_file():
                raise ValueError('Convert the locked vanilla shaders before regenerating the complete default pack')
            interfaces = json.loads(manifest.read_text(encoding='utf-8'))
            interfaces += json.loads((args.vanilla_shaders/'post-interfaces.json').read_text(encoding='utf-8'))
            if any('error' in entry for entry in interfaces):
                raise ValueError('The vanilla conversion contains failed variants')
            source = args.vanilla_shaders.resolve().relative_to(root).as_posix()
            programs = [name+'=native/shaders/'+name+'.hlsl' for name in PROGRAMS]
            programs += [name+'='+source+'/'+name+'.hlsl' for name in sorted({entry['program'] for entry in interfaces})]
        for program in programs:
            command.extend(['--program', program])
        run(command, root, env)
        if not output.exists() or output.read_bytes() != temporary.read_bytes():
            os.replace(temporary, output)
    print(f'Shader pack ready: {output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as failure:
        print(f'error: {failure}', file=sys.stderr)
        sys.exit(1)
