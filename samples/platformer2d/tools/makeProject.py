#!/usr/bin/env python3
"""Writes Neon Sprint's project file, its images' .meta files and its scene.

The scene is laid out here once; after that it's the editor's to change (open
the project in SfsEditor). Running this again resets it. Asset ids are fixed
(made from each file's path), so the scene's references stay valid.
"""
import json
import os
import uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
NAMESPACE = uuid.UUID('6b1f7c2e-9d3a-4f5b-8c21-0a7e5d4b3c19')
TILE = 64.0


def asset(relative, settings):
    """Writes relative's .meta (fixed id, import settings); returns its name."""
    asset_id = uuid.uuid5(NAMESPACE, relative).hex
    with open(os.path.join(ROOT, relative + '.meta'), 'w') as out:
        json.dump({'formatVersion': 1, 'id': asset_id, 'settings': settings}, out, indent=2)
        out.write('\n')
    return 'asset:' + asset_id


def transform(x, y, sx=1.0, sy=1.0, z=0.0):
    return {'position': [x, y, z], 'rotation': [1.0, 0.0, 0.0, 0.0], 'scale': [sx, sy, 1.0]}


def sprite(image, frame=0, anchor=(0.5, 0.5)):
    return {'image': image, 'frame': frame, 'anchor': list(anchor), 'renderOffset': [0.0, 0.0]}


