"""Container checks for committed shader resources; full artifact decoding lives in RadRay."""
import re
import struct
import zlib

RESOURCE_PATH = 'assets/raynium/shaderpacks/default.rnsp'
HEADER = struct.Struct('<6I')
RECORD = struct.Struct('<64s4I16s')


def verify_shader_pack(data):
    def require(condition, message):
        if not condition:
            raise ValueError('Invalid shader pack: ' + message)

    require(HEADER.size <= len(data) <= 64 << 20, 'size')
    magic, version, size, count, directory, reserved = HEADER.unpack_from(data)
    require(magic == 0x50534e52 and version == 1 and size == len(data)
            and directory == HEADER.size and reserved == 0 and 0 < count <= 4096, 'header/version')
    end = directory + count * RECORD.size
    require(end <= len(data), 'truncated directory')
    keys, ranges = set(), []
    for index in range(count):
        raw_name, target, offset, length, checksum, identity = RECORD.unpack_from(data, directory + index * RECORD.size)
        name, separator, padding = raw_name.partition(b'\0')
        require(separator and not any(padding) and re.fullmatch(rb'[a-z0-9_./-]+', name), 'program name')
        require(target in (0, 1) and (name, target) not in keys, 'target/duplicate entry')
        keys.add((name, target))
        require(offset >= end and offset % 8 == 0 and length >= 152 and offset + length <= len(data), 'artifact range')
        ranges.append((offset, offset + length))
        artifact = data[offset:offset + length]
        require(zlib.crc32(artifact) == checksum, 'artifact checksum')
        artifact_magic, schema, header_size, artifact_size, artifact_target = struct.unpack_from('<IHHIB', artifact)
        require(artifact_magic == 0x59524452 and schema == 8 and header_size == 152
                and artifact_size == length and artifact_target == target, 'artifact envelope')
        require(artifact[136:152] == identity, 'artifact identity')
        for section in range(16, 80, 8):
            start, extent = struct.unpack_from('<II', artifact, section)
            require(start + extent <= length and (not extent or start >= header_size), 'artifact section range')
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'overlapping artifacts')
    require({(b'terrain', 0), (b'terrain', 1)} <= keys, 'missing terrain/backend')
    return keys


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    args = parser.parse_args()
    verify_shader_pack(args.pack.read_bytes())
    print('Shader pack container and required terrain artifacts OK')
