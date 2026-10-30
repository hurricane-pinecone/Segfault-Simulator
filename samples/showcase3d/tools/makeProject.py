#!/usr/bin/env python3
"""Writes Glowfall Isle's project: its project file, materials, prefabs, data
asset, every asset's .meta file, and its scene.

The scene is laid out here once; after that it's the editor's to change (open
the project in SfsEditor). Running this again resets it. Asset ids are fixed
(made from each file's path), so references stay valid across runs.

The island is in metres, Z up: its top is the plane Z = 0, and the stations
showing each engine feature sit around it (see STATIONS in the scene below).
"""
import json
import math
import os
import uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
NAMESPACE = uuid.UUID('2d8e6a4c-31f7-4b9e-a5d0-7c1b9e3f6a58')


def asset_id(relative):
    return uuid.uuid5(NAMESPACE, relative).hex


def write_json(relative, data):
    path = os.path.join(ROOT, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as out:
        json.dump(data, out, indent=1)
        out.write('\n')


def asset(relative, settings=None, contents=None):
    """Writes relative (if contents are given) and its .meta; returns its name."""
    if contents is not None:
        write_json(relative, contents)
    write_json(relative + '.meta', {'formatVersion': 1, 'id': asset_id(relative),
                                    'settings': settings or {}})
    return 'asset:' + asset_id(relative)


# --- Materials -----------------------------------------------------------------


def standard(base, metallic=0.0, roughness=1.0, alpha='opaque', texture='', tiling=(1, 1),
             cutoff=0.5):
    return {'formatVersion': 1, 'baseColor': list(base), 'baseTexture': texture,
            'metallicRoughnessTexture': '', 'normalTexture': '', 'normalScale': 1,
            'tiling': list(tiling), 'offset': [0, 0], 'metallic': metallic,
            'roughness': roughness, 'alphaMode': alpha, 'alphaCutoff': cutoff}


class Graph:
    """A node graph, built a node at a time; links name pins as the editor does."""

    def __init__(self):
        self.nodes, self.links = [], []

    def node(self, kind, x, y, inputs=None, value=None):
        node = {'id': len(self.nodes) + 1, 'type': kind, 'position': [x, y]}
        if inputs:
            node['inputs'] = {k: list(v) if isinstance(v, (list, tuple)) else [v, v, v, v]
                              for k, v in inputs.items()}
        if value is not None:
            node['value'] = list(value) if isinstance(value, (list, tuple)) else [value, 0, 0, 0]
        self.nodes.append(node)
        return node['id']

    def link(self, src, out, dst, pin):
        self.links.append({'from': [src, out], 'to': [dst, pin]})

    def material(self, alpha='opaque'):
        return {'formatVersion': 2, 'alphaMode': alpha,
                'graph': {'nodes': self.nodes, 'links': self.links}}


def pulsing_glow(colour, strength, speed, base=(0.05, 0.05, 0.08, 1)):
    """Dark base, an emission colour that breathes over time (and along Z, so a
    tall crystal ripples)."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.25, 0, 0, 0], 'Metallic': [0.1, 0, 0, 0]})
    b = g.node('color', -700, -150, value=base)
    g.link(b, 'Colour', out, 'Base Colour')
    t = g.node('time', -900, 100)
    p = g.node('position', -900, 220)
    split = g.node('split', -720, 220)
    g.link(p, 'Position', split, 'In')
    speed_n = g.node('multiply', -560, 100, {'B': speed})
    g.link(t, 'Time', speed_n, 'A')
    phase = g.node('add', -420, 140)
    g.link(speed_n, 'Out', phase, 'A')
    g.link(split, 'B', phase, 'B')
    wave = g.node('sine', -300, 140)
    g.link(phase, 'Out', wave, 'In')
    level = g.node('remap', -170, 140, {'From Min': -1, 'From Max': 1, 'To Min': 0.45,
                                        'To Max': 1.0})
    g.link(wave, 'Out', level, 'In')
    c = g.node('color', -300, 0, value=colour)
    glow = g.node('multiply', -40, 60, {'B': strength})
    lit = g.node('multiply', -150, 40)
    g.link(c, 'Colour', lit, 'A')
    g.link(level, 'Out', lit, 'B')
    g.link(lit, 'Out', glow, 'A')
    g.link(glow, 'Out', out, 'Emission')
    return g.material()


def water():
    """Blue water: two scrolling noise layers mix deep and shallow colours."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.08, 0, 0, 0], 'Alpha': [0.78, 0, 0, 0]})
    uv = g.node('uv', -900, 0)
    t = g.node('time', -900, 150)
    drift = g.node('multiply', -760, 150, {'B': [0.02, 0.013, 0, 0]})
    g.link(t, 'Time', drift, 'A')
    moved = g.node('add', -620, 40)
    g.link(uv, 'UV', moved, 'A')
    g.link(drift, 'Out', moved, 'B')
    n = g.node('noise', -480, 40, {'Scale': [60, 0, 0, 0]})
    g.link(moved, 'Out', n, 'UV')
    deep = g.node('color', -480, -160, value=(0.02, 0.18, 0.32, 1))
    shallow = g.node('color', -480, -280, value=(0.12, 0.55, 0.62, 1))
    mix = g.node('lerp', -260, -120)
    g.link(deep, 'Colour', mix, 'A')
    g.link(shallow, 'Colour', mix, 'B')
    g.link(n, 'Out', mix, 'T')
    g.link(mix, 'Out', out, 'Base Colour')
    return g.material('blend')


def grass():
    """Grass blades darker at the root, sunny at the tip (the Grass node's Height)."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.85, 0, 0, 0]})
    blade = g.node('grass', -600, 80)
    root = g.node('color', -600, -160, value=(0.07, 0.24, 0.06, 1))
    tip = g.node('color', -600, -280, value=(0.42, 0.68, 0.2, 1))
    mix = g.node('lerp', -300, -120)
    g.link(root, 'Colour', mix, 'A')
    g.link(tip, 'Colour', mix, 'B')
    g.link(blade, 'Height', mix, 'T')
    g.link(mix, 'Out', out, 'Base Colour')
    return g.material()


def wheat():
    """Tall golden grass: honey at the root, pale straw at the tip."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.8, 0, 0, 0]})
    blade = g.node('grass', -600, 80)
    root = g.node('color', -600, -160, value=(0.45, 0.3, 0.06, 1))
    tip = g.node('color', -600, -280, value=(0.95, 0.82, 0.38, 1))
    mix = g.node('lerp', -300, -120)
    g.link(root, 'Colour', mix, 'A')
    g.link(tip, 'Colour', mix, 'B')
    g.link(blade, 'Height', mix, 'T')
    g.link(mix, 'Out', out, 'Base Colour')
    return g.material()


def turf_paint(holes, rects):
    """Grass coverage over the island's turf: full, but none in `holes` (x, y,
    radius) and `rects` (x0, y0, x1, y1), where there are floors and the wheat.
    Saved as GrassMaps::toJson() does: run-length bytes (count, value), base64."""
    import base64
    lo, hi, texel = -23.0, 23.0, 0.25
    n = int(round((hi - lo) / texel))
    cells = []
    for row in range(n):
        y = lo + (row + 0.5) * texel
        for col in range(n):
            x = lo + (col + 0.5) * texel
            bare = any((x - hx) ** 2 + (y - hy) ** 2 < r * r for hx, hy, r in holes) or \
                any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in rects)
            cells.append(0 if bare else 255)
    runs = bytearray()
    i = 0
    while i < len(cells):
        count = 1
        while i + count < len(cells) and cells[i + count] == cells[i] and count < 255:
            count += 1
        runs += bytes((count, cells[i]))
        i += count
    return {'grid': {'min': [lo, lo], 'max': [hi, hi], 'size': [n, n]},
            'coverage': base64.b64encode(bytes(runs)).decode('ascii')}


