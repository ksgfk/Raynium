"""Join the current vanilla catalog, implementation routes, shader artifacts and captured selections."""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from shader_pack import verify_shader_pack

JAVA = 'src/client/java/dev/raynium/client/'
IMMEDIATE_JAVA = [JAVA+'renderpearl/RayniumRenderPassBackend.java', JAVA+'renderpearl/RayniumPrograms.java']
IMMEDIATE_NATIVE = ['native/src/immediate_pass.cpp', 'native/src/immediate_draw.cpp']
TERRAIN_JAVA = [JAVA+'mixin/TerrainAnchorMixin.java', JAVA+'terrain/RetainedTerrain.java']
TERRAIN_NATIVE = ['native/src/terrain_feature.cpp', 'native/src/terrain_draw.cpp']
TERRAIN_PROGRAMS = ['terrain','terrain_cutout','terrain_translucent','terrain_oit_bounds','terrain_oit_transmittance','terrain_oit_accumulate']


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def require_sources(paths):
    for path in paths:
        if not (ROOT / path).is_file():
            raise ValueError('Missing implementation: ' + path)


def references(source):
    result = {}
    for path in source.rglob('*.java'):
        if path.name == 'RenderPipelines.java':
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            for name in re.findall(r'RenderPipelines\.([A-Z][A-Z_0-9]+)', line):
                result.setdefault(name, []).append({'path': path.relative_to(source).as_posix(), 'line': number})
    return result


def route(name, consumers):
    if name in ('GUI','GUI_INVERT','GUI_NAUSEA_OVERLAY','GUI_OPAQUE_TEXTURED_BACKGROUND','GUI_TEXT','GUI_TEXTURED',
                'GUI_TEXTURED_PREMULTIPLIED_ALPHA','GUI_TEXT_GRAYSCALE','GUI_TEXT_HIGHLIGHT','MOJANG_LOGO','CROSSHAIR','VIGNETTE'): return 'gui'
    if name.startswith('ANIMATE_SPRITE_'): return 'atlas'
    if name == 'LIGHTMAP': return 'lightmap'
    if name == 'TRACY_BLIT': return 'tracy'
    if name == 'ENTITY_OUTLINE_BLIT': return 'outline'
    if name == 'OIT_COMPOSITE': return 'composite'
    if name in ('BLIT_DEPTH_BOUNDS','OIT_DEPTH_BOUNDS_CULL'): return 'bounds'
    if name in ('BLIT_DEPTH','BLIT_DEPTH_DURING_DEPTH_BOUNDS','INTEGRATE_DEPTH'): return 'depth'
    if any('/rendertype/' in item['path'] for item in consumers): return 'features'
    classes = {
        'SingleQuadParticle.java':'particles', 'SkyRenderer.java':'sky', 'CloudRenderer.java':'clouds',
        'WeatherEffectRenderer.java':'weather', 'WorldBorderRenderer.java':'border', 'CubeMap.java':'panorama',
        'ChunkSectionLayer.java':'terrain', 'ChunkSectionsToRender.java':'terrain',
    }
    for item in consumers:
        class_name = Path(item['path']).name
        if class_name in classes: return classes[class_name]
    raise ValueError('Pipeline has no explicit adapter route: '+name)


def observed(paths):
    selections = {}
    effects = {}
    for path in paths:
        result = read(path)
        if result.get('status') != 'completed':
            raise ValueError('Incomplete capture cannot supply coverage: '+str(path))
        for scene in result['scenes']:
            evidence = {'result': str(path.resolve()), 'scene': scene['name'], 'renderer': result['renderer'], 'backend': result['backend']}
            for pipeline in scene.get('selectedVanillaPipelines', []):
                selections.setdefault(pipeline['id'], []).append(evidence)
            for effect in scene.get('postEffects', []):
                effects.setdefault(effect, []).append(evidence)
    return selections, effects


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, default=ROOT/'build/vanilla/pipelines.json')
    parser.add_argument('--minecraft-source', type=Path, required=True)
    parser.add_argument('--shaders', type=Path, default=ROOT/'build/vanilla/immediate')
    parser.add_argument('--captures', nargs='*', type=Path, default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    catalog = read(args.catalog)
    interfaces = {p['id']: p for p in read(args.shaders/'interfaces.json')}
    packed = verify_shader_pack((ROOT/'src/client/resources/assets/raynium/shaderpacks/default.rnsp').read_bytes())
    consumers = references(args.minecraft_source)
    selections, effects = observed(args.captures)
    rows = []
    for entry in catalog:
        name = entry['names'][0].split('/')[0]
        origins = consumers.get(name, [])
        kind = route(name, origins)
        adapters, native = (TERRAIN_JAVA, TERRAIN_NATIVE) if kind == "terrain" else (IMMEDIATE_JAVA, IMMEDIATE_NATIVE)
        interface = interfaces[entry['id']]
        if interface['names'] != entry['names']:
            raise ValueError('Catalog/interface identity mismatch: '+name)
        programs = TERRAIN_PROGRAMS if kind == 'terrain' else [interface['program']]
        for program in programs + [interface['program']]:
            for backend in (0, 1):
                if (program.encode(), backend) not in packed:
                    raise ValueError('Missing packed backend artifact: '+program)
        require_sources(adapters + native)
        rows.append({**entry, 'minecraftConsumers': origins, 'adapter': kind, 'javaSources': adapters, 'nativeSources': native,
                     'execution': 'native-raster',
                     'convertedProgram': interface['program'], 'executedProgramFamily': programs,
                     'selectedInCaptures': selections.get(entry['id'], []), 'visualAcceptance': 'requires-review'})
    posts = []
    for chain in read(args.shaders/'post-chains.json'):
        java_source, native_source = IMMEDIATE_JAVA[0], IMMEDIATE_NATIVE[0]
        require_sources((java_source, native_source))
        posts.append({**chain, 'javaSource': java_source, 'nativeSource': native_source,
                      'requestedInCaptures': effects.get('minecraft:'+chain['name'], []), 'visualAcceptance': 'requires-review'})
    report = {'minecraft': '26.3', 'status': 'implementation-inventory-with-selection-evidence',
              'notes': ['Selection and image review are recorded independently.',
                        'Retained terrain binds its stage-specific program at the vanilla terrain draw.',
                        'RenderPearl replays all other pipelines, including atlas animation, lightmaps and post effects.'],
              'pipelines': rows, 'postEffects': posts}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f'{len(rows)} pipelines, {len(posts)} post effects, {sum(bool(p["selectedInCaptures"]) for p in rows)} selected in supplied captures')


if __name__ == '__main__':
    main()