def main():
    pixel = {'filter': 'nearest'}
    runner = asset('assets/sprites/runner.png', dict(pixel, spriteCell=[16, 16]))
    props = asset('assets/sprites/props.png', dict(pixel, spriteCell=[16, 16]))
    glitch = asset('assets/sprites/glitch.png', dict(pixel, spriteCell=[16, 64]))
    sky = asset('assets/backdrop/sky.png', pixel)
    city_far = asset('assets/backdrop/city_far.png', pixel)
    city_near = asset('assets/backdrop/city_near.png', pixel)

    entities = [
        {'NameComponent': {'name': 'Run'},
         'TransformComponent': transform(0.0, 0.0),
         'ScriptComponent': {'script': 'RunDirector',
                             'fields': {'props': props, 'glitch': glitch}}},
        {'NameComponent': {'name': 'Runner'},
         'TransformComponent': transform(3 * TILE, 0.0),
         'SpriteComponent': sprite(runner, 0, (0.5, 13.0 / 16.0)),
         'RenderLayerComponent': {'layer': 3},
         'PointLightComponent': {'color': [0.35, 0.95, 1.0], 'intensity': 1.6, 'radius': 320.0,
                                 'offset': [0.0, 0.0, 60.0], 'castsShadow': False,
                                 'sourceRadius': 0.05},
         'ScriptComponent': {'script': 'Runner', 'fields': {}}},
        {'NameComponent': {'name': 'Camera'},
         'TransformComponent': transform(6 * TILE, -2 * TILE),
         'CameraComponent': {'active': True, 'orthographic': True, 'orthoHeight': 760.0,
                             'zoom': 1.0, 'nearPlane': 1.0, 'farPlane': 100000.0,
                             'bloom': True, 'bloomIntensity': 0.9, 'bloomThreshold': 0.85,
                             'bloomSoftness': 0.5, 'bloomRadius': 1.0, 'depthOfField': False},
         'CameraFollowComponent': {'target': -1, 'offset': [5 * TILE, -1.5 * TILE, 0.0],
                                   'smoothing': 7.0, 'shakeMaxOffset': 36.0}},
        # The backdrop: sky fixed on screen, two skylines drifting slower than
        # the world.
        {'NameComponent': {'name': 'Sky'},
         'TransformComponent': transform(0.0, -2 * TILE, 5.0, 5.0),
         'SpriteComponent': sprite(sky),
         'RenderLayerComponent': {'layer': -30},
         'ScriptComponent': {'script': 'Parallax',
                             'fields': {'follow': 1.0, 'width': 2560.0, 'copies': 2,
                                        'height': 0.0}}},
        {'NameComponent': {'name': 'City Far'},
         'TransformComponent': transform(0.0, -2 * TILE, 4.0, 4.0),
         'SpriteComponent': sprite(city_far),
         'RenderLayerComponent': {'layer': -20},
         'ScriptComponent': {'script': 'Parallax',
                             'fields': {'follow': 0.8, 'width': 2048.0, 'copies': 4,
                                        'height': -120.0}}},
        {'NameComponent': {'name': 'City Near'},
         'TransformComponent': transform(0.0, -2 * TILE, 4.0, 4.0),
         'SpriteComponent': dict(sprite(city_near), tint=[0.8, 0.85, 1.0, 1.0]),
         'RenderLayerComponent': {'layer': -10},
         'ScriptComponent': {'script': 'Parallax',
                             'fields': {'follow': 0.55, 'width': 2048.0, 'copies': 4,
                                        'height': -220.0}}},
    ]

    # The ground the run starts on: a solid, and the blocks that show it.
    tiles, depth = 16, 3
    entities.append({'NameComponent': {'name': 'Start Ground'},
                     'TransformComponent': transform(tiles * TILE / 2 - 2 * TILE, depth * TILE / 2),
                     'BoxCollider2D': {'half': [tiles * TILE / 2, depth * TILE / 2],
                                       'offset': [0.0, 0.0]},
                     'SolidObject': {}})
    for column in range(tiles):
        for row in range(depth):
            entities.append({'NameComponent': {'name': 'Ground Block'},
                             'TransformComponent': transform((column - 2 + 0.5) * TILE,
                                                             (row + 0.5) * TILE),
                             'SpriteComponent': sprite(props, 0 if row == 0 else 1),
                             'RenderLayerComponent': {'layer': 0}})
    # A wall at the start's back: nowhere to go but forward (and a first wall
    # to slide down and jump off).
    wall_height = 6
    entities.append({'NameComponent': {'name': 'Back Wall'},
                     'TransformComponent': transform(-2.5 * TILE, -(wall_height - depth) * TILE / 2),
                     'BoxCollider2D': {'half': [TILE / 2, (wall_height + depth) * TILE / 2],
                                       'offset': [0.0, 0.0]},
                     'SolidObject': {}})
    for row in range(-wall_height, depth):
        entities.append({'NameComponent': {'name': 'Wall Block'},
                         'TransformComponent': transform(-2.5 * TILE, (row + 0.5) * TILE),
                         'SpriteComponent': sprite(props, 0 if row == -wall_height else 1),
                         'RenderLayerComponent': {'layer': 0}})
    entities.append({'NameComponent': {'name': 'Beacon'},
                     'TransformComponent': transform(8 * TILE, -1.5 * TILE, 1.0, 2.0),
                     'SpriteComponent': sprite(props, 9),
                     'RenderLayerComponent': {'layer': -1},
                     'PointLightComponent': {'color': [1.0, 0.85, 0.4], 'intensity': 2.2,
                                             'radius': 420.0, 'offset': [0.0, 0.0, 60.0],
                                             'castsShadow': False, 'sourceRadius': 0.05}})

    scene = {
        'formatVersion': 2,
        'systems': ['CameraSystem'],
        'render': {
            'style': {'kind': 'flat', 'pixelsPerUnit': 0.25, 'tilePixels': [32.0, 16.0],
                      'heightPixels': 8.0, 'yDown': True, 'ySort': False},
            'lighting': {'sunDirection': [0.0, 0.0, 1.0], 'sunColor': [1.0, 1.0, 1.0],
                         'sunStrength': 0.0, 'ambient': 0.62, 'skyColor': [0.03, 0.02, 0.08]},
            'shadows': {'enabled': False, 'distance': 40.0, 'depthBias': 0.0015,
                        'softness': 1.5, 'pointLights': 0, 'pointResolution': 256},
            'camera': {'target': [6 * TILE, -TILE, 0.0], 'yaw': 1.5707964,
                       'pitch': 1.5707964, 'distance': 50000.0, 'orthographic': True,
                       'fieldOfView': 0.8, 'orthoHeight': 900.0},
        },
        'entities': entities,
    }
    os.makedirs(os.path.join(ROOT, 'scenes'), exist_ok=True)
    with open(os.path.join(ROOT, 'scenes', 'run.scene'), 'w') as out:
        json.dump(scene, out, indent=1)
        out.write('\n')

    with open(os.path.join(ROOT, 'project.sfsproj'), 'w') as out:
        json.dump({'formatVersion': 1, 'name': 'Neon Sprint', 'startScene': 'scenes/run.scene'},
                  out, indent=2)
        out.write('\n')
    print('Wrote the project in', ROOT)


if __name__ == '__main__':
    main()
