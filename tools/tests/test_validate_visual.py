from contextlib import ExitStack
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_visual import (
    build_gradle_args,
    build_validation_command,
    check_capture_log,
    compare,
    link_region_storage,
)


class VisualInputTest(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'win32', 'Windows save junctions')
    def test_original_regions_are_linked_and_metadata_is_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            world, save = root / 'world', root / 'session/save'
            region = Path('dimensions/minecraft/overworld/region/r.0.0.mca')
            (world / region).parent.mkdir(parents=True)
            (world / region).write_bytes(b'region fixture')
            (world / 'level.dat').write_bytes(b'original')
            with ExitStack() as cleanup:
                link_region_storage(world, save, cleanup)
                self.assertTrue((world / region).samefile(save / region))
                (save / 'level.dat').write_bytes(b'session')
                self.assertEqual(b'original', (world / 'level.dat').read_bytes())
                self.assertFalse((save / 'session.lock').exists())
            self.assertFalse((save / region).exists())
            self.assertEqual(b'region fixture', (world / region).read_bytes())

    def test_decoding_failures_and_gpu_validation_cannot_pass_as_visual_evidence(self):
        for text in ['Serialization errors:\nInvalid block state', 'Failed to decode value',
                     'Validation Error: VUID-command-buffer', '[error] d3d12 validation message', '[critical] Device lost',
                     '[error] native/source.cpp:10\nd3d12 validation message: bad binding',
                     '[error] native/source.cpp:10\nRHI validation: missing parameter']:
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                check_capture_log(text)
        check_capture_log('WARNING-Shader-OutputNotConsumed\nMinecraft started')

    def test_build_gradle_args_and_direct_validation_commands(self):
        self.assertEqual(['harnessClasses', 'writeLaunchSpec'], build_gradle_args(['reference'], 'Debug'))
        self.assertEqual(['harnessClasses', 'classes', 'clientClasses', 'nativeStageDebug', 'writeLaunchSpec'],
                         build_gradle_args(['reference', 'd3d12', 'vulkan'], 'Debug'))
        self.assertEqual(['harnessClasses', 'classes', 'clientClasses', 'nativeStageRelease', 'writeLaunchSpec'],
                         build_gradle_args(['vulkan'], 'Release'))

        spec = {
            'schemaVersion': 1,
            'javaExecutable': 'C:/jdk25/bin/java.exe',
            'mainClass': 'net.fabricmc.loader.impl.launch.knot.KnotClient',
            'commonJvmArgs': ['--enable-native-access=ALL-UNNAMED'],
            'commonSystemProperties': {'fabric.development': 'true'},
            'commonProgramArgs': ['--version', '26.3'],
            'baseClasspath': ['C:/libs/mc.jar'],
            'mods': {
                'raynium': ['C:/repo/build/classes/java/client'],
                'rayniumHarness': ['C:/repo/build/classes/java/harness'],
            },
            'nativeStageDirs': {'Debug': 'C:/stage/Debug', 'Release': 'C:/stage/Release'},
        }
        ref_cmd = build_validation_command(spec, Path('C:/out/ref/game'), Path('C:/out/ref/scenes.json'), 'reference', 'Debug',
                                           tracy_images=True, in_place_world=True)
        self.assertIn('--graphicsBackend', ref_cmd)
        self.assertIn('VULKAN', ref_cmd)
        self.assertIn('--tracy', ref_cmd)
        self.assertIn('-DMC_DEBUG_ENABLED=true', ref_cmd)
        self.assertIn('-DMC_DEBUG_DONT_SAVE_WORLD=true', ref_cmd)
        self.assertFalse(any(a.startswith('-Draynium.native.dir=') for a in ref_cmd))
        self.assertNotIn('build/classes/java/client', ref_cmd[ref_cmd.index('-cp') + 1])

        native_cmd = build_validation_command(spec, Path('C:/out/d3d12/game'), Path('C:/out/d3d12/scenes.json'), 'd3d12', 'Debug')
        self.assertIn('-Draynium.native.dir=C:/stage/Debug', native_cmd)
        self.assertNotIn('--graphicsBackend', native_cmd)
        self.assertIn('build/classes/java/client', native_cmd[native_cmd.index('-cp') + 1])


@unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('PIL'), 'Image comparison requires NumPy and Pillow')
class VisualComparisonTest(unittest.TestCase):
    def test_alpha_only_difference_is_measured_and_visible(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for mode in ('reference', 'vulkan'):
                (root / mode).mkdir()
            (root / 'reference/visual-result.json').write_text('{"scenes": []}', encoding='utf-8')
            Image.new('RGBA', (2, 1), (40, 50, 60, 0)).save(root / 'reference/export.png')
            Image.new('RGBA', (2, 1), (40, 50, 60, 255)).save(root / 'vulkan/export.png')
            compare(root, ['vulkan'])
            report = json.loads((root / 'vulkan/differences/report.json').read_text(encoding='utf-8'))['images'][0]
            self.assertEqual([0, 0, 0, 255], report['maximumRgbaErrors'])
            self.assertEqual(2, report['pixelsAbove2'])
            with Image.open(root / 'vulkan/differences/export.png') as image:
                self.assertEqual((255, 255, 255), image.getpixel((0, 0)))
