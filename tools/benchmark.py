"""Build Release, fly a copied world, and save native Tracy and optional JVM JFR captures."""

import argparse
import ctypes
from contextlib import contextmanager
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {
    'outputDir': str(ROOT / 'build/benchmarks'), 'backend': 'd3d12', 'terrainProfile': True,
    'warmupSeconds': 20, 'startupTimeoutSeconds': 180, 'shutdownTimeoutSeconds': 60,
    'captureTimeoutSeconds': 120, 'width': 1280, 'height': 720,
    'renderDistance': 32, 'simulationDistance': 8, 'jfrEnabled': False,
}
PATH_KEYS = ('tracyCapture', 'world', 'optionsFile', 'outputDir')


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def number(value, name, minimum, maximum):
    if type(value) not in (int, float) or not math.isfinite(value) or not minimum <= value <= maximum:
        raise ValueError(f'{name} must be a finite number in [{minimum}, {maximum}]')
    return value


def load_config(path, capture_override=None):
    raw = read_json(path)
    if not isinstance(raw, dict):
        raise ValueError('Configuration must be a JSON object')
    unknown = raw.keys() - (DEFAULTS.keys() | set(PATH_KEYS) | {'route'})
    if unknown:
        raise ValueError(f'Unknown settings: {sorted(unknown)}')
    config = {**DEFAULTS, **raw}
    if capture_override:
        config['tracyCapture'] = str(Path(capture_override).resolve())
    for key in ('world', 'tracyCapture', 'route'):
        if key not in config:
            raise ValueError(f'Missing setting: {key}')
    for key in PATH_KEYS:
        if key in config:
            if not isinstance(config[key], str) or not config[key].strip():
                raise ValueError(f'{key} must be a nonempty path')
            config[key] = str((path.parent / config[key]).resolve())
    if not (Path(config['world']) / 'level.dat').is_file():
        raise ValueError('world must name an existing singleplayer save directory containing level.dat')
    if Path(config['outputDir']).is_relative_to(Path(config['world'])):
        raise ValueError('outputDir cannot be inside the source world')
    for key in ('tracyCapture', 'optionsFile'):
        if key in config and not Path(config[key]).is_file():
            raise ValueError(f'{key} does not exist: {config[key]}')
    if config['backend'] not in ('d3d12', 'vulkan'):
        raise ValueError('backend must be d3d12 or vulkan')
    for key in ('terrainProfile', 'jfrEnabled'):
        if type(config[key]) is not bool:
            raise ValueError(f'{key} must be a boolean')
    for key in ('warmupSeconds', 'startupTimeoutSeconds', 'shutdownTimeoutSeconds', 'captureTimeoutSeconds'):
        number(config[key], key, 0 if key == 'warmupSeconds' else 1, 86400)
    for key, minimum, maximum in (('width', 320, 7680), ('height', 240, 4320),
                                  ('renderDistance', 2, 32), ('simulationDistance', 5, 32)):
        number(config[key], key, minimum, maximum)
        if type(config[key]) is not int:
            raise ValueError(f'{key} must be an integer')
    route = config['route']
    if not isinstance(route, list) or len(route) < 2:
        raise ValueError('route needs at least two points')
    previous = -1
    for index, point in enumerate(route):
        if not isinstance(point, dict) or point.keys() != {'seconds', 'x', 'y', 'z', 'yaw', 'pitch'}:
            raise ValueError(f'route[{index}] must contain seconds, x, y, z, yaw, pitch')
        seconds = number(point['seconds'], f'route[{index}].seconds', 0, 86400)
        if (index == 0 and seconds != 0) or seconds <= previous:
            raise ValueError('Route must start at 0 with strictly increasing seconds')
        previous = seconds
        for axis in ('x', 'z'):
            number(point[axis], f'route[{index}].{axis}', -29_999_984, 29_999_984)
        number(point['y'], f'route[{index}].y', -20_000_000, 20_000_000)
        number(point['yaw'], f'route[{index}].yaw', -360, 360)
        number(point['pitch'], f'route[{index}].pitch', -90, 90)
    return config


def process_options():
    if os.name != 'nt':
        return {}
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    return {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW,
            'startupinfo': startup}


