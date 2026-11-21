"""Explicitly build the offline RadRay compiler tool and regenerate the committed shader pack."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import json
import shutil
import hashlib

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
    parser.add_argument('--vanilla-shaders', type=Path, default=root / 'build/vanilla/immediate', help='Converted vanilla HLSL directory (required for the default pack)')
    parser.add_argument('--install-tools', type=Path, help='Install the optional runtime shader toolchain into this directory')
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
    if args.install_tools:
        target = args.install_tools.resolve()
        target.mkdir(parents=True, exist_ok=True)
        for name in ('glslangValidator', 'spirv-cross'):
            exe = shutil.which(name)
            if not exe:
                raise ValueError('Missing shader tool: ' + name)
            shutil.copy2(exe, target / (name + '.exe'))
        for file in (binary / 'bin/raynium_shader_pack.exe', sdk / 'bin/dxcompiler.dll', sdk / 'bin/dxil.dll',
                     root / 'tools/compile_shader_override.py', root / 'tools/convert_vanilla_shaders.py'):
            shutil.copy2(file, target / file.name)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.shader-pack-', dir=output.parent) as directory:
        temporary = Path(directory) / output.name
        command = [str(binary / 'bin/raynium_shader_pack.exe'), '--root', str(root if not args.program else root / 'native/shaders'),
                   '--include', str(root / 'native/shaders'), '--include', str(radray / 'shaderlib'), '--output', str(temporary)]
        programs = args.program
        if programs is not None and len(programs) == 1:
            command.extend(['--standalone', programs[0].split('=', 1)[0]])
        if programs is None:
            immediate = args.vanilla_shaders
            programs = [name+'=native/shaders/'+name+'.hlsl' for name in PROGRAMS]
            catalog = {}
            for filename in ('interfaces.json', 'post-interfaces.json'):
                for entry in json.loads((immediate / filename).read_text(encoding='utf-8')):
                    key = entry['key']
                    record = {k: entry[k] for k in ('program', 'resources', 'inputs', 'pushBytes')}
                    if 'instanced' in entry:
                        record['instanced'] = entry['instanced']
                    catalog[key] = record
            identities = {e['id']: e['identity'] for e in json.loads((immediate.parent/'pipelines.json').read_text(encoding='utf-8'))}
            terrain = {}
            for entry in json.loads((immediate/'interfaces.json').read_text(encoding='utf-8')):
                identity = identities[entry['id']]
                if all(stage == 'minecraft:core/terrain' for stage in identity['shaders'].values()):
                    defines = {**dict.fromkeys(identity['defineFlags'], ''), **identity['defineValues']}
                    fields = [field for pair in sorted(defines.items()) for field in pair]
                    terrain[hashlib.sha256(chr(0).join(fields).encode('utf-8')).hexdigest()] = entry['key']
            prefix = immediate.resolve().relative_to(root).as_posix()
            programs += [f"{e['program']}={prefix}/{e['program']}.hlsl" for e in catalog.values()]
            programs += [f"{e['instanced']['program']}={prefix}/{e['instanced']['program']}.hlsl" for e in catalog.values() if 'instanced' in e]
        manifest_path = Path(directory) / 'program-list.txt'
        manifest_path.write_text('\n'.join(programs), encoding='utf-8')
        command.extend(['--program-list', str(manifest_path)])
        run(command, root, env)
        if not output.exists() or output.read_bytes() != temporary.read_bytes():
            os.replace(temporary, output)
        if args.program is None:
            output.with_name('terrain-programs.json').write_text(json.dumps(terrain, sort_keys=True)+'\n', encoding='utf-8', newline='\n')
            output.with_suffix('.json').write_text(json.dumps(catalog, sort_keys=True, separators=(',', ':'))+'\n', encoding='utf-8', newline='\n')
    print(f'Shader pack ready: {output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as failure:
        print(f'error: {failure}', file=sys.stderr)
        sys.exit(1)
