import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import benchmark


class BenchmarkCompletionTest(unittest.TestCase):
    def test_completion_requires_frames_and_resource_shutdown_without_checkpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            logs = session / 'game/logs'
            logs.mkdir(parents=True)
            shutdown = 'Raynium runtime destroyed\nRaynium native environment destroyed\nRaynium native library closed\n'
            (logs / 'latest.log').write_text(shutdown)
            result = {'status': 'completed', 'routeNativeFrames': 12}
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

    def test_gradle_properties_keep_sampling_implicit(self):
        properties = benchmark.gradle_properties({'backend': 'vulkan', 'terrainSubmission': 'indirect'})
        self.assertIn('-PnativeProfiler=true', properties)
        self.assertIn('-PrhiBackend=vulkan', properties)
        self.assertFalse(any('terrainProfile' in value or 'terrainDisableFaceCull' in value for value in properties))
