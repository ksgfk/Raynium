"""Compile one already-expanded RenderPearl program using the explicitly installed companion tools."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile

from convert_vanilla_shaders import convert, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('request', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    tools = Path(__file__).resolve().parent
    request = json.loads(args.request.read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory(dir=args.output.resolve().parent) as directory:
        root = Path(directory)
        interface = convert(request, None, root, tools/'glslangValidator.exe', tools/'spirv-cross.exe',
                            request['expanded'])
        name = interface['program']
        if interface['key'] != request['key']:
            raise ValueError('Source and define hash disagrees with the request')
        pack = root/'program.rnsp'
        run([tools/'raynium_shader_pack.exe', '--root', root, '--program', f'{name}={name}.hlsl',
             '--standalone', name, '--output', pack])
        data = pack.read_bytes()
        interface['packHash'] = hashlib.sha256(data).hexdigest()
        with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_STORED) as result:
            result.writestr('program.json', json.dumps(interface))
            result.writestr('program.rnsp', data)


if __name__ == '__main__':
    main()
