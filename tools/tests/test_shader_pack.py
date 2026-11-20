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
        programs = set(PROGRAMS)
        source_index = json.loads((Path(__file__).resolve().parents[2] /
            'src/client/resources/assets/raynium/shaderpacks/default.json').read_text(encoding='utf-8'))
        for key, entry in source_index.items():
            self.assertRegex(key, r'^p_[0-9a-f]{64}$')
            self.assertEqual(key[:58], entry['program'])
            programs.add(entry['program'])
        self.assertEqual({(name.encode(), target) for name in programs for target in (0, 1)}, verify_shader_pack(self.data))

    def test_packed_bindings_are_covered_by_each_program_interface(self):
        resource_root = Path(__file__).resolve().parents[2] / 'src/client/resources/assets/raynium/shaderpacks'
        catalog = {entry['program']: entry for entry in json.loads((resource_root/'default.json').read_text()).values()}
        count = struct.unpack_from('<I', self.data, 12)[0]
        for i in range(count):
            name, target, offset, length = struct.unpack_from('<64s3I', self.data, 24+96*i)
            name = name.rstrip(bytes(1)).decode('ascii')
            if name not in catalog:
                continue
            artifact = memoryview(self.data)[offset:offset+length]
            start, size = struct.unpack_from('<2I', artifact, 24)
            resources = catalog[name]['resources'].values()
            expected = {(r['group'], r['binding']) for r in resources}
            expected.update((1,r['sampler']) for r in resources if r.get('sampler',-1)>=0)
            self.assertEqual(0, size%36, name)
            for pos in range(start,start+size,36):
                group, binding, flags = struct.unpack_from('<3I',artifact,pos+8)
                if target==0 and group in (2,3) and binding==0:
                    continue  # Private D3D12 push/draw-base bindings.
                if target==0 and group==1:
                    binding = 2*binding + ((flags & 15)==10)  # Schema 10 sampler code.
                self.assertIn((group,binding), expected, (name,target,group,binding))

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