def runes():
    """A stone floor with cells of light that glow in a slow wave (Voronoi)."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.6, 0, 0, 0]})
    stone = g.node('color', -300, -200, value=(0.32, 0.3, 0.34, 1))
    g.link(stone, 'Colour', out, 'Base Colour')
    uv = g.node('uv', -1000, 0)
    cells = g.node('voronoi', -820, 0, {'Scale': [7, 0, 0, 0], 'Randomness': [0.9, 0, 0, 0]})
    g.link(uv, 'UV', cells, 'UV')
    # Light near each cell's centre: 1 - smoothstep(0, 0.06, distance).
    near = g.node('smoothstep', -700, 0, {'Edge 1': [0.0, 0, 0, 0], 'Edge 2': [0.06, 0, 0, 0]})
    g.link(cells, 'Distance', near, 'In')
    edge = g.node('oneMinus', -600, 0)
    g.link(near, 'Out', edge, 'In')
    t = g.node('time', -820, 200)
    phase = g.node('add', -640, 200)
    g.link(t, 'Time', phase, 'A')
    g.link(cells, 'Cell', phase, 'B')
    wave = g.node('sine', -500, 200)
    g.link(phase, 'Out', wave, 'In')
    level = g.node('remap', -380, 200, {'From Min': -1, 'From Max': 1, 'To Min': 0.1,
                                        'To Max': 1.0})
    g.link(wave, 'Out', level, 'In')
    lit = g.node('multiply', -380, 40)
    g.link(edge, 'Out', lit, 'A')
    g.link(level, 'Out', lit, 'B')
    cyan = g.node('color', -380, -80, value=(0.2, 0.9, 1.0, 1))
    glow = g.node('multiply', -200, 0, {'B': 3.0})
    tinted = g.node('multiply', -280, -20)
    g.link(cyan, 'Colour', tinted, 'A')
    g.link(lit, 'Out', tinted, 'B')
    g.link(tinted, 'Out', glow, 'A')
    g.link(glow, 'Out', out, 'Emission')
    return g.material()


def rock():
    """Stone tinted by world-space noise, so every block looks different."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.9, 0, 0, 0]})
    p = g.node('position', -900, 0)
    split = g.node('split', -760, 0)
    g.link(p, 'Position', split, 'In')
    xy = g.node('combine', -620, 0)
    g.link(split, 'R', xy, 'R')
    g.link(split, 'B', xy, 'G')
    n = g.node('noise', -480, 0, {'Scale': [1.3, 0, 0, 0]})
    g.link(xy, 'RG', n, 'UV')
    a = g.node('color', -480, -200, value=(0.36, 0.33, 0.38, 1))
    b = g.node('color', -480, -320, value=(0.55, 0.5, 0.52, 1))
    mix = g.node('lerp', -260, -120)
    g.link(a, 'Colour', mix, 'A')
    g.link(b, 'Colour', mix, 'B')
    g.link(n, 'Out', mix, 'T')
    v = g.node('vertexColor', -260, 60)
    shaded = g.node('multiply', -120, -40)
    g.link(mix, 'Out', shaded, 'A')
    g.link(v, 'Colour', shaded, 'B')
    g.link(shaded, 'Out', out, 'Base Colour')
    return g.material()


