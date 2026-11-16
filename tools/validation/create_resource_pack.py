"""Create deterministic model, texture, animation and font inputs for visual comparisons."""
import argparse
import json
from pathlib import Path
import shutil
import zipfile


def main():
    from PIL import Image
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--minecraft-jar', required=True, type=Path)
    parser.add_argument('--options', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--resource-format', default='97.1')
    parser.add_argument('--truetype-font', type=Path, help='Local font copied only to the generated test pack')
    parser.add_argument('--oversized-gui-item', action='store_true', help='Exercise the original oversized diamond item renderer')
    parser.add_argument('--translucent-compass', action='store_true', help='Exercise translucent special item glint with ordinary texture replacement')
    parser.add_argument('--translucent-sorting-items', action='store_true', help='Exercise overlapping translucent quads in item atlas and oversized GUI targets')
    args = parser.parse_args()
    version = [int(value) for value in args.resource_format.split('.')]
    if len(version) != 2 or any(value < 0 for value in version):
        parser.error('--resource-format must be major.minor')
    root = args.output / 'resourcepacks/raynium-validation'
    root.mkdir(parents=True, exist_ok=False)

    def write(name, value):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    def texture(name, width, height, pixel):
        path = root / ('assets/minecraft/textures/' + name + '.png')
        path.parent.mkdir(parents=True, exist_ok=True)
        image = Image.new('RGBA', (width, height))
        image.putdata([pixel(x, y) for y in range(height) for x in range(width)])
        image.save(path)

    write('pack.mcmeta', {'pack': {'description': 'Raynium deterministic visual inputs', 'min_format': version, 'max_format': version}})
    texture('block/diamond_block', 32, 64,
            lambda x, y: ((240, 40, 150, 255) if (x // 4 + y // 4) % 2 else (30, 220, 240, 255)) if y < 32
            else ((240, 210, 30, 255) if (x // 4 + y // 4) % 2 else (30, 50, 220, 255)))
    write('assets/minecraft/textures/block/diamond_block.png.mcmeta', {'animation': {'frametime': 2, 'interpolate': True}})
    texture('block/blue_stained_glass', 64, 64,
            lambda x, y: (70, 180, 230, 190 if x < 3 or y < 3 or x >= 61 or y >= 61 else 75))
    texture('item/diamond_sword', 32, 32,
            lambda x, y: (240, 70, 210, 255) if abs(x + y - 31) < 5 else (0, 0, 0, 0))
    write('assets/minecraft/models/block/diamond_block.json', {
        'textures': {'all': 'minecraft:block/diamond_block', 'particle': 'minecraft:block/diamond_block'},
        'elements': [{'from': start, 'to': end, 'faces': {face: {'texture': '#all'} for face in ('north', 'south', 'east', 'west', 'up', 'down')}}
                     for start, end in [([2, 0, 2], [14, 12, 14]), ([0, 12, 0], [16, 16, 16])]]})
    texture('font/raynium_validation', 16, 8,
            lambda x, y: (255, 255, 255, 255) if y in (0, 3, 7) or x % 8 in (0, 7) else (0, 0, 0, 0))
    with zipfile.ZipFile(args.minecraft_jar) as source:
        font = json.loads(source.read('assets/minecraft/font/default.json'))
        if args.translucent_compass:
            for name in source.namelist():
                if name.startswith('assets/minecraft/textures/item/compass_') and name.endswith('.png'):
                    with source.open(name) as stream, Image.open(stream) as original:
                        image = original.convert('RGBA')
                    image.putalpha(image.getchannel('A').point(lambda value: min(value, 128)))
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    image.save(target)
    font['providers'].insert(0, {'type': 'bitmap', 'file': 'minecraft:font/raynium_validation.png', 'height': 8, 'ascent': 7, 'chars': ['A世']})
    if args.truetype_font:
        target = root / 'assets/raynium/font/validation.ttf'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.truetype_font, target)
        font['providers'].insert(1, {'type': 'ttf', 'file': 'raynium:validation.ttf', 'size': 11, 'oversample': 2, 'shift': [0, 0]})
    write('assets/minecraft/font/default.json', font)
    if args.oversized_gui_item:
        write('assets/minecraft/items/diamond.json', {'oversized_in_gui': True,
              'model': {'type': 'minecraft:model', 'model': 'minecraft:item/raynium_giant_diamond'}})
        write('assets/minecraft/models/item/raynium_giant_diamond.json', {
            'parent': 'minecraft:item/generated', 'textures': {'layer0': 'minecraft:item/diamond'},
            'display': {'gui': {'scale': [3, 3, 3]}}})
    if args.translucent_sorting_items:
        texture('item/raynium_sort_red', 16, 16, lambda x, y: (255, 0, 0, 128))
        texture('item/raynium_sort_blue', 16, 16, lambda x, y: (0, 0, 255, 128))
        for item, scale in [('chest', 1), ('diamond', 3)]:
            model = 'minecraft:item/raynium_sort_' + item
            write('assets/minecraft/items/' + item + '.json', {
                'oversized_in_gui': scale > 1, 'model': {'type': 'minecraft:model', 'model': model}})
            geometry = {
                'gui_light': 'front',
                'textures': {'red': 'minecraft:item/raynium_sort_red', 'blue': 'minecraft:item/raynium_sort_blue'},
                'display': {'gui': {'rotation': [0, 0, 0], 'scale': [scale, scale, scale]}},
                'elements': [
                    {'from': [4, 0, 10], 'to': [16, 16, 10], 'shade': False, 'faces': {'south': {'texture': '#blue'}}},
                    {'from': [0, 0, 6], 'to': [12, 16, 6], 'shade': False, 'faces': {'south': {'texture': '#red'}}}]}
            if scale == 1:
                geometry['elements'].reverse()
            write('assets/minecraft/models/item/raynium_sort_' + item + '.json', geometry)
    options = args.options.read_text(encoding='utf-8').splitlines()
    options = [line for line in options if not line.startswith(('resourcePacks:', 'incompatibleResourcePacks:'))]
    options += ['resourcePacks:["vanilla","file/raynium-validation"]', 'incompatibleResourcePacks:[]']
    (args.output / 'options.txt').write_text('\n'.join(options) + '\n', encoding='utf-8')
    print(args.output.resolve())


if __name__ == '__main__':
    main()
