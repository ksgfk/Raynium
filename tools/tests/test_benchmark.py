import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import benchmark
import fetch_radray
import native as native_tool


def sample_launch_spec():
    return {
        'schemaVersion': 1,
        'javaExecutable': 'C:/jdk25/bin/java.exe',
        'mainClass': 'net.fabricmc.loader.impl.launch.knot.KnotClient',
        'commonJvmArgs': ['--enable-native-access=ALL-UNNAMED'],
        'commonSystemProperties': {
            'fabric.development': 'true',
            'fabric.gameJarPath': 'C:/mc/common.jar',
        },
        'commonProgramArgs': ['--assetIndex', '26.3-34', '--assetsDir', 'C:/assets', '--version', '26.3'],
        'baseClasspath': ['C:/libs/mc.jar', 'C:/libs/loader.jar'],
        'mods': {
            'raynium': ['C:/repo/build/classes/java/client', 'C:/repo/build/resources/main'],
            'rayniumHarness': ['C:/repo/build/classes/java/harness', 'C:/repo/build/resources/harness'],
        },
        'nativeStageDirs': {
            'Debug': 'C:/repo/build/native/stage/windows-x86_64/Debug',
            'Release': 'C:/repo/build/native/stage/windows-x86_64/Release',
        },
    }


class BenchmarkCompletionTest(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt', 'benchmark world junctions require Windows')
    def test_run_opens_source_world_and_isolates_options_and_raynium_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            world = root / 'source map'
            world.mkdir()
            (world / 'level.dat').write_bytes(b'world')
            session = root / 'session'
            run = benchmark.prepare_run(
                {**benchmark.DEFAULTS, 'world': str(world), 'backend': 'vulkan', 'terrainSubmission': 'indirect'},
                session,
            )
            link = run / 'saves/benchmark'
            self.assertTrue(link.samefile(world))
            link.rmdir()
            self.assertEqual((world / 'level.dat').read_bytes(), b'world')
            self.assertEqual((session / 'options.txt').read_bytes(), (run / 'options.txt').read_bytes())
            raynium_cfg = json.loads((run / 'config/raynium.json').read_text(encoding='utf-8'))
            self.assertEqual({'backend': 'vulkan', 'rhiValidation': False, 'terrainSubmission': 'indirect'}, raynium_cfg)

    def test_completion_requires_frames_and_resource_shutdown_without_checkpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            logs = session / 'game/logs'
            logs.mkdir(parents=True)
            shutdown = 'Raynium runtime destroyed\nRaynium native environment destroyed\nRaynium native library closed\n'
            (logs / 'latest.log').write_text('Using graphics backend Raynium, using drivers: test\n' + shutdown)
            (session / 'game.log').write_text('DeviceContext: ready backend=D3D12 GPU=test Direct queues=1 RHI validation=false driver validation=false')
            result = {'status': 'completed', 'routeNativeFrames': 12, 'javaTracyLoaded': False,
                      'jfrRecording': False, 'lwjglChecks': False, 'javaAssertions': False}
            (session / 'client-result.json').write_text(json.dumps(result))
            (session / 'session.tracy').write_bytes(bytes(32))
            (session / 'capture.log').write_text('Saving trace... done!')
            self.assertEqual(result, benchmark.check_result(session, 0, 0, False))
            result['routeNativeFrames'] = 0
            (session / 'client-result.json').write_text(json.dumps(result))
            with self.assertRaisesRegex(RuntimeError, 'did not complete'):
                benchmark.check_result(session, 0, 0, False)
            result['routeNativeFrames'] = 12
            (session / 'client-result.json').write_text(json.dumps(result))
            (logs / 'latest.log').write_text('Raynium runtime destroyed\n')
            with self.assertRaisesRegex(RuntimeError, 'missing required evidence'):
                benchmark.check_result(session, 0, 0, False)

    def test_scene_decode_fallback_is_not_a_successful_benchmark(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            (session / 'game/logs').mkdir(parents=True)
            (session / 'game/logs/latest.log').write_text('Serialization errors: missing block id')
            (session / 'client-result.json').write_text(json.dumps({'status': 'completed', 'routeNativeFrames': 10}))
            with self.assertRaisesRegex(RuntimeError, 'invalid Minecraft input'):
                benchmark.check_result(session, 0, 0, False, False, 'vanilla')

    def test_build_gradle_args_and_direct_launch_isolate_vanilla_and_raynium(self):
        self.assertEqual(['harnessClasses', 'classes', 'clientClasses', 'nativeStageRelease', 'writeLaunchSpec'],
                         benchmark.build_gradle_args({'renderer': 'raynium', 'profilerEnabled': False}))
        self.assertEqual(['harnessClasses', 'classes', 'clientClasses', 'nativeStageRelease', 'writeLaunchSpec', '-Praynium.build.profiler=true'],
                         benchmark.build_gradle_args({'renderer': 'raynium', 'profilerEnabled': True}))
        self.assertEqual(['harnessClasses', 'writeLaunchSpec'],
                         benchmark.build_gradle_args({'renderer': 'vanilla', 'profilerEnabled': False}))

        spec = sample_launch_spec()
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            run = session / 'game'
            vanilla_cmd = benchmark.build_benchmark_command(spec, {**benchmark.DEFAULTS, 'renderer': 'vanilla', 'backend': 'vulkan'}, session, run)
            self.assertEqual(spec['javaExecutable'], vanilla_cmd[0])
            self.assertIn('--graphicsBackend', vanilla_cmd)
            self.assertIn('VULKAN', vanilla_cmd)
            self.assertIn('-da', vanilla_cmd)
            self.assertFalse(any(arg.startswith('-Draynium.native.dir=') for arg in vanilla_cmd))
            cp = vanilla_cmd[vanilla_cmd.index('-cp') + 1]
            self.assertNotIn('build/classes/java/client', cp)
            self.assertIn('build/classes/java/harness', cp)
            groups = next(arg for arg in vanilla_cmd if arg.startswith('-Dfabric.classPathGroups='))
            self.assertNotIn('build/classes/java/client', groups)
            self.assertNotIn(os.pathsep * 2, groups)

            raynium_cmd = benchmark.build_benchmark_command(spec, {**benchmark.DEFAULTS, 'renderer': 'raynium', 'jfrEnabled': True}, session, run)
            self.assertIn('-Draynium.native.dir=' + spec['nativeStageDirs']['Release'], raynium_cmd)
            self.assertTrue(any(arg.startswith('-XX:StartFlightRecording=') for arg in raynium_cmd))
            self.assertNotIn('--graphicsBackend', raynium_cmd)
            raynium_cp = raynium_cmd[raynium_cmd.index('-cp') + 1]
            self.assertIn('build/classes/java/client', raynium_cp)
            self.assertIn('build/classes/java/harness', raynium_cp)
            raynium_groups = next(arg for arg in raynium_cmd if arg.startswith('-Dfabric.classPathGroups='))
            self.assertIn(os.pathsep * 2, raynium_groups)

    def test_primary_reference_does_not_require_native_shutdown_or_tracy(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            (session / 'game/logs').mkdir(parents=True)
            (session / 'game/logs/latest.log').write_text('Using graphics backend Vulkan, using drivers: test\n')
            result = {'status': 'completed', 'routeNativeFrames': 10, 'javaTracyLoaded': False, 'jfrRecording': False,
                      'lwjglChecks': False, 'javaAssertions': False}
            (session / 'client-result.json').write_text(json.dumps(result))
            self.assertEqual(result, benchmark.check_result(session, 0, 0, False, False, 'vanilla'))
            (session / 'client-result.json').write_text(json.dumps({**result, 'javaTracyLoaded': True}))
            with self.assertRaisesRegex(RuntimeError, 'javaTracyLoaded'):
                benchmark.check_result(session, 0, 0, False, False, 'vanilla')
            (session / 'game/logs/latest.log').write_text('Using graphics backend OpenGL, using drivers: test\n')
            with self.assertRaisesRegex(RuntimeError, 'did not select'):
                benchmark.check_result(session, 0, 0, False, False, 'vanilla')

    def test_fetch_radray_proxy_precedence_and_direct_default(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            self.assertIsNone(fetch_radray.resolve_proxy(None, workspace, {}))
            self.assertEqual('http://env:8080', fetch_radray.resolve_proxy(None, workspace, {'HTTPS_PROXY': 'http://env:8080'}))
            (workspace / 'local.properties').write_text('# comment\nproxy=http://127.0.0.1:10808\n', encoding='utf-8')
            self.assertEqual('http://127.0.0.1:10808', fetch_radray.resolve_proxy(None, workspace, {'HTTPS_PROXY': 'http://env:8080'}))
            self.assertEqual('socks5://cli:1080', fetch_radray.resolve_proxy('socks5://cli:1080', workspace, {'HTTPS_PROXY': 'http://env:8080'}))

    def test_verify_package_rejects_profiler_enabled_release_manifest(self):
        import hashlib
        import zipfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jar_path = root / 'bad-profiler.jar'
            shader = (Path(__file__).resolve().parents[2] / 'src/client/resources/assets/raynium/shaderpacks/default.rnsp').read_bytes()
            dll_bytes = b'fake-dll'
            lic_bytes = b'MIT'
            manifest = {
                'schemaVersion': 1,
                'platform': 'windows-x86_64',
                'abiVersion': native_tool.ABI_VERSION,
                'abiInfoSize': 240,
                'configuration': 'Release',
                'crt': 'MT',
                'radrayDirty': False,
                'profilerEnabled': True,
                'buildId': '0' * 64,
                'radrayCommit': 'a' * 40,
                'files': [{'path': 'rayniumruntime.dll', 'size': len(dll_bytes), 'sha256': hashlib.sha256(dll_bytes).hexdigest()}],
                'licenses': [{'path': 'licenses/RadRay/LICENSE', 'size': len(lic_bytes), 'sha256': hashlib.sha256(lic_bytes).hexdigest()}],
            }
            with zipfile.ZipFile(jar_path, 'w') as zf:
                zf.writestr('fabric.mod.json', json.dumps({'id': 'raynium'}))
                zf.writestr(native_tool.RESOURCE_PATH, shader)
                zf.writestr('natives/windows-x86_64/native-manifest.json', json.dumps(manifest))
                zf.writestr('natives/windows-x86_64/rayniumruntime.dll', dll_bytes)
                zf.writestr('META-INF/licenses/raynium-native/RadRay/LICENSE', lic_bytes)
            with self.assertRaisesRegex(RuntimeError, 'profiler'):
                native_tool.verify_package(jar_path)
