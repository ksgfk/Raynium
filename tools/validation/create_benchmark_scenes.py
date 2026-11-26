"""Write equivalent full-scene benchmark inputs for vanilla Vulkan and both native backends."""
import argparse
import json
from pathlib import Path


def point(seconds, x=0.5, y=185, z=26, yaw=180, pitch=15):
    return dict(seconds=seconds, x=x, y=y, z=z, yaw=yaw, pitch=pitch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--world', type=Path, required=True)
    parser.add_argument('--options', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--smoke', action='store_true', help='Use 2 s warmup and 8 s route; not a performance result')
    args = parser.parse_args()
    duration = 8 if args.smoke else 240
    base = dict(world=str(args.world.resolve()), optionsFile=str(args.options.resolve()),
                width=1920, height=1080, renderDistance=32, simulationDistance=8,
                warmupSeconds=2 if args.smoke else 60, startupTimeoutSeconds=600, shutdownTimeoutSeconds=180,
                captureTimeoutSeconds=240, terrainSubmission='auto', clearBenchmarkEntities=True, profilerEnabled=False, jfrEnabled=False,
                route=[point(0), point(duration/4, x=-12, yaw=155), point(duration/2, x=12, yaw=-155), point(duration, pitch=10)])
    # Seed each fill with one changed block so rerunning the fixture also succeeds.
    commands = [
        'execute store result storage raynium:benchmark rule int 1 run gamerule advance_weather',
        'execute if data storage raynium:benchmark {rule:1} run gamerule advance_weather false',
        'weather clear',
        'execute store result storage raynium:benchmark rule int 1 run gamerule advance_time',
        'execute if data storage raynium:benchmark {rule:1} run gamerule advance_time false',
        'data remove storage raynium:benchmark rule',
        'execute unless predicate {type:"minecraft:time_check",clock:"minecraft:overworld",value:6000} run time set 6000',
        'setblock 0 185 0 minecraft:barrier',
        'fill -24 180 -24 24 185 24 minecraft:air strict',
        'setblock -24 179 -24 minecraft:barrier',
        'fill -24 179 -24 24 179 24 minecraft:smooth_stone', 'tick unfreeze']
    entities = commands.copy()
    kinds = ['armor_stand', 'zombie', 'cow', 'sheep', 'slime', 'creeper', 'enderman', 'spider']
    for n in range(128):
        kind = kinds[n % len(kinds)]
        x, z = (n % 16 - 7.5)*2.7, (n // 16 - 4)*3
        extra = ',ShowArms:1b' if kind == 'armor_stand' else ',Size:1' if kind == 'slime' else ',powered:1b' if kind == 'creeper' else ''
        entities.append(f'summon minecraft:{kind} {x} 180 {z} {{Tags:["raynium_bench"],NoAI:1b,NoGravity:1b,Silent:1b,Invulnerable:1b,PersistenceRequired:1b,CustomName:"Entity {n}",CustomNameVisible:1b{extra}}}')
    for slot, item in [('armor.head','diamond_helmet'), ('armor.chest','diamond_chestplate'), ('armor.legs','diamond_leggings'),
                       ('armor.feet','diamond_boots'), ('weapon.mainhand','diamond_sword')]:
        entities.append(f'item replace entity @e[type=minecraft:armor_stand,tag=raynium_bench] {slot} with minecraft:{item}[enchantments={{unbreaking:1}}]')
    particles = commands + ['weather rain', 'fill -18 180 -18 18 180 10 minecraft:campfire',
                             'fill -18 180 -19 18 184 -19 minecraft:blue_stained_glass',
                             'fill -18 180 12 18 184 12 minecraft:glass',
                             'fill -18 180 14 18 180 18 minecraft:water']
    gui = commands + ['give @s minecraft:stick', 'clear @s']
    items = ['clock','compass','diamond_sword','enchanted_book','filled_map','shield','bow','crossbow','trident',
             'white_banner','blue_shulker_box','end_crystal','spyglass','recovery_compass','brush','wolf_armor',
             'leather_chestplate','netherite_chestplate','totem_of_undying','glass','water_bucket','lava_bucket',
             'enchanted_golden_apple','firework_rocket','splash_potion','mace','carrot_on_a_stick','bundle']
    gui += [f'give @s minecraft:{item}' for item in items]
    gui += [f'item replace entity @s {slot} with minecraft:{item}[enchantments={{unbreaking:1}}]'
            for slot,item in [('armor.head','diamond_helmet'),('armor.chest','diamond_chestplate'),
                              ('armor.legs','diamond_leggings'),('armor.feet','diamond_boots')]]
    workloads = {
        'entities': dict(commands=entities, oit=False),
        'particles': dict(commands=particles, oit=True),
        'gui': dict(commands=gui, screen='inventory', playerMode='survival', oit=False,
                    route=[point(0,y=180,z=20,pitch=0),point(duration,y=180,z=20,pitch=0)]),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, workload in workloads.items():
        for mode in ('reference','vulkan','d3d12'):
            config = {**base, **workload, 'renderer':'vanilla' if mode=='reference' else 'raynium',
                      'backend':'vulkan' if mode=='reference' else mode,
                      'outputDir':str((args.output.parent/('smoke' if args.smoke else 'measurements')/name/mode).resolve())}
            (args.output/f'{name}-{mode}.json').write_text(json.dumps(config, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(args.output.resolve())


if __name__ == '__main__':
    main()