def gradle_command(*arguments):
    java_home = os.environ.get('JAVA_HOME')
    java = str(Path(java_home) / 'bin/java.exe') if java_home else shutil.which('java')
    if not java:
        raise RuntimeError('Java is missing; configure JAVA_HOME or PATH')
    # Invoke the wrapper main directly so paths never pass through cmd.exe expansion.
    return [java, '-Xmx64m', '-classpath', str(ROOT / 'gradle/wrapper/gradle-wrapper.jar'),
            'org.gradle.wrapper.GradleWrapperMain', '--console=plain', '--no-parallel',
            '--no-configuration-cache', '--no-daemon', *arguments]


def gradle_properties(config):
    return ['-PnativeConfig=Release', '-PnativeProfiler=true', '-PrhiValidation=false',
            '-PvulkanValidation=false', '-PpreviewEnabled=true', '-PterrainDisableFaceCull=false',
            '-PterrainTextureTransfer=auto', '-PrhiBackend=' + config['backend'],
            '-PterrainProfile=' + str(config['terrainProfile']).lower(), '-PnativePython=' + sys.executable]


def terminate_tree(process):
    if process is None or process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False, **process_options())
    else:
        process.kill()
    process.wait(timeout=30)


def capture_connected(pid, port):
    # A read-only TCP table query avoids depending on Tracy's buffered console output.
    # MIB_TCPTABLE_OWNER_PID: count, then six DWORDs per IPv4 row.
    size = ctypes.c_ulong(0)
    query = ctypes.WinDLL('iphlpapi').GetExtendedTcpTable
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.c_int,
                      ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    query.restype = ctypes.c_ulong
    for _ in range(4):
        table = ctypes.create_string_buffer(size.value) if size.value else None
        status = query(table, ctypes.byref(size), False, socket.AF_INET, 5, 0)
        if status == 122:  # ERROR_INSUFFICIENT_BUFFER; table may grow between calls.
            continue
        if status:
            raise OSError(status, 'Cannot inspect Tracy TCP connection')
        rows = memoryview(table).cast('B')
        count = int.from_bytes(rows[:4], 'little')
        for index in range(count):
            offset = 4 + index * 24
            state, local_address, local_port, remote_address, remote_port, owner = [
                int.from_bytes(rows[offset + i:offset + i + 4], 'little') for i in range(0, 24, 4)]
            if owner == pid and state == 5 and socket.ntohs(remote_port & 0xffff) == port:
                return True
        return False
    return False


