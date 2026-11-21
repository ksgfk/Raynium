import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import benchmark


class BenchmarkCompletionTest(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt', 'benchmark world junctions require Windows')
    def test_run_opens_source_world_and_isolates_options(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            world = root / 'source map'
            world.mkdir()
            (world / 'level.dat').write_bytes(b'world')
            session = root / 'session'
            run = benchmark.prepare_run({**benchmark.DEFAULTS, 'world': str(world)}, session)
            link = run / 'saves/benchmark'
            self.assertTrue(link.samefile(world))
            link.rmdir()
            self.assertEqual((world / 'level.dat').read_bytes(), b'world')
            self.assertEqual((session / 'options.txt').read_bytes(), (run / 'options.txt').read_bytes())

    def test_completion_requires_frames_and_resource_shutdown_without_checkpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            logs = session / 'game/logs'
            logs.mkdir(parents=True)
            shutdown = 'Raynium runtime destroyed\nRaynium native environment destroyed\nRaynium native library closed\n'
            (logs / 'latest.log').write_text('Using graphics backend Raynium, using drivers: test\n'+shutdown)
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
            session=Path(directory); (session/'game/logs').mkdir(parents=True)
            (session/'game/logs/latest.log').write_text('Serialization errors: missing block id')
            (session/'client-result.json').write_text(json.dumps({'status':'completed','routeNativeFrames':10}))
            with self.assertRaisesRegex(RuntimeError,'invalid Minecraft input'):
                benchmark.check_result(session,0,0,False,False,'vanilla')

    def test_primary_build_has_no_profiler_and_diagnostics_are_explicit(self):
        properties = benchmark.gradle_properties({'backend': 'vulkan', 'terrainSubmission': 'indirect'})
        self.assertIn('-PnativeProfiler=false', properties)
        self.assertIn('-PrhiBackend=vulkan', properties)
        self.assertFalse(any('terrainProfile' in value or 'terrainDisableFaceCull' in value for value in properties))
        self.assertIn('-PnativeProfiler=true',benchmark.gradle_properties({'backend':'d3d12','terrainSubmission':'auto','profilerEnabled':True}))

    def test_primary_reference_does_not_require_native_shutdown_or_tracy(self):
        with tempfile.TemporaryDirectory() as directory:
            session=Path(directory); (session/'game/logs').mkdir(parents=True)
            (session/'game/logs/latest.log').write_text('Using graphics backend Vulkan, using drivers: test\n')
            result={'status':'completed','routeNativeFrames':10,'javaTracyLoaded':False,'jfrRecording':False,
                    'lwjglChecks':False,'javaAssertions':False}
            (session/'client-result.json').write_text(json.dumps(result))
            self.assertEqual(result,benchmark.check_result(session,0,0,False,False,'vanilla'))
            (session/'client-result.json').write_text(json.dumps({**result, 'javaTracyLoaded': True}))
            with self.assertRaisesRegex(RuntimeError, 'javaTracyLoaded'):
                benchmark.check_result(session,0,0,False,False,'vanilla')
            (session/'game/logs/latest.log').write_text('Using graphics backend OpenGL, using drivers: test\n')
            with self.assertRaisesRegex(RuntimeError, 'did not select'):
                benchmark.check_result(session,0,0,False,False,'vanilla')
