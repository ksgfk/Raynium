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
ROUTES = {
    'features': ([JAVA+'takeover/scene/SceneFeatures.java', JAVA+'mixin/takeover/FeatureDispatcherSceneMixin.java'], ['native/src/scene_pass.cpp'], None),
    'particles': ([JAVA+'takeover/scene/SceneFeatures.java'], ['native/src/scene_pass.cpp'], None),
    'atlas': ([JAVA+'takeover/TakeoverAtlas.java'], ['native/src/texture_resources.cpp'], []),
    'lightmap': ([JAVA+'takeover/CpuLightmap.java'], ['native/src/texture_resources.cpp'], []),
    'terrain': ([JAVA+'terrain/TakeoverTerrain.java'], ['native/src/terrain_draw.cpp', 'native/src/mesh_pass.cpp'],
                ['terrain','terrain_cutout','terrain_translucent','terrain_oit_bounds','terrain_oit_transmittance','terrain_oit_accumulate']),
    'gui': ([JAVA+'takeover/TakeoverGui.java'], ['native/src/gui_pass.cpp'], ['gui_color','gui_textured','gui_text']),
    'sky': ([JAVA+'mixin/takeover/SkySceneMixin.java'], ['native/src/scene_pass.cpp'], None),
    'clouds': ([JAVA+'mixin/takeover/CloudSceneMixin.java'], ['native/src/scene_pass.cpp'], None),
    'weather': ([JAVA+'mixin/takeover/WeatherSceneMixin.java'], ['native/src/scene_pass.cpp'], None),
    'border': ([JAVA+'mixin/takeover/WorldBorderSceneMixin.java'], ['native/src/scene_pass.cpp'], None),
    'panorama': ([JAVA+'takeover/scene/ScenePanorama.java'], ['native/src/scene_pass.cpp'], None),
    'depth': ([], ['native/src/scene_pass.cpp', 'native/src/solid_pipeline.cpp'], None),
    'bounds': ([], ['native/src/mesh_pass.cpp', 'native/src/solid_pipeline.cpp'], ['oit_bounds_cull']),
    'composite': ([], ['native/src/mesh_pass.cpp', 'native/src/solid_pipeline.cpp'], ['oit_composite']),
    'outline': ([JAVA+'takeover/scene/SceneFeatures.java'], ['native/src/scene_post.inc'], None),
    'tracy': ([JAVA+'takeover/TakeoverTracyImages.java'], ['native/src/scene_post.inc', 'native/src/solid_pipeline.cpp'], None),
}


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


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
    parser.add_argument('--shaders', type=Path, default=ROOT/'build/vanilla/shaders')
    parser.add_argument('--captures', nargs='*', type=Path, default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    catalog = read(args.catalog)
    interfaces = {p['id']: p for p in read(ROOT/'src/client/resources/assets/raynium/scene-interfaces.json')}
    packed = verify_shader_pack((ROOT/'src/client/resources/assets/raynium/shaderpacks/default.rnsp').read_bytes())
    consumers = references(args.minecraft_source)
    selections, effects = observed(args.captures)
    rows = []
    for entry in catalog:
        name = entry['names'][0].split('/')[0]
        origins = consumers.get(name, [])
        kind = route(name, origins)
        adapters, native, override = ROUTES[kind]
        interface = interfaces[entry['id']]
        if interface['names'] != entry['names']:
            raise ValueError('Catalog/interface identity mismatch: '+name)
        programs = [interface['program']] if override is None else override
        for program in programs + [interface['program']]:
            for backend in (0, 1):
                if (program.encode(), backend) not in packed:
                    raise ValueError('Missing packed backend artifact: '+program)
        for path in adapters + native:
            if not (ROOT/path).is_file(): raise ValueError('Missing implementation: '+path)
        rows.append({**entry, 'minecraftConsumers': origins, 'adapter': kind, 'javaSources': adapters, 'nativeSources': native,
                     'execution': 'cpu-generated-upload' if kind in ('atlas','lightmap') else 'native-raster',
                     'convertedProgram': interface['program'], 'executedProgramFamily': programs,
                     'selectedInCaptures': selections.get(entry['id'], []), 'visualAcceptance': 'requires-review'})
    posts = []
    for chain in read(args.shaders/'post-chains.json'):
        posts.append({**chain, 'javaSource': JAVA+'takeover/scene/ScenePost.java', 'nativeSource': 'native/src/scene_post.inc',
                      'requestedInCaptures': effects.get('minecraft:'+chain['name'], []), 'visualAcceptance': 'requires-review'})
    report = {'minecraft': '26.3', 'status': 'implementation-inventory-with-selection-evidence',
              'notes': ['Selection proves that a pipeline was chosen, not that its pixels were reviewed.',
                        'CPU equivalents and dedicated native terrain/GUI/composite programs do not select the converted vanilla shader.',
                        'The same feature adapter serves world, first-person and offscreen GUI draws.'],
              'pipelines': rows, 'postEffects': posts}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f'{len(rows)} pipelines, {len(posts)} post effects, {sum(bool(p["selectedInCaptures"]) for p in rows)} selected in supplied captures')


if __name__ == '__main__':
    main()
