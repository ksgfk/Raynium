import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_visual import compare, check_capture_log


class VisualInputTest(unittest.TestCase):
    def test_decoding_failures_and_gpu_validation_cannot_pass_as_visual_evidence(self):
        for text in ['Serialization errors:\nInvalid block state', 'Failed to decode value',
                     'Validation Error: VUID-command-buffer', '[error] d3d12 validation message', '[critical] Device lost',
                     '[error] native/source.cpp:10\nd3d12 validation message: bad binding',
                     '[error] native/source.cpp:10\nRHI validation: missing parameter']:
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                check_capture_log(text)
        check_capture_log('WARNING-Shader-OutputNotConsumed\nMinecraft started')


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