def warm_glow():
    """A lantern's glass: plain warm light."""
    g = Graph()
    out = g.node('output', 0, 0)
    c = g.node('color', -300, 0, value=(1.0, 0.72, 0.35, 1))
    g.link(c, 'Colour', out, 'Base Colour')
    strong = g.node('multiply', -150, 60, {'B': 4.0})
    g.link(c, 'Colour', strong, 'A')
    g.link(strong, 'Out', out, 'Emission')
    return g.material()


def checker():
    """The metal garden's plinths: a checkerboard of marble."""
    g = Graph()
    out = g.node('output', 0, 0, {'Roughness': [0.35, 0, 0, 0]})
    uv = g.node('uv', -500, 0)
    board = g.node('checkerboard', -300, 0, {'Scale': [4, 4, 0, 0],
                                             'Colour A': [0.92, 0.9, 0.86, 1],
                                             'Colour B': [0.18, 0.18, 0.22, 1]})
    g.link(uv, 'UV', board, 'UV')
    g.link(board, 'Colour', out, 'Base Colour')
    return g.material()


# --- Scene helpers -------------------------------------------------------------


def quat_z(angle):
    return [math.cos(angle / 2), 0.0, 0.0, math.sin(angle / 2)]


def transform(x, y, z=0.0, sx=1.0, sy=1.0, sz=1.0, yaw=0.0):
    return {'position': [x, y, z], 'rotation': quat_z(yaw), 'scale': [sx, sy, sz]}


def mesh(name, materials=None, tint=(1, 1, 1, 1), shadow=True, emits=False, strength=1.0,
         reach=1.0, light_shadows=True):
    return {'mesh': name, 'materials': materials or [], 'tint': list(tint),
            'castsShadow': shadow, 'emitsLight': emits, 'lightStrength': strength,
            'lightRange': reach, 'lightCastsShadows': light_shadows}


