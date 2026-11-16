import struct
import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from shader_pack import verify_shader_pack, PROGRAMS


class ShaderPackTest(unittest.TestCase):
    def setUp(self):
        self.data = bytearray((Path(__file__).resolve().parents[2] /
            'src/client/resources/assets/raynium/shaderpacks/default.rnsp').read_bytes())

    def test_dual_backend_pack(self):
        interfaces = json.loads((Path(__file__).resolve().parents[2] /
            'src/client/resources/assets/raynium/scene-interfaces.json').read_text(encoding='utf-8'))
        programs = set(PROGRAMS) | {entry['program'] for entry in interfaces}
        post_catalog=(Path(__file__).resolve().parents[2]/'native/src/post_catalog.inc').read_text(encoding='utf-8')
        programs.update(re.findall(r'result.program="([a-z0-9_]+)"',post_catalog))
        self.assertEqual({(name.encode(), target) for name in programs for target in (0, 1)}, verify_shader_pack(self.data))

    def test_bad_containers(self):
        for mutation in ('version', 'truncated', 'range', 'duplicate', 'missing', 'checksum', 'identity'):
            with self.subTest(mutation=mutation):
                data = self.data.copy()
                if mutation == 'version': struct.pack_into('<I', data, 4, 2)
                if mutation == 'truncated': data.pop()
                if mutation == 'range': struct.pack_into('<I', data, 24 + 68, 0xfffffff8)
                if mutation == 'duplicate': struct.pack_into('<I', data, 24 + 96 + 64, 0)
                if mutation == 'missing': data[24:31] = b'absent\0'
                if mutation == 'checksum': data[-1] ^= 1
                if mutation == 'identity': data[24 + 80] ^= 1
                with self.assertRaises(ValueError): verify_shader_pack(data)


if __name__ == '__main__': unittest.main()
