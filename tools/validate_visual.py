"""Run isolated vanilla/native scene captures sequentially and retain local image differences."""
import argparse
from contextlib import ExitStack
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time

from benchmark import ROOT, gradle_command, locked_world, process_options, terminate_tree, write_json


def check_capture_log(text):
    if 'Serialization errors:' in text or 'Failed to decode value' in text:
        raise RuntimeError('Scene input failed Minecraft decoding; inspect game.log before accepting its images')
    if '[critical]' in text or 'VUID-' in text or re.search(r'\[error\][^\n]*(?:d3d12 validation|vk Validation|RHI validation)|\[error\][^\n]*\r?\n(?:d3d12 validation|vk Validation|RHI validation)', text):
        raise RuntimeError('GPU validation or native abort in capture; inspect game.log')


def link_region_storage(world, save, cleanup):
    import _winapi

    cleanup.enter_context(locked_world(world))
    for directory, folders, files in os.walk(world):
        source = Path(directory)
        target = save / source.relative_to(world)
        target.mkdir(parents=True, exist_ok=True)
        for folder in tuple(folders):
            if folder in {'region', 'poi', 'entities'}:
                link = target / folder
                _winapi.CreateJunction(str(source / folder), str(link))
                cleanup.callback(link.rmdir)
                folders.remove(folder)
        for name in files:
            if name not in {'session.lock', 'level.dat_old'}:
                shutil.copy2(source / name, target / name)


def capture(world, options, scenes, output, mode, configuration, validation, tracy_images=False, in_place_world=False):
    with ExitStack() as cleanup:
        return capture_session(world, options, scenes, output, mode, configuration, validation, tracy_images, in_place_world, cleanup)


def capture_session(world, options, scenes, output, mode, configuration, validation, tracy_images, in_place_world, cleanup):
    session = output / mode
    session.mkdir(parents=True, exist_ok=False)
    game = session / 'game'
    save = game / 'saves/validation'
    if in_place_world:
        link_region_storage(world, save, cleanup)
    else:
        with locked_world(world):
            shutil.copytree(world, save, ignore=shutil.ignore_patterns('session.lock'))
    shutil.copyfile(options, game / 'options.txt')
    packs = options.parent / 'resourcepacks'
    if packs.is_dir():
        shutil.copytree(packs, game / 'resourcepacks')
    shutil.copyfile(scenes, session / 'scenes.json')
    command = gradle_command('runClientReference' if mode == 'reference' else 'runClientNative',
                            '-PgameRunDir=' + str(game), '-PquickPlayWorld=validation', '-PgameUsername=RayniumTest',
                            '-PvalidationConfig=' + str(session / 'scenes.json'), '-PnativeConfig=' + configuration,
                            '-PrhiValidation=' + str(validation).lower(), '-PnativeProfiler=false',
                            '-PcaptureTracyImages=' + str(tracy_images).lower(),
                            '-PrhiBackend=' + ('vulkan' if mode == 'reference' else mode))
    if in_place_world:
        command.append('-PvalidationReadOnlyRegions=true')
    write_json(session / 'command.json', command)
    print(f'Capturing {mode}: {session}', flush=True)
    count = len(json.loads(scenes.read_text(encoding='utf-8-sig'))['scenes'])
    process = None
    try:
        with (session / 'game.log').open('wb') as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, **process_options())
            deadline = time.monotonic() + 300 * (count + 2)
            while process.poll() is None:
                if time.monotonic() > deadline:
                    raise TimeoutError(f'{mode} capture exceeded scene timeouts')
                # Native abort may leave a Windows crash dialog with a live Java process.
                text = (session / 'game.log').read_text(encoding='utf-8', errors='replace')
                if '[critical]' in text:
                    raise RuntimeError(f'{mode} native abort; inspect game.log')
                time.sleep(1)
        if process.returncode:
            raise RuntimeError(f'{mode} exited with code {process.returncode}; inspect {session / "game.log"}')
        result = json.loads((session / 'visual-result.json').read_text(encoding='utf-8'))
        if result['status'] != 'completed' or result['index'] != count:
            detail = {key: result.get(key) for key in ('status', 'phase', 'index', 'error')}
            raise RuntimeError(f'{mode} failed: {detail}')
        check_capture_log((session / 'game.log').read_text(encoding='utf-8', errors='replace'))
    finally:
        terminate_tree(process)
    print(f'Completed {mode}: {count} captures', flush=True)


def compare(output, modes):
    import numpy as np
    from PIL import Image
    reference = output / 'reference'
    if not reference.is_dir():
        return
    for mode in modes:
        if mode == 'reference':
            continue
        directory = output / mode / 'differences'
        directory.mkdir(exist_ok=True)
        report = {'status': 'requires-local-review', 'images': []}
        paths=set(reference.glob('*.png'))
        result=read_capture_result(reference)
        for scene in result['scenes']:
            paths.update(reference / name for name in scene.get('exports', []))
        for path in sorted(paths):
            relative=path.relative_to(reference)
            target = output / mode / relative
            if not target.is_file():
                raise RuntimeError(f'Missing image: {target}')
            left = np.array(Image.open(path).convert('RGBA'), dtype=np.int16)
            right = np.array(Image.open(target).convert('RGBA'), dtype=np.int16)
            if left.shape != right.shape:
                raise RuntimeError(f'Image extent differs: {target}')
            delta = np.abs(left - right).astype(np.uint8)
            error = delta.max(axis=2)
            difference=directory / relative
            difference.parent.mkdir(parents=True,exist_ok=True)
            visible = np.maximum(delta[:, :, :3], delta[:, :, 3:4])
            Image.fromarray(np.minimum(visible.astype(np.uint16) * 8, 255).astype(np.uint8)).save(difference)
            tiles = []
            for y in range(0, error.shape[0], 64):
                for x in range(0, error.shape[1], 64):
                    tile = error[y:y+64, x:x+64]
                    if tile.max() > 2:
                        tiles.append({'x': x, 'y': y, 'max': int(tile.max()), 'pixelsAbove2': int((tile > 2).sum())})
            report['images'].append({'name': relative.with_suffix('').as_posix(), 'maximumChannelError': int(error.max()),
                                     'maximumRgbaErrors': [int(value) for value in delta.max(axis=(0, 1))],
                                     'pixelsAbove2': int((error > 2).sum()), 'tiles': tiles})
        write_json(directory / 'report.json', report)


def read_capture_result(directory):
    return json.loads((directory / 'visual-result.json').read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--world', type=Path)
    parser.add_argument('--options', type=Path)
    parser.add_argument('--scenes', type=Path, default=ROOT / 'tools/validation/vanilla-scenes.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--modes', nargs='+', choices=['reference', 'd3d12', 'vulkan'], default=['reference', 'd3d12', 'vulkan'])
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--no-validation', action='store_true')
    parser.add_argument('--tracy-images', action='store_true', help='Exercise explicit Minecraft Tracy image capture')
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--in-place-world', action='store_true', help='Link original region data read-only and isolate session metadata on Windows')
    args = parser.parse_args()
    output = args.output.resolve()
    if not args.compare_only:
        if args.world is None or args.options is None:
            parser.error('--world and --options are required for capture')
        if output.is_relative_to(args.world.resolve()):
            parser.error('--output must be outside the source world')
        output.mkdir(parents=True, exist_ok=True)
        for mode in args.modes:
            capture(args.world.resolve(), args.options.resolve(), args.scenes.resolve(), output, mode, args.configuration, not args.no_validation, args.tracy_images, args.in_place_world)
    compare(output, args.modes)


if __name__ == '__main__':
    main()