@contextmanager
def locked_world(world):
    # MC locks the whole session.lock file. Hold one byte while copying so a live save is never copied.
    import msvcrt
    with (world / 'session.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError('Source world is in use; close it before benchmarking') from error
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def prepare_run(config, session):
    world = Path(config['world'])
    run = session / 'game'
    with locked_world(world):
        # Reject directory links rather than following data outside the selected world.
        for base, directories, files in os.walk(world, followlinks=False):
            for name in directories + files:
                path = Path(base) / name
                if path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                    raise RuntimeError(f'World contains a link; use a regular save copy: {path}')
        shutil.copytree(world, run / 'saves/benchmark', ignore=shutil.ignore_patterns('session.lock'))
    options = {}
    if 'optionsFile' in config:
        for line in Path(config['optionsFile']).read_text(encoding='utf-8').splitlines():
            key, separator, value = line.partition(':')
            if separator:
                options[key] = value
    options.update({
        'preferredGraphicsBackend': '"vulkan"', 'enableVsync': 'false', 'maxFps': '260',
        'pauseOnLostFocus': 'false', 'inactivityFpsLimit': '"minimized"',
        'fullscreen': 'false', 'exclusiveFullscreen': 'false', 'onboardAccessibility': 'false',
        'tutorialStep': 'none', 'startedCleanly': 'true', 'glDebugVerbosity': '0',
        'resourcePacks': '["vanilla"]', 'incompatibleResourcePacks': '[]',
        'overrideWidth': str(config['width']), 'overrideHeight': str(config['height']),
        'renderDistance': str(config['renderDistance']), 'simulationDistance': str(config['simulationDistance']),
    })
    (run / 'options.txt').write_text(''.join(f'{key}:{value}\n' for key, value in options.items()), encoding='utf-8')
    shutil.copyfile(run / 'options.txt', session / 'options.txt')
    write_json(session / 'benchmark.json', config)
    return run


def check_capture_tool(path):
    probe = subprocess.run([str(path)], capture_output=True, timeout=15, **process_options())
    version = re.search(rb'tracy-capture (\d+\.\d+\.\d+)', probe.stdout + probe.stderr)
    header = (ROOT / 'external/RadRay/third_party/tracy/public/common/TracyVersion.hpp').read_text(encoding='utf-8')
    expected = '.'.join(re.search(rf'{part}\s*=\s*(\d+)', header).group(1) for part in ('Major', 'Minor', 'Patch'))
    if not version or version.group(1).decode() != expected:
        raise RuntimeError(f'tracyCapture must be the Tracy {expected} command-line capture executable')
    return expected


def check_payload(session):
    stage = ROOT / 'build/native/stage/windows-x86_64/Release'
    current = read_json(stage / 'current.json')
    payload = stage / current['contentId']
    manifest = read_json(payload / 'native-manifest.json')
    if manifest.get('configuration') != 'Release' or manifest.get('crt') != 'MT' or manifest.get('profilerEnabled') is not True:
        raise RuntimeError('Staged payload is not Release/MT with Tracy enabled')
    shutil.copyfile(payload / 'native-manifest.json', session / 'native-manifest.json')


def check_result(session, game_code, capture_code, jfr_enabled):
    if game_code != 0 or capture_code != 0:
        raise RuntimeError(f'Process failed: game/Gradle={game_code}, Tracy={capture_code}')
    client = read_json(session / 'client-result.json')
    if client.get('status') != 'completed' or client.get('routeNativeFrames', 0) <= 0:
        raise RuntimeError('Client did not complete native benchmark: ' + client.get('message', 'missing completion'))
    game_log = (session / 'game/logs/latest.log').read_text(encoding='utf-8', errors='replace')
    for marker in ('Raynium shared preview ready:', 'Raynium SOLID terrain inputs ready',
                   'Raynium runtime destroyed', 'Raynium native environment destroyed', 'Raynium native library closed'):
        if marker not in game_log:
            raise RuntimeError(f'Game log is missing required evidence: {marker}')
    if 'preview disabled' in game_log or 'Shutdown failure!' in game_log:
        raise RuntimeError('Native preview was disabled or shutdown failed; inspect game log')
    trace = session / 'session.tracy'
    if not trace.is_file() or trace.stat().st_size < 16:
        raise RuntimeError('Tracy did not save a nonempty trace')
    capture_log = (session / 'capture.log').read_text(encoding='utf-8', errors='replace')
    if 'Instrumentation failure:' in capture_log:
        raise RuntimeError('Tracy reported an instrumentation failure; inspect capture.log')
    if 'Saving trace...' not in capture_log or 'done!' not in capture_log:
        raise RuntimeError('Tracy did not report successful trace serialization')
    if jfr_enabled:
        recording = session / 'session.jfr'
        if not recording.is_file() or recording.stat().st_size < 68:
            raise RuntimeError('JFR did not save a nonempty recording; inspect game.log')
        with recording.open('rb') as stream:
            if stream.read(4) != b'FLR\x00':
                raise RuntimeError('JFR recording has an invalid header; inspect game.log')
    return client


def run_benchmark(config):
    if os.name != 'nt':
        raise RuntimeError('This benchmark currently requires Windows x64')
    version = check_capture_tool(Path(config['tracyCapture']))
    session = Path(config['outputDir']) / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    session.mkdir(parents=True, exist_ok=False)
    print(f'Session: {session}', flush=True)
    summary = {'status': 'running', 'tracyVersion': version, 'configuration': 'Release',
               'nativeProfiler': True, 'rhiValidation': False, 'vulkanValidation': False,
               'jfrEnabled': config['jfrEnabled']}
    game = capture = build = None
    try:
        print('Copying world and preparing isolated options...', flush=True)
        run = prepare_run(config, session)
        properties = gradle_properties(config)
        print('Building Release native payload and Java client (build.log)...', flush=True)
        with (session / 'build.log').open('wb') as log:
            build = subprocess.Popen(gradle_command('nativeStage', 'classes', 'clientClasses', *properties),
                                     cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, **process_options())
            if build.wait(timeout=1800) != 0:
                raise RuntimeError('Build failed; see build.log (dependencies must be prepared explicitly)')
        check_payload(session)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        environment = os.environ.copy()
        environment['TRACY_PORT'] = str(port)
        summary['port'] = port
        command = gradle_command('runClientNative', '-x', 'nativeStage', *properties,
                                 '-PbenchmarkRunDir=' + str(run), '-PbenchmarkConfig=' + str(session / 'benchmark.json'))
        if config['jfrEnabled']:
            command.append('-PjfrFile=' + str(session / 'session.jfr'))
        write_json(session / 'commands.json', {'game': command, 'capture': [config['tracyCapture'], '-a', '127.0.0.1',
                                                                        '-p', str(port), '-o', 'session.tracy']})
        print(f'Starting Tracy on localhost:{port}, then Minecraft...', flush=True)
        with (session / 'capture.log').open('wb') as capture_log, (session / 'game.log').open('wb') as game_log:
            # A relative ASCII trace name also works with Tracy's Windows narrow-character fopen.
            capture = subprocess.Popen([config['tracyCapture'], '-a', '127.0.0.1', '-p', str(port), '-o', 'session.tracy'],
                                       cwd=session, stdout=capture_log, stderr=subprocess.STDOUT, **process_options())
            game = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=game_log,
                                    stderr=subprocess.STDOUT, **process_options())
            deadline = time.monotonic() + config['startupTimeoutSeconds'] + config['warmupSeconds'] + config['route'][-1]['seconds'] + config['shutdownTimeoutSeconds']
            connected = False
            stopping = False
            while game.poll() is None:
                if not connected and capture_connected(capture.pid, port):
                    connected = True
                    (session / 'capture.ready').touch()
                    print('Tracy connected; waiting for warmup and route completion...', flush=True)
                if capture.poll() is not None and not (session / 'client-result.json').exists():
                    raise RuntimeError('Tracy exited before the route completed; see capture.log')
                if not stopping and (session / 'client-result.json').exists():
                    stopping = True
                    deadline = min(deadline, time.monotonic() + config['shutdownTimeoutSeconds'])
                if time.monotonic() >= deadline:
                    raise TimeoutError('Game startup, route or shutdown timed out')
                time.sleep(0.25)
            if not connected:
                raise RuntimeError('Game exited before Tracy connected; see game.log')
            print('Game exited; waiting for Tracy to finish saving...', flush=True)
            capture.wait(timeout=config['captureTimeoutSeconds'])
        summary['client'] = check_result(session, game.returncode, capture.returncode, config['jfrEnabled'])
        summary['traceBytes'] = (session / 'session.tracy').stat().st_size
        if config['jfrEnabled']:
            summary['jfrBytes'] = (session / 'session.jfr').stat().st_size
        summary['status'] = 'completed'
        print(f"Saved: {session / 'session.tracy'}", flush=True)
        if config['jfrEnabled']:
            print(f"Saved: {session / 'session.jfr'}", flush=True)
    except (Exception, KeyboardInterrupt) as error:
        summary['status'] = 'failed'
        summary['error'] = str(error) or 'Interrupted'
        print(f"Benchmark failed: {summary['error']}", file=sys.stderr, flush=True)
        if game is not None and game.poll() is None:
            (session / 'stop.request').touch()
            print('Requesting normal game shutdown...', flush=True)
            try:
                game.wait(timeout=config['shutdownTimeoutSeconds'])
            except subprocess.TimeoutExpired:
                summary['forcedGameTermination'] = True
                terminate_tree(game)
        if capture is not None and capture.poll() is None:
            try:
                capture.wait(timeout=config['captureTimeoutSeconds'] if (session / 'capture.ready').exists() else 2)
            except subprocess.TimeoutExpired:
                summary['forcedCaptureTermination'] = True
                terminate_tree(capture)
    finally:
        terminate_tree(build)
        terminate_tree(game)
        terminate_tree(capture)
        summary['finishedAt'] = datetime.now().astimezone().isoformat()
        write_json(session / 'summary.json', summary)
    return 0 if summary['status'] == 'completed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True, help='JSON config; relative paths resolve against its directory')
    parser.add_argument('--tracy-capture', help='Override capture executable path (relative to current directory)')
    parser.add_argument('--jfr', action='store_true', help='Record JVM profile JFR alongside Tracy in session.jfr')
    parser.add_argument('--check', action='store_true', help='Validate configuration and print resolved settings without building or launching')
    args = parser.parse_args()
    try:
        config = load_config(args.config.resolve(), args.tracy_capture)
        if args.jfr:
            config['jfrEnabled'] = True
        if args.check:
            print(json.dumps(config, ensure_ascii=False, indent=2))
            return 0
        return run_benchmark(config)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Benchmark error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
