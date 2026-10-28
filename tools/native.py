"""Explicit native build, staging and mod packaging. Never downloads dependencies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            checksum.update(chunk)
    return checksum.hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def run(arguments, cwd, environment, capture=False):
    search_path = next((value for key, value in environment.items() if key.casefold() == 'path'), '')
    executable = shutil.which(arguments[0], path=search_path)
    if executable is None:
        raise RuntimeError(f'Required tool is not on PATH: {arguments[0]}')
    return subprocess.run([executable, *arguments[1:]], cwd=cwd, env=environment, check=True, text=True,
                          encoding='utf-8', errors='replace', capture_output=capture)


def msvc_environment():
    if os.name != 'nt':
        raise RuntimeError('Native builds currently require Windows x64.')
    environment = os.environ.copy()
    if environment.get('VSCMD_ARG_TGT_ARCH', '').lower() == 'x64' and shutil.which('cl', path=environment.get('PATH')):
        return environment
    installer = Path(environment.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not installer.is_file():
        raise RuntimeError('Install Visual Studio C++ x64 tools, or start an x64 Developer PowerShell.')
    installation = run([str(installer), '-latest', '-products', '*', '-requires',
                        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                       installer.parent, environment, True).stdout.strip()
    devcmd = Path(installation) / 'Common7/Tools/VsDevCmd.bat'
    if not installation or not devcmd.is_file() or any(char in str(devcmd) for char in '"%!\r\n'):
        raise RuntimeError('Cannot locate a supported Visual Studio x64 developer environment.')
    comspec = environment.get('COMSPEC', 'C:/Windows/System32/cmd.exe')
    command = f'"{comspec}" /u /d /s /c ""{devcmd}" -no_logo -arch=x64 -host_arch=x64 >nul && set"'
    result = subprocess.run(command, env=environment, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError('VsDevCmd failed; run from an x64 Developer PowerShell.')
    # Do not log this output: it contains the caller's environment.
    for line in result.stdout.decode('utf-16-le').splitlines():
        key, separator, value = line.partition('=')
        if separator and key:
            for existing in list(environment):
                if existing.casefold() == key.casefold():
                    del environment[existing]
            environment[key] = value
    return environment


def checked_dependencies(dll, environment):
    text = run(['dumpbin', '/nologo', '/dependents', str(dll)], dll.parent, environment, True).stdout
    names = sorted(set(re.findall(r'^\s*([\w.-]+\.dll)\s*$', text, re.MULTILINE | re.IGNORECASE)))
    if not names:
        raise RuntimeError('dumpbin returned no DLL dependencies; cannot classify the payload.')
    system = {'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
              'ole32.dll', 'oleaut32.dll', 'shell32.dll', 'shlwapi.dll', 'ws2_32.dll', 'bcrypt.dll',
              'crypt32.dll', 'secur32.dll', 'version.dll', 'winmm.dll', 'd3d12.dll', 'dxgi.dll',
              'd3dcompiler_47.dll', 'vulkan-1.dll', 'setupapi.dll', 'cfgmgr32.dll', 'userenv.dll',
              'psapi.dll', 'dbghelp.dll', 'dwmapi.dll', 'imm32.dll', 'wintrust.dll'}
    for name in names:
        lower = name.lower()
        if lower.startswith(('vcruntime', 'msvcp', 'ucrtbase', 'api-ms-win-crt-')):
            raise RuntimeError(f'{name} is a dynamic CRT dependency; rebuild bridge and dependencies with /MT.')
        if lower not in system and not lower.startswith(('api-ms-win-', 'ext-ms-win-')):
            raise RuntimeError(f'Unclassified DLL dependency {name}. Add an explicit packaging rule before distribution.')
    return names


def file_entry(root, path):
    return {'path': path.relative_to(root).as_posix(), 'size': path.stat().st_size, 'sha256': digest(path)}


def collect_licenses(radray, destination):
    names = ('fmt', 'magic_enum', 'eigen', 'sigslot', 'stdexec', 'spdlog', 'xxHash', 'yyjson',
             'pugixml', 'mimalloc', 'Vulkan-Headers', 'volk', 'VulkanMemoryAllocator',
             'DirectX-Headers', 'D3D12MemoryAllocator')
    roots = [('RadRay', radray), *[(name, radray / 'third_party' / name) for name in names]]
    files = []
    for name, source in roots:
        if not source.is_dir():
            raise RuntimeError(f'Missing license source {source}; explicitly restore RadRay third-party sources first.')
        candidates = [path for path in source.iterdir() if path.is_file() and
                      path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE'))]
        for directory_name in ('LICENSES', 'licenses'):
            directory = source / directory_name
            if directory.is_dir():
                candidates.extend(path for path in directory.rglob('*') if path.is_file())
        candidates = sorted(set(candidates))
        if not candidates:
            raise RuntimeError(f'No license files found for {name}; supply its upstream license before packaging.')
        for path in candidates:
            target = destination / 'licenses' / name / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            files.append(file_entry(destination, target))
    return files


def check_manifest(manifest):
    if (manifest.get('schemaVersion') != 1 or manifest.get('platform') != 'windows-x86_64'
            or manifest.get('abiVersion') != 6 or manifest.get('abiInfoSize') != 240
            or manifest.get('crt') != 'MT' or manifest.get('configuration') not in ('Debug', 'Release')
            or not isinstance(manifest.get('radrayDirty'), bool)):
        raise RuntimeError('Unsupported native manifest.')
    for key, length in (('buildId', 64), ('radrayCommit', 40)):
        if not re.fullmatch('[0-9a-f]{' + str(length) + '}', manifest.get(key, '')):
            raise RuntimeError(f'Invalid native manifest {key}.')
    if [entry['path'] for entry in manifest.get('files', [])] != ['rayniumruntime.dll']:
        raise RuntimeError('The current payload must contain exactly rayniumruntime.dll.')
    if not manifest.get('licenses'):
        raise RuntimeError('Native license files are missing.')
    seen = set()
    for entry in manifest['files'] + manifest['licenses']:
        path = entry['path']
        if (not isinstance(path, str) or '\\' in path or ':' in path or path.startswith('/')
                or any(part in ('', '.', '..') for part in path.split('/')) or path.casefold() in seen
                or not isinstance(entry['size'], int) or not 0 < entry['size'] <= 1024**3
                or not re.fullmatch('[0-9a-f]{64}', entry['sha256'])):
            raise RuntimeError('Invalid native payload file entry.')
        seen.add(path.casefold())
    if any(not entry['path'].startswith('licenses/') for entry in manifest['licenses']):
        raise RuntimeError('License entries must be inside licenses/.')


def verify_snapshot(snapshot):
    manifest = read_json(snapshot / 'native-manifest.json')
    check_manifest(manifest)
    for entry in manifest['files'] + manifest['licenses']:
        path = snapshot / entry['path']
        if not path.is_file() or path.stat().st_size != entry['size'] or digest(path) != entry['sha256']:
            raise RuntimeError(f'Native payload mismatch: {path}')
    return manifest


def current_snapshot(stage):
    pointer = read_json(stage / 'current.json')
    content = pointer.get('contentId', '')
    if pointer.get('schemaVersion') != 1 or not re.fullmatch('[0-9a-f]{64}', content):
        raise RuntimeError('Invalid native stage current.json.')
    snapshot = stage / content
    if digest(snapshot / 'native-manifest.json') != content:
        raise RuntimeError('Native stage content ID does not match its manifest.')
    return snapshot


def stage_native(root, binary, radray, config, environment):
    metadata = read_json(binary / 'bin/native-build.json')
    dll = binary / 'bin/rayniumruntime.dll'
    revision = run(['git', '-C', str(radray), 'rev-parse', 'HEAD'], root, environment, True).stdout.strip()
    dirty = bool(run(['git', '-C', str(radray), 'status', '--porcelain', '--untracked-files=normal'], root, environment, True).stdout.strip())
    if metadata.get('radrayCommit') != revision or metadata.get('radrayDirty') != dirty or metadata.get('configuration') != config:
        raise RuntimeError('Native source identity changed; configure and build before staging.')
    dependencies = checked_dependencies(dll, environment)
    stage = root / 'build/native/stage/windows-x86_64' / config
    stage.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.stage-', dir=stage) as directory:
        temporary = Path(directory)
        shutil.copyfile(dll, temporary / dll.name)
        metadata['files'] = [file_entry(temporary, temporary / dll.name)]
        metadata['licenses'] = collect_licenses(radray, temporary)
        metadata['systemDependencies'] = dependencies
        write_json(temporary / 'native-manifest.json', metadata)
        verify_snapshot(temporary)
        content = digest(temporary / 'native-manifest.json')
        destination = stage / content
        if destination.exists():
            if digest(destination / 'native-manifest.json') != content:
                raise RuntimeError(f'Existing native snapshot has an invalid identity: {destination}')
            verify_snapshot(destination)
        else:
            # Only complete snapshots are published; temporary files never become current.
            os.replace(temporary, destination)
            temporary.mkdir()
        pointer = stage / (temporary.name + '.json')
        write_json(pointer, {'schemaVersion': 1, 'contentId': content})
        os.replace(pointer, stage / 'current.json')
    print(f'Native snapshot: {destination}')


def package_mod(root, input_jar, output_jar):
    snapshot = current_snapshot(root / 'build/native/stage/windows-x86_64/Release')
    manifest = verify_snapshot(snapshot)
    if manifest['configuration'] != 'Release' or manifest['radrayDirty']:
        raise RuntimeError('Distribution requires a clean Release native build.')
    locked = read_json(root / 'native/radray.lock.json')['commit']
    if manifest['radrayCommit'] != locked:
        raise RuntimeError('Distribution requires the pinned RadRay commit.')
    if input_jar.resolve() == output_jar.resolve():
        raise RuntimeError('Native distribution must not overwrite the Java-only JAR.')
    output_jar.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.package-', dir=output_jar.parent) as directory:
        temporary = Path(directory) / output_jar.name
        with zipfile.ZipFile(input_jar) as source, zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as target:
            names = set()
            for item in source.infolist():
                if item.filename in names or item.filename.startswith(('natives/', 'META-INF/licenses/raynium-native/')):
                    raise RuntimeError(f'Duplicate/reserved JAR path: {item.filename}')
                if re.search(r'(?i)^META-INF/[^/]+\.(SF|RSA|DSA|EC)$', item.filename):
                    raise RuntimeError('Cannot merge native files into a signed JAR.')
                names.add(item.filename)
                target.writestr(item, source.read(item))
            if 'fabric.mod.json' not in names:
                raise RuntimeError('Input is not a Fabric mod JAR.')
            target.write(snapshot / 'native-manifest.json', 'natives/windows-x86_64/native-manifest.json')
            for entry in manifest['files']:
                target.write(snapshot / entry['path'], 'natives/windows-x86_64/' + entry['path'])
            for entry in manifest['licenses']:
                target.write(snapshot / entry['path'], 'META-INF/licenses/raynium-native/' + entry['path'][len('licenses/'):])
        verify_package(temporary)
        os.replace(temporary, output_jar)
    print(f'Native mod: {output_jar}')


def verify_package(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or 'fabric.mod.json' not in names:
            raise RuntimeError('Invalid or duplicate mod JAR entries.')
        if json.loads(archive.read('fabric.mod.json'))['id'] != 'raynium':
            raise RuntimeError('Input is not the Raynium mod.')
        manifest = json.loads(archive.read('natives/windows-x86_64/native-manifest.json'))
        check_manifest(manifest)
        if manifest['configuration'] != 'Release' or manifest['radrayDirty']:
            raise RuntimeError('Packaged native payload is not a clean Release build.')
        for entry in manifest['files'] + manifest['licenses']:
            name = ('META-INF/licenses/raynium-native/' + entry['path'][len('licenses/'):]
                    if entry['path'].startswith('licenses/') else 'natives/windows-x86_64/' + entry['path'])
            data = archive.read(name)
            if len(data) != entry['size'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise RuntimeError(f'Packaged payload mismatch: {name}')
    print(f'Package structure and hashes OK: {path.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('configure', 'build', 'stage', 'package', 'verify'))
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--parallel', type=int, default=8)
    parser.add_argument('--radray-source', type=Path)
    parser.add_argument('--input-jar', type=Path)
    parser.add_argument('--output-jar', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    radray = (args.radray_source or root / 'external/RadRay').resolve()
    binary = root / 'build/native/windows-x64-msvc-staticcrt' / args.config
    if args.parallel < 1:
        parser.error('--parallel must be positive')
    if args.command == 'package':
        if args.input_jar is None or args.output_jar is None:
            parser.error('package requires --input-jar and --output-jar')
        package_mod(root, args.input_jar, args.output_jar)
    elif args.command == 'verify':
        if args.input_jar is None:
            parser.error('verify requires --input-jar')
        verify_package(args.input_jar)
    else:
        environment = msvc_environment()
        if args.command == 'configure':
            run(['cmake', '--preset', 'win-x64-' + args.config.lower(), '-DRADRAY_SOURCE_DIR=' + str(radray)], root / 'native', environment)
        elif args.command == 'build':
            run(['cmake', '--build', str(binary), '--target', 'rayniumruntime', '--parallel', str(args.parallel)], root, environment)
        else:
            stage_native(root, binary, radray, args.config, environment)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile) as failure:
        print(f'error: {failure}', file=sys.stderr)
        sys.exit(1)
