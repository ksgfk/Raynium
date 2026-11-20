"""Convert the exported, finite vanilla pipeline catalog to offline HLSL sources.

Inputs are the locked Minecraft assets and explicitly selected glslang/SPIRV-Cross tools.
Generated sources and their interface manifest are build artifacts, not runtime shader loading.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stdout + result.stderr)
    return result.stdout


def expand(path, root, stack=()):
    path = path.resolve()
    if path in stack:
        raise ValueError(f'Cyclic shader include: {path}')
    source = path.read_text(encoding='utf-8')
    def include(match):
        namespace, name = match[1].split(':', 1)
        if namespace != 'minecraft' or '..' in Path(name).parts:
            raise ValueError(f'Unsupported built-in include: {match[1]}')
        return expand(root / 'include' / name, root, (*stack, path))
    return re.sub(r'^\s*#include\s+<([^>]+)>\s*$', include, source, flags=re.M)


def raw_block(name, body, register):
    """Preserve SPIRV-Cross packoffsets without backend-dependent float3 reflection."""
    definitions, end = [], 0
    members = re.findall(r'\s*(row_major )?(float|uint|int)([1-4])?(?:x([1-4]))?\s+(\w+)\s*:\s*packoffset\(c(\d+)(?:\.([xyzw]))?\);', body)
    if not members or len(members) != body.count('packoffset'):
        raise ValueError(f'Unsupported constant block: {name}: {body}')
    for row_major, scalar, rows, columns, member, row, component in members:
        row = int(row); offset = 'xyzw'.index(component) if component else 0
        rows = int(rows or 1); columns = int(columns or 1)
        if columns == 1 and offset+rows > 4:
            raise ValueError(f'Constant crosses a register: {name}.{member}')
        if columns > 1 and not row_major:
            raise ValueError(f'Unexpected matrix major order: {name}.{member}')
        cast = 'asfloat' if scalar == 'float' else 'asint' if scalar == 'int' else ''
        def load(index, count, first=0):
            word = f'Raw_{name}[{index}]' + ('.' + 'xyzw'[first:first+count] if count < 4 else '')
            return f'{cast}({word})' if cast else word
        if columns > 1:
            value = f'{scalar}{rows}x{columns}(' + ', '.join(load(row+i, columns) for i in range(rows)) + ')'
            end = max(end, (row+rows)*16)
        else:
            value = load(row, rows, offset)
            end = max(end, row*16+(offset+rows)*4)
        definitions.append(f'#define {member} ({value})')
    count = (end+15)//16
    return f'[[vk::binding({register}, 0)]] cbuffer {name} : register(b{register}, space0) {{ uint4 Raw_{name}[{count}]; }};\n' + '\n'.join(definitions), count*16


def resource_declaration(declaration, kind, index, texel_formats):
    vk_binding = index*2 + (1 if kind == 's' else 0)
    result = f'[[vk::binding({vk_binding}, 1)]] ' + declaration
    if declaration.startswith('Buffer<'):
        typed = re.match(r'Buffer<(int|uint|float)[1-4]?> (\w+) ', declaration)
        fmt = texel_formats.get(typed[2]) if typed else None
        detail = re.fullmatch(r'(R|RG|RGBA)(8|16|32)_(SINT|UINT|FLOAT|SNORM|UNORM)', fmt or '')
        if not detail:
            raise ValueError('Unsupported texel buffer format for '+declaration)
        channels, bits, encoding = detail.groups()
        scalar, suffix = {'SINT': ('int', 'i'), 'UINT': ('uint', 'ui'), 'FLOAT': ('float', 'f'),
                          'SNORM': ('float', '_snorm'), 'UNORM': ('float', '')}[encoding]
        if scalar != typed[1] or (encoding == 'FLOAT' and bits == '8') or (encoding.endswith('NORM') and bits == '32'):
            raise ValueError(f'Texel buffer declaration {declaration} is incompatible with {fmt}')
        image_format = channels.lower() + bits + suffix
        result = f'#ifdef __spirv__\n[[vk::image_format("{image_format}")]]\n#endif\n' + result
    return result



def link_varyings(vertex, pixel):
    """Drop producer-only outputs before DXC independently packs the stage signatures."""
    inputs = re.search(r'struct SPIRV_Cross_Input\s*\{(.*?)\};', pixel, re.S)
    consumed = set(re.findall(r':\s*(TEXCOORD\d+)', inputs[1] if inputs else ''))
    output = re.search(r'struct SPIRV_Cross_Output\s*\{(.*?)\};', vertex, re.S)
    if output is None:
        raise ValueError('Vertex stage has no output interface')
    removed = []
    def member(match):
        if match[2] in consumed:
            return match[0]
        removed.append(match[1])
        return ''
    body = re.sub(r'^.*?\b(\w+)\s*:\s*(TEXCOORD\d+)\s*;\s*$', member, output[1], flags=re.M)
    vertex = vertex[:output.start(1)] + body + vertex[output.end(1):]
    for name in removed:
        vertex = re.sub(r'\s*stage_output\.'+re.escape(name)+r'\s*=\s*[^;]+;', '', vertex)
    return vertex


def program_key(sources, identity):
    stages = [hashlib.sha256(sources[stage].encode('utf-8')).hexdigest() for stage in ('VERTEX', 'FRAGMENT')]
    defines = {**dict.fromkeys(identity['defineFlags'], ''), **identity['defineValues']}
    fields = stages + [field for pair in sorted(defines.items()) for field in pair]
    return 'p_' + hashlib.sha256(chr(0).join(fields).encode('utf-8')).hexdigest()


def convert(entry, root, destination, glslang, cross, expanded=None):
    identity = entry['identity']
    sources = {}
    for stage, suffix in [('VERTEX', 'vsh'), ('FRAGMENT', 'fsh')]:
        shader = identity['shaders'][stage]
        if expanded is not None:
            sources[stage] = expanded[stage]
            continue
        if not shader.startswith('minecraft:') or '..' in Path(shader.split(':', 1)[1]).parts:
            raise ValueError(f'Non-vanilla shader: {shader}')
        sources[stage] = expand(root / (shader.split(':', 1)[1] + '.' + suffix), root)
    source_key = program_key(sources, identity)
    name = source_key[:58]
    directory = destination / name
    directory.mkdir(parents=True, exist_ok=True)
    if identity['shaders']['VERTEX'] == 'minecraft:core/debug_point':
        # One vertex stream element per instance; six generated corners preserve pixel-sized points.
        sources['VERTEX'] = sources['VERTEX'].replace('gl_PointSize = LineWidth;', '''
    const vec2 corners[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(1,1),vec2(-1,1),vec2(-1,-1));
    gl_Position.xy += corners[gl_VertexIndex % 6] * LineWidth / ScreenSize * gl_Position.w;''')
    combined = '\n'.join(sources.values())
    blocks = sorted(set(re.findall(r'layout\(std140\)\s+uniform\s+(\w+)', combined)))
    textures = sorted(set(re.findall(r'uniform\s+[iu]?sampler\w+\s+(\w+)\s*;', combined)))
    block_ids = {value: index for index, value in enumerate(blocks)}
    texture_ids = {value: index for index, value in enumerate(textures)}
    globals_, stages, wrappers, constant_sizes = {}, [], [], {}
    texel_formats = {u['name']: u['gpuFormat'] for u in identity.get('uniforms', []) if u['type'] == 'TEXEL_BUFFER'}
    inputs, resources, push_size = {}, {}, 0
    for stage, suffix, namespace in [('VERTEX', 'vert', 'VertexStage'), ('FRAGMENT', 'frag', 'PixelStage')]:
        source = sources[stage]
        defines = '#define RENDERPEARL_DEPTH_IS_ZERO_TO_ONE\n'
        defines += '\n'.join(f'#define {key} {value}' for key, value in sorted(identity['defineValues'].items()))
        defines += '\n' + '\n'.join(f'#define {value}' for value in sorted(identity['defineFlags']))
        source = re.sub(r'#version\s+\d+', '#version 450\n'+defines, source)
        source = re.sub(r'layout\(std140\)\s+uniform\s+(\w+)',
            lambda m: f'layout(std140,set=0,binding={block_ids[m[1]]}) uniform {m[1]}', source)
        source = re.sub(r'uniform\s+([iu]?sampler\w+)\s+(\w+)\s*;',
            lambda m: f'layout(set=1,binding={texture_ids[m[2]]}) uniform {m[1]} {m[2]};', source)
        path = directory / ('source.'+suffix)
        path.write_text(source, encoding='utf-8')
        spirv = directory / (suffix+'.spv')
        args = [glslang, '-V', '--target-env', 'vulkan1.2', '-S', suffix]
        run([*args, '-o', spirv, path])
        optimized = directory / (suffix + '-active.spv')
        run([*args, '-Os', '-o', optimized, path])
        reflection = json.loads(run([cross, optimized, '--reflect']))
        if stage == 'VERTEX':
            inputs = {v['name']: {'location': v['location'], 'type': v['type']} for v in json.loads(run([cross, spirv, '--reflect'])).get('inputs', [])}
        for uniform in reflection.get('ubos', []):
            resources[uniform['name']] = {'group': 0, 'binding': uniform['binding'], 'type': 'ubo'}
        for uniform in reflection.get('textures', []):
            resources[uniform['name']] = {'group': 1, 'binding': 2 * uniform['binding'], 'type': uniform['type']}
            if uniform['type'].endswith('samplerBuffer'):
                resources[uniform['name']]['texelFormat'] = texel_formats[uniform['name']]
        for uniform in reflection.get('push_constants', []):
            members = reflection['types'][uniform['type']]['members']
            for member in members:
                match = re.fullmatch(r'(?:float|int|uint|[iu]?vec([234])|mat([234]))', member['type'])
                if not match or 'array' in member:
                    raise ValueError('Unsupported push constant member ' + repr(member))
                size = int(match[2]) * member['matrix_stride'] if match[2] else 4 * int(match[1] or 1)
                push_size = max(push_size, member['offset'] + size)
        hlsl = run([cross, spirv, '--hlsl', '--shader-model', '60', '--rename-entry-point', 'main', 'GeneratedMain', suffix])
        # SPIRV-Cross converts DirectX pixel-position W into GLSL reciprocal W.
        # DXC's SPIR-V target already supplies reciprocal W unless explicitly overridden.
        hlsl = hlsl.replace('gl_FragCoord.w = 1.0 / gl_FragCoord.w;',
            '#ifndef __spirv__\n    gl_FragCoord.w = 1.0 / gl_FragCoord.w;\n#endif')
        hlsl = re.sub(r'\b_\d+_([A-Za-z]\w*)', r'\1', hlsl)
        if stage == 'VERTEX':
            for builtin, field in [('gl_VertexIndex', 'RnFirstVertex'), ('gl_InstanceIndex', 'RnFirstInstance')]:
                assign = f'{builtin} = int(stage_input.{builtin});'
                if assign in hlsl:
                    globals_['RnDrawBases'] = '#ifndef __spirv__\ncbuffer RnDrawBases : register(b0, space3) { int RnFirstVertex; uint RnFirstInstance; };\n#endif'
                    hlsl = hlsl.replace(assign, assign + f'\n#ifndef __spirv__\n    {builtin} += int({field});\n#endif')
        def push_block(match):
            block_name = match[1]
            declaration, padded = raw_block(block_name, match[2], 0)
            body = declaration.split('\n', 1)[1]
            payload = f'struct RnPushData {{ uint4 raw[{padded//16}]; }};'
            location = '#ifdef __spirv__\n[[vk::push_constant]]\n#endif\nConstantBuffer<RnPushData> RnPush : register(b0, space2);'
            body = body.replace('Raw_' + block_name + '[', 'RnPush.raw[')
            value = payload + '\n' + location + '\n' + body
            if 'RnPush' in globals_ and globals_['RnPush'] != value:
                raise ValueError('Incompatible push constant declarations across shader stages')
            globals_['RnPush'] = value
            return ''
        hlsl = re.sub(r'cbuffer (\w+)\s*\{(.*?)\};', push_block, hlsl, flags=re.S)
        def block(match):
            declaration, size = raw_block(match[1], match[3], int(match[2]))
            if match[1] in globals_ and globals_[match[1]] != declaration:
                raise ValueError(f'Inconsistent block across stages: {match[1]}')
            globals_[match[1]] = declaration; constant_sizes[match[1]] = size
            return ''
        hlsl = re.sub(r'cbuffer (\w+) : register\(b(\d+), space0\)\s*\{(.*?)\};', block, hlsl, flags=re.S)
        def resource(match):
            declaration, kind, index = match[0], match[1], int(match[2])
            declaration = resource_declaration(declaration, kind, index, texel_formats)
            globals_[match[0]] = declaration
            return ''
        hlsl = re.sub(r'^(?:Texture\w*<[^>]+>|Buffer<[^>]+>|SamplerState) \w+ : register\(([ts])(\d+), space1\);', resource, hlsl, flags=re.M)
        stages.append(f'namespace {namespace} {{\n{hlsl}\n}}')
        signature = re.search(r'(\w+) GeneratedMain\((.*?)\)', hlsl)
        if signature is None:
            raise ValueError('Missing converted entry point')
        result = signature[1] if signature[1] == 'void' else namespace+'::'+signature[1]
        parameters = '' if not signature[2] else namespace+'::SPIRV_Cross_Input input'
        call = f'{namespace}::GeneratedMain(' + ('input' if parameters else '') + ')'
        shader, entrypoint = ('vertex', 'VSMain') if stage == 'VERTEX' else ('pixel', 'PSMain')
        if stage == 'VERTEX':
            clip = "\n#ifndef __spirv__\noutput.gl_Position.y = -output.gl_Position.y;\n#endif\n"
            wrappers.append(f'[shader("vertex")] {result} VSMain({parameters}) {{ auto output = {call}; {clip}return output; }}')
        else:
            wrappers.append(f'[shader("{shader}")] {result} {entrypoint}({parameters}) {{ ' + ('return ' if result != 'void' else '') + call+'; }')
    # DXC's SPIR-V reflection can keep a cbuffer referenced only by a dead expression.
    # Preserve the declarations used by that expression without creating an unbound descriptor.
    for block_name in constant_sizes.keys() - resources.keys():
        _, definitions = globals_[block_name].split('\n', 1)
        registers = constant_sizes[block_name] // 16
        zero = ', '.join(['(uint4)0'] * registers)
        globals_[block_name] = f'static const uint4 Raw_{block_name}[{registers}] = {{ {zero} }};\n' + definitions
    stages[0] = link_varyings(stages[0], stages[1])
    source = '\n'.join(globals_.values())+'\n'+'\n'.join(stages)
    source += '\n'+'\n'.join(wrappers)+'\n'
    path = destination / (name+'.hlsl'); path.write_text(source, encoding='utf-8')
    for resource in resources.values():
        if resource['group'] == 1:
            sampler = resource['binding'] // 2
            resource['sampler'] = resource['binding'] + 1 if f'register(s{sampler}, space1)' in source else -1
    return {'program': name, 'key': source_key, 'inputs': inputs, 'resources': resources, 'pushBytes': push_size}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', required=True, type=Path)
    parser.add_argument('--shaders', required=True, type=Path)
    parser.add_argument('--glslang', required=True, type=Path)
    parser.add_argument('--spirv-cross', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--post-effects', type=Path, help='Built-in post_effect directory; defaults to the sibling of shaders')
    parser.add_argument('--parallel', type=int, default=8)
    args = parser.parse_args()
    entries = json.loads(args.catalog.read_text(encoding='utf-8'))
    args.output.mkdir(parents=True, exist_ok=True)
    # Program identity excludes raster state; convert each source/define combination only once.
    groups = {}
    for entry in entries:
        identity = entry['identity']
        key = json.dumps([identity['shaders'], identity['defineValues'], sorted(identity['defineFlags'])], sort_keys=True)
        groups.setdefault(key, []).append(entry)
    def work(group):
        try:
            result = convert(group[0], args.shaders, args.output, args.glslang, args.spirv_cross)
        except (ValueError, OSError) as failure:
            result = {'error': str(failure)}
        return [{'id': entry['id'], 'names': entry['names'], **result} for entry in group]
    with ThreadPoolExecutor(max_workers=args.parallel) as executor:
        results = [entry for group in executor.map(work, groups.values()) for entry in group]
    results.sort(key=lambda entry: entry['id'])
    (args.output / 'interfaces.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    failures = [entry for entry in results if 'error' in entry]
    for entry in failures:
        print(entry['names'][0]+': '+entry['error'])
    print(f'{len(results)-len(failures)}/{len(results)} variants converted, {len(groups)} distinct shader programs')
    if not failures:
        directory=args.post_effects or args.shaders.parent/'post_effect'
        chains=[]; programs={}
        for chain_id,chain_name in enumerate(('blur','creeper','entity_outline','invert','spider'),1):
            config=json.loads((directory/(chain_name+'.json')).read_text(encoding='utf-8'))
            for item in config['passes']:
                key=(item['vertex_shader'],item['fragment_shader'])
                if key not in programs:
                    identity={'shaders':dict(zip(('VERTEX','FRAGMENT'),key)),'defineValues':{},'defineFlags':[]}
                    programs[key]={'id':10000+len(programs),'names':[chain_name+'/'+str(len(programs))],
                        **convert({'identity':identity},args.shaders,args.output,args.glslang,args.spirv_cross)}
                item['pipeline']=programs[key]['id']
            chains.append({'id':chain_id,'name':chain_name,**config})
        (args.output/'post-interfaces.json').write_text(json.dumps(list(programs.values()),indent=2),encoding='utf-8')
        (args.output/'post-chains.json').write_text(json.dumps(chains,indent=2),encoding='utf-8')
        print(f'{len(chains)} built-in post chains, {len(programs)} programs converted')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