def light(colour, intensity, radius, offset=(0, 0, 0), shadow=False):
    return {'color': list(colour), 'intensity': intensity, 'radius': radius,
            'offset': list(offset), 'castsShadow': shadow, 'sourceRadius': 0.08}


def box(size, offset=(0, 0, 0), trigger=False):
    return {'size': list(size), 'offset': list(offset), 'trigger': trigger}


def body(mass=1.0, kinematic=False, friction=0.5, bounce=0.0, lock=False):
    return {'mass': mass, 'kinematic': kinematic, 'friction': friction, 'bounciness': bounce,
            'linearDamping': 0.05, 'angularDamping': 0.05, 'gravityScale': 1.0,
            'lockRotation': lock, 'bendsGrass': True}


def entity(name, tf, **components):
    entry = {'NameComponent': {'name': name}, 'TransformComponent': tf}
    entry.update(components)
    return entry


def instance(prefab, tf):
    return {'$prefab': prefab, '$overrides': {'': {'TransformComponent': tf}}}


def bob(amplitude=0.25, speed=1.6, spin=1.2, axis=(0, 0, 1)):
    return {'axis': list(axis), 'amplitude': amplitude, 'speed': speed, 'spin': spin}


def main():
    # Textures (drawn by makeArt.py).
    crate_tex = asset('assets/textures/crate.png', {'filter': 'linear'})
    lattice_tex = asset('assets/textures/lattice.png', {'filter': 'linear'})
    signs_tex = asset('assets/textures/signs.png', {'filter': 'nearest', 'spriteCell': [32, 32]})
    lighthouse = asset('assets/models/lighthouse.gltf')

    # Materials.
    m = {}
    m['crate'] = asset('assets/materials/crate.mat', contents=standard(
        (1, 1, 1, 1), roughness=0.85, texture=crate_tex))
    m['glass'] = asset('assets/materials/glass.mat', contents=standard(
        (0.55, 0.85, 1.0, 0.28), roughness=0.05, alpha='blend'))
    m['lattice'] = asset('assets/materials/lattice.mat', contents=standard(
        (1, 1, 1, 1), roughness=0.8, alpha='mask', texture=lattice_tex, tiling=(2, 2)))
    m['gold'] = asset('assets/materials/gold.mat', contents=standard(
        (1.0, 0.78, 0.34, 1), metallic=1.0, roughness=0.15))
    m['steel'] = asset('assets/materials/steel.mat', contents=standard(
        (0.75, 0.77, 0.8, 1), metallic=1.0, roughness=0.45))
    m['copper'] = asset('assets/materials/copper.mat', contents=standard(
        (0.95, 0.55, 0.38, 1), metallic=1.0, roughness=0.7))
    m['ceramic'] = asset('assets/materials/ceramic.mat', contents=standard(
        (0.2, 0.45, 0.85, 1), roughness=0.12))
    m['rubber'] = asset('assets/materials/rubber.mat', contents=standard(
        (0.85, 0.25, 0.3, 1), roughness=1.0))
    m['crystal'] = asset('assets/materials/crystal.mat', contents=pulsing_glow(
        (0.55, 0.3, 1.0, 1), 3.5, 2.2, base=(0.12, 0.06, 0.25, 1)))
    m['spark'] = asset('assets/materials/spark.mat', contents=pulsing_glow(
        (1.0, 0.72, 0.2, 1), 2.6, 5.0, base=(0.6, 0.42, 0.08, 1)))
    m['water'] = asset('assets/materials/water.mat', contents=water())
    m['grass'] = asset('assets/materials/grass.mat', contents=grass())
    m['wheat'] = asset('assets/materials/wheat.mat', contents=wheat())
    m['runes'] = asset('assets/materials/runes.mat', contents=runes())
    m['rock'] = asset('assets/materials/rock.mat', contents=rock())
    m['lantern'] = asset('assets/materials/lanternGlow.mat', contents=warm_glow())
    m['checker'] = asset('assets/materials/checker.mat', contents=checker())

    # The hero's tuning, shared data in an asset (a scriptable object).
    tuning = asset('assets/data/heroTuning.asset', contents={
        'formatVersion': 1, 'type': 'HeroTuning',
        'fields': {'runSpeed': 7.0, 'groundAcceleration': 55.0, 'airAcceleration': 22.0,
                   'jumpSpeed': 7.2, 'airJumps': 1, 'dashSpeed': 17.0, 'dashTime': 0.18,
                   'coyoteTime': 0.12, 'jumpBuffer': 0.12, 'turnSpeed': 3.2}})

    # Prefabs: things placed many times.
    crate = asset('prefabs/crate.prefab', contents={'formatVersion': 1, 'root': entity(
        'Crate', transform(0, 0, 0),
        MeshRendererComponent=mesh('builtin:cube', [m['crate']]),
        BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5)),
        RigidBody3DComponent=body(mass=0.7, friction=0.6))})
    pin = asset('prefabs/pin.prefab', contents={'formatVersion': 1, 'root': entity(
        'Pin', transform(0, 0, 0),
        MeshRendererComponent=mesh('glowfall:pin'),
        CapsuleCollider3DComponent={'radius': 0.17, 'height': 0.95, 'offset': [0, 0, 0.475],
                                    'trigger': False},
        RigidBody3DComponent=body(mass=0.6, friction=0.4, bounce=0.2),
        Pin={})})
    lantern = asset('prefabs/lantern.prefab', contents={'formatVersion': 1, 'root': dict(
        entity('Lantern', transform(0, 0, 0),
               MeshRendererComponent=mesh('glowfall:lanternPost'),
               BoxCollider3DComponent=box((0.3, 0.3, 2.4), (0, 0, 1.2))),
        children=[
            dict(entity('Glass', transform(0, 0, 2.05),
                        MeshRendererComponent=mesh('glowfall:lanternGlass', [m['lantern']],
                                                   shadow=False)), **{'$id': 1}),
            dict(entity('Light', transform(0, 0, 2.05),
                        PointLightComponent=light((1.0, 0.7, 0.4), 2.4, 9.0, shadow=True),
                        Lantern={}), **{'$id': 2}),
        ])})
    tree = asset('prefabs/tree.prefab', contents={'formatVersion': 1, 'root': entity(
        'Tree', transform(0, 0, 0),
        MeshRendererComponent=mesh('glowfall:tree'),
        CapsuleCollider3DComponent={'radius': 0.35, 'height': 3.0, 'offset': [0, 0, 1.5],
                                    'trigger': False})})
    spark = asset('prefabs/spark.prefab', contents={'formatVersion': 1, 'root': dict(
        entity('Spark', transform(0, 0, 0),
               SphereCollider3DComponent={'radius': 0.7, 'offset': [0, 0, 0],
                                          'trigger': True},
               Spark={}),
        children=[dict(entity('Spark Glow', transform(0, 0, 0),
                              MeshRendererComponent=mesh('glowfall:spark', [m['spark']],
                                                         shadow=False, emits=True,
                                                         strength=0.6, reach=0.6,
                                                         light_shadows=False),
                              Bob=bob()), **{'$id': 1})])})

    E = []  # the scene's entities

    # The game: its rules, and the camera that follows the hero.
    E.append(entity('Island', transform(0, 0, 0),
                    ScriptComponent={'script': 'IslandDirector', 'fields': {}}))
    # The hero: a physics body (a capsule that doesn't tip), and its model as a
    # child, which the script turns and squashes.
    E.append(dict(entity('Hero', transform(0, -18, 0.05),
                         CapsuleCollider3DComponent={'radius': 0.4, 'height': 1.7,
                                                     'offset': [0, 0, 0.85], 'trigger': False},
                         RigidBody3DComponent=body(mass=2.0, friction=0.0, lock=True),
                         PointLightComponent=light((0.6, 0.85, 1.0), 0.6, 4.0, (0, 0, 1.4)),
                         ScriptComponent={'script': 'Hero', 'fields': {'tuning': tuning,
                                                                       'fallLimit': -3.0}}),
                  children=[entity('Model', transform(0, 0, 0, yaw=math.pi / 2),
                                   MeshRendererComponent=mesh('glowfall:hero'),
                                   GrassBenderComponent={'fitMesh': True, 'radius': 0.5,
                                                         'offset': [0, 0, 0]})]))
    E.append(entity('Camera', transform(0, -26, 4),
                    CameraComponent={'active': True, 'orthographic': False, 'fieldOfView': 52.0,
                                     'orthoHeight': 20.0, 'zoom': 1.0, 'nearPlane': 0.1,
                                     'farPlane': 400.0, 'focusDistance': 8.0, 'bloom': True,
                                     'bloomIntensity': 0.55, 'bloomThreshold': 1.0,
                                     'bloomSoftness': 0.5, 'bloomRadius': 1.0,
                                     'depthOfField': True, 'focusRange': 30.0,
                                     'focusBlur': 0.3},
                    ScriptComponent={'script': 'FollowCamera', 'fields': {}}))

    # The island: a rocky plateau in the sea, flat on top (one box to walk on).
    E.append(entity('Island Ground', transform(0, 0, 0),
                    MeshRendererComponent=mesh('glowfall:island'),
                    BoxCollider3DComponent=box((52, 52, 4), (0, 0, -2))))
    E.append(entity('Sea', transform(0, 0, -0.55, 30, 30, 1),
                    MeshRendererComponent=mesh('builtin:plane', [m['water']], shadow=False)))

    def sign(name, frame, x, y, hint, radius=7.0):
        E.append(entity(name + ' Sign', transform(x, y, 0, 0.06, 0.06, 0.06),
                        SpriteComponent={'image': signs_tex, 'frame': frame,
                                         'anchor': [0.5, 1.0], 'renderOffset': [0, 0]},
                        Station={'title': name, 'hint': hint, 'radius': radius}))

    # Grass over the whole island's turf (not under the floors, nor the wheat).
    E.append(entity('Turf', transform(0, 0, 0),
                    MeshRendererComponent=mesh('glowfall:turf', shadow=False),
                    GrassComponent={'material': m['grass'], 'density': 170.0, 'height': 0.28,
                                    'heightVariation': 0.4, 'width': 0.03, 'stiffness': 0.45,
                                    'recovery': 3.0, 'drawDistance': 38.0,
                                    'paint': turf_paint(
                                        holes=[(0, 17, 4.2),     # the rune floor
                                               (0, -7, 3.4),     # the fountain
                                               (-20, -1, 2.6),   # the lighthouse
                                               (6.5, -2, 1.9)],  # the lift
                                        rects=[(-20.6, -17.1, -5.4, -4.9)])}))  # the wheat

    # The meadow: tall golden wheat that sways in the wind and parts as you
    # walk through.
    E.append(entity('Meadow', transform(-13, -11, 0.02, 1.5, 1.2, 1),
                    MeshRendererComponent=mesh('builtin:plane', [m['rock']], shadow=False),
                    GrassComponent={'material': m['wheat'], 'density': 300.0, 'height': 1.05,
                                    'heightVariation': 0.35, 'width': 0.04, 'stiffness': 0.3,
                                    'recovery': 2.0, 'drawDistance': 45.0, 'paint': {}}))
    sign('Meadow', 0, -6.8, -12.5, 'Grass bends in the wind and where you walk. Sparks hide in it!',
         radius=6.0)

    # The yard: crates to topple and pins to bowl over (3D physics).
    for row, count in enumerate((3, 2, 1)):
        for i in range(count):
            for depth in range(2):
                E.append(instance(crate, transform(11.5 + i * 1.02 + row * 0.51,
                                                   -6.5 + depth * 1.02, row * 1.0)))
    for i, (px, py) in enumerate([(0, 0), (0.6, 0.35), (0.6, -0.35), (1.2, 0.7), (1.2, 0),
                                  (1.2, -0.7)]):
        E.append(instance(pin, transform(18 + px, -14 + py, 0)))
    E.append(entity('Ball', transform(13, -14.2, 0),
                    MeshRendererComponent=mesh('glowfall:ball', [m['rubber']]),
                    SphereCollider3DComponent={'radius': 0.45, 'offset': [0, 0, 0.45],
                                               'trigger': False},
                    RigidBody3DComponent=body(mass=4.0, friction=0.6, bounce=0.3)))
    sign('Physics Yard', 1, 10, -12, 'Shove the crates, roll the ball into the pins!', radius=6.0)

    # The grove: lanterns casting shadows through the trees; fireflies at night.
    for x, y in [(-15, 9), (-10, 14), (-16, 16), (-8.5, 9.5)]:
        E.append(instance(lantern, transform(x, y, 0)))
    for x, y, s in [(-12.5, 11.5, 1.1), (-18, 12, 0.9), (-13, 17.5, 1.25), (-7, 15.5, 0.95),
                    (-19.5, 6.5, 1.05), (-21, -4, 0.8), (19.5, 0.5, 1.0), (-11, -21.5, 0.9)]:
        E.append(instance(tree, transform(x, y, 0, s, s, s, yaw=x * 0.7)))
    E.append(entity('Fireflies', transform(-13, 13, 0),
                    ParticleEmitterComponent={'effect': 'fireflies', 'offset': [0, 0, 0],
                                              'enabled': False}))
    sign('Lantern Grove', 2, -8.5, 8.5,
         'Lanterns cast real shadows. Come back at night (hold T to hurry the day).',
         radius=5.0)

    # Crystal ridge: steps of rock up to glowing crystals that light the ridge.
    for i, (x, y, h) in enumerate([(9, 9, 0.6), (11.5, 11, 1.3), (14, 13.5, 2.0),
                                   (16.5, 16, 2.7)]):
        step = entity('Ridge Step', transform(x, y, 0, 2.6, 2.6, h),
                      MeshRendererComponent=mesh('builtin:cube', [m['rock']]),
                      BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5)))
        if i == 0:
            step['Station'] = {'title': 'Crystal Ridge',
                               'hint': 'Glowing crystals light the rocks around them. Climb!',
                               'radius': 5.0}
        E.append(step)
    for x, y, z, s in [(17.3, 16.6, 2.7, 1.2), (15.8, 17, 2.7, 0.8), (14.5, 14.2, 2.0, 0.7),
                       (12.3, 11.8, 1.3, 0.6)]:
        E.append(entity('Crystal', transform(x, y, z, s, s, s, yaw=x),
                        MeshRendererComponent=mesh('glowfall:crystal', [m['crystal']],
                                                   emits=True, strength=1.4, reach=1.2),
                        Bob=bob(amplitude=0.0, speed=0.0, spin=0.3)))

    # The pavilion: glass walls (see-through) under a cut-out lattice roof, over
    # a floor of glowing runes (a node-graph material).
    E.append(entity('Pavilion Floor', transform(0, 17, 0, 1, 1, 1),
                    MeshRendererComponent=mesh('glowfall:disc', [m['runes']], emits=True,
                                               strength=0.5, reach=0.8),
                    Station={'title': 'Glass Pavilion',
                             'hint': 'See-through glass, a cut-out lattice, runes drawn by a node graph.',
                             'radius': 5.0}))
    for x, y in [(-3, 14), (3, 14), (-3, 20), (3, 20)]:
        E.append(entity('Pillar', transform(x, y, 0, 0.5, 0.5, 3.2),
                        MeshRendererComponent=mesh('glowfall:column', [m['checker']]),
                        BoxCollider3DComponent=box((0.7, 0.7, 1), (0, 0, 0.5))))
    for x, y, sx, sy in [(0, 20, 5.4, 0.08), (-3, 17, 0.08, 5.4), (3, 17, 0.08, 5.4)]:
        E.append(entity('Glass Wall', transform(x, y, 0.3, sx, sy, 2.6),
                        MeshRendererComponent=mesh('builtin:cube', [m['glass']], shadow=False),
                        BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5))))
    E.append(entity('Lattice Roof', transform(0, 17, 3.2, 6.6, 6.6, 0.12),
                    MeshRendererComponent=mesh('builtin:cube', [m['lattice']]),
                    BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5))))

    # The metal garden: one sphere per surface, rough to polished, on plinths.
    for i, name in enumerate(['rubber', 'copper', 'steel', 'gold', 'ceramic']):
        x = -6 + i * 3
        E.append(entity('Plinth', transform(x, 4, 0, 0.9, 0.9, 1.0),
                        MeshRendererComponent=mesh('glowfall:column', [m['checker']]),
                        BoxCollider3DComponent=box((1.3, 1.3, 1), (0, 0, 0.5))))
        E.append(entity(name.capitalize() + ' Sphere', transform(x, 4, 1.0),
                        MeshRendererComponent=mesh('glowfall:orb', [m[name]]),
                        Bob=bob(amplitude=0.0, speed=0.0, spin=0.4)))
    sign('Metal Garden', 1, -2, 1.0, 'Five surfaces, one sun: rough rubber to polished gold.',
         radius=4.0)

    # The fountain: water particles and a shimmering pool.
    E.append(entity('Fountain', transform(0, -7, 0),
                    MeshRendererComponent=mesh('glowfall:fountain', [m['rock']]),
                    ParticleEmitterComponent={'effect': 'fountain', 'offset': [0, 0, 1.4],
                                              'enabled': True}))
    E.append(entity('Pool', transform(0, -7, 0.47, 0.33, 0.33, 1),
                    MeshRendererComponent=mesh('builtin:plane', [m['water']], shadow=False)))
    sign('Fountain', 3, 3.5, -10.5, 'Particles, lit like everything else.', radius=4.5)

    # The lighthouse: a model imported from a glTF file (made by makeArt.py).
    E.append(instance(lighthouse, transform(-20, -1, 0)))
    E.append(entity('Lighthouse Lamp', transform(-20, -1, 6.8),
                    PointLightComponent=light((1.0, 0.85, 0.5), 3.0, 16.0, shadow=False),
                    Station={'title': 'Lighthouse',
                             'hint': 'A model imported from a glTF file.', 'radius': 6.0}))

    # A lift: a kinematic platform riding up and down, to the highest spark.
    E.append(entity('Lift', transform(6.5, -2, 0.3, 2.4, 2.4, 0.35),
                    MeshRendererComponent=mesh('builtin:cube', [m['steel']]),
                    BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5)),
                    RigidBody3DComponent=body(mass=50.0, kinematic=True, friction=1.0),
                    Bob=bob(amplitude=2.2, speed=0.55, spin=0.0),
                    Station={'title': 'The Lift',
                             'hint': 'A kinematic platform: ride it up to the ledge.',
                             'radius': 3.5}))
    E.append(entity('Ledge', transform(9.5, 0.5, 4.2, 2.5, 2.5, 0.35),
                    MeshRendererComponent=mesh('builtin:cube', [m['steel']]),
                    BoxCollider3DComponent=box((1, 1, 1), (0, 0, 0.5))))

    # The sparks: twelve to find (one appears when the pins are down).
    for x, y, z in [(-17, -8, 0.6), (-9.5, -14, 0.6),          # in the meadow
                    (12.5, -6, 3.8),                           # on the crate tower
                    (-16.5, 14.5, 0.9), (-9, 17.5, 0.9),       # in the grove
                    (16.5, 16, 4.0),                           # top of the ridge
                    (0, 17, 1.0),                              # in the pavilion
                    (0, 1.5, 1.2),                             # in the metal garden
                    (0, -7, 2.4),                              # over the fountain
                    (-20, -3.6, 0.9),                          # by the lighthouse
                    (9.5, 0.5, 5.4)]:                          # up on the ledge
        E.append(instance(spark, transform(x, y, z)))

    scene = {
        'formatVersion': 2,
        'systems': ['PhysicsSystem3D', 'BobSystem'],
        'render': {
            'style': {'kind': '3d'},
            'lighting': {'sunDirection': [-0.5, -0.35, 0.8], 'sunColor': [1.0, 0.95, 0.85],
                         'sunStrength': 0.85, 'ambient': 0.45, 'skyColor': [0.45, 0.66, 0.9]},
            'shadows': {'enabled': True, 'distance': 55.0, 'depthBias': 0.0015,
                        'softness': 1.5, 'pointLights': 4, 'pointResolution': 512},
            'wind': {'direction': 35.0, 'strength': 0.55, 'gustiness': 0.6},
            'haze': {'strength': 0.35, 'distance': 110.0},
            'camera': {'target': [0, -14, 1], 'yaw': 1.5707964, 'pitch': 0.42,
                       'distance': 14.0, 'orthographic': False, 'fieldOfView': 0.9,
                       'orthoHeight': 20.0},
        },
        'entities': E,
    }
    write_json('scenes/island.scene', scene)
    write_json('project.sfsproj', {'formatVersion': 1, 'name': 'Glowfall Isle',
                                   'startScene': 'scenes/island.scene'})
    print('Wrote the project in', ROOT)


if __name__ == '__main__':
    main()
