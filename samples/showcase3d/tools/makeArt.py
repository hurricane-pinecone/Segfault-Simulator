#!/usr/bin/env python3
"""Draws Glowfall Isle's art: its textures and its one glTF model.

Everything here is original, made by this script (no third-party assets). It
needs only the standard library: a tiny PNG writer, and a glTF writer with the
buffer embedded as base64.
"""
import base64
import json
import math
import os
import random
import struct
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def save_png(path, width, height, pixels):
    """pixels: a list of rows, each a list of (r, g, b, a) 0-255."""
    raw = b''.join(b'\x00' + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n')
        out.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)))
        out.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        out.write(chunk(b'IEND', b''))


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def crate():
    """A wooden crate: planks with dark seams and a cross brace."""
    size = 64
    rnd = random.Random(7)
    light, dark, seam = (196, 138, 78, 255), (150, 98, 52, 255), (78, 48, 26, 255)
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            grain = 0.5 + 0.5 * math.sin(y * 0.9 + math.sin(x * 0.21) * 2.0)
            colour = lerp(dark, light, grain * 0.7 + rnd.random() * 0.3)
            edge = x < 5 or x > 58 or y < 5 or y > 58
            brace = abs(x - y) < 4 or abs(x + y - 63) < 4
            if y % 16 == 0 and not edge:
                colour = seam
            if edge or brace:
                colour = lerp(colour, (120, 76, 40, 255), 0.6)
            if (x in (2, 61) and y in (2, 61)):
                colour = (210, 210, 220, 255)  # nails
            row.append(colour)
        rows.append(row)
    save_png(os.path.join(ROOT, 'assets/textures/crate.png'), size, size, rows)


def lattice():
    """A cut-out lattice: diamonds of stone with holes between (alpha 0)."""
    size = 64
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            u, v = (x + 0.5) / size, (y + 0.5) / size
            d = abs((u * 4) % 1 - 0.5) + abs((v * 4) % 1 - 0.5)
            if d > 0.38:
                shade = 200 + int(30 * math.sin(x * 0.7) * math.cos(y * 0.5))
                row.append((shade, shade - 10, shade - 30, 255))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    save_png(os.path.join(ROOT, 'assets/textures/lattice.png'), size, size, rows)


def signs():
    """Four signposts in one sheet (32 x 32 each): a board on a post with an
    icon -- a blade of grass, a box, a flame, a droplet."""
    cell, count = 32, 4
    width, height = cell * count, cell
    rows = [[(0, 0, 0, 0)] * width for _ in range(height)]

    def put(cx, x, y, colour):
        if 0 <= x < cell and 0 <= y < cell:
            rows[y][cx * cell + x] = colour

    wood, board, ink = (110, 70, 40, 255), (232, 205, 150, 255), (40, 30, 24, 255)
    icons = [(70, 190, 80, 255), (190, 130, 70, 255), (255, 150, 40, 255), (70, 160, 255, 255)]
    for i in range(count):
        for y in range(18, 32):
            for x in (15, 16):
                put(i, x, y, wood)
        for y in range(3, 19):
            for x in range(4, 28):
                edge = y in (3, 18) or x in (4, 27)
                put(i, x, y, ink if edge else board)
        colour = icons[i]
        for y in range(6, 16):
            for x in range(10, 22):
                cx, cy = x - 15.5, y - 10.5
                inside = {
                    0: abs(cx) < 1.5 + (15 - y) * 0.05 and y > 6,
                    1: abs(cx) < 4 and abs(cy) < 4,
                    2: cx * cx + (cy - 1) ** 2 < 14 - (y - 6) * 0.6,
                    3: cx * cx + (cy - 1.5) ** 2 < 12 or (abs(cx) < (y - 6) * 0.7 and y < 10),
                }[i]
                if inside:
                    put(i, x, y, colour)
    save_png(os.path.join(ROOT, 'assets/textures/signs.png'), width, height, rows)


def lighthouse():
    """A little striped lighthouse as a glTF model: tower, gallery and lamp
    room, each its own mesh primitive and material."""
    positions, normals, indices = [], [], []
    primitives = []

    def ring_mesh(r0, r1, z0, z1, segments=20):
        start = len(positions)
        first_index = len(indices)
        for i in range(segments + 1):
            a = 2 * math.pi * i / segments
            c, s = math.cos(a), math.sin(a)
            slope = (r0 - r1) / max(z1 - z0, 1e-3)
            n = (c, s, slope)
            length = math.sqrt(sum(v * v for v in n))
            n = tuple(v / length for v in n)
            positions.extend([(r0 * c, r0 * s, z0), (r1 * c, r1 * s, z1)])
            normals.extend([n, n])
        for i in range(segments):
            a, b = start + 2 * i, start + 2 * i + 2
            indices.extend([a, b, a + 1, a + 1, b, b + 1])
        # A cap on top.
        centre = len(positions)
        positions.append((0.0, 0.0, z1))
        normals.append((0.0, 0.0, 1.0))
        rim = len(positions)
        for i in range(segments + 1):
            a = 2 * math.pi * i / segments
            positions.append((r1 * math.cos(a), r1 * math.sin(a), z1))
            normals.append((0.0, 0.0, 1.0))
        for i in range(segments):
            indices.extend([centre, rim + i, rim + i + 1])
        return first_index, len(indices) - first_index

    # glTF is Y-up; the engine is Z-up and turns models on import. Build Y-up:
    # (x, z, -y) of the Z-up shape.
    parts = []
    stripes = 4
    height = 6.0
    for i in range(stripes):
        z0, z1 = height * i / stripes, height * (i + 1) / stripes
        r0, r1 = 1.4 - 0.5 * z0 / height, 1.4 - 0.5 * z1 / height
        parts.append((ring_mesh(r0, r1, z0, z1), 0 if i % 2 == 0 else 1))
    parts.append((ring_mesh(1.25, 1.25, height, height + 0.25), 2))   # gallery
    parts.append((ring_mesh(0.7, 0.7, height + 0.25, height + 1.3), 3))  # lamp room
    parts.append((ring_mesh(0.85, 0.05, height + 1.3, height + 2.0), 2))  # roof

    y_up = [(x, z, -y) for x, y, z in positions]
    n_up = [(x, z, -y) for x, y, z in normals]
    pos_bytes = b''.join(struct.pack('<3f', *p) for p in y_up)
    nor_bytes = b''.join(struct.pack('<3f', *n) for n in n_up)
    idx_bytes = b''.join(struct.pack('<I', i) for i in indices)
    buffer = pos_bytes + nor_bytes + idx_bytes
    lo = [min(p[k] for p in y_up) for k in range(3)]
    hi = [max(p[k] for p in y_up) for k in range(3)]

    accessors = [
        {'bufferView': 0, 'componentType': 5126, 'count': len(y_up), 'type': 'VEC3',
         'min': lo, 'max': hi},
        {'bufferView': 1, 'componentType': 5126, 'count': len(n_up), 'type': 'VEC3'},
    ]
    for (first, count), material in parts:
        accessors.append({'bufferView': 2, 'byteOffset': first * 4, 'componentType': 5125,
                          'count': count, 'type': 'SCALAR'})
        primitives.append({'attributes': {'POSITION': 0, 'NORMAL': 1},
                           'indices': len(accessors) - 1, 'material': material})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'Glowfall Isle makeArt.py'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'Lighthouse', 'mesh': 0}],
        'meshes': [{'name': 'Lighthouse', 'primitives': primitives}],
        'materials': [
            {'name': 'White', 'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.9, 0.86, 1],
                                                      'metallicFactor': 0, 'roughnessFactor': 0.6}},
            {'name': 'Red', 'pbrMetallicRoughness': {'baseColorFactor': [0.75, 0.12, 0.1, 1],
                                                    'metallicFactor': 0, 'roughnessFactor': 0.5}},
            {'name': 'Iron', 'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.22, 0.25, 1],
                                                     'metallicFactor': 0.9, 'roughnessFactor': 0.4}},
            {'name': 'Lamp', 'emissiveFactor': [1.0, 0.85, 0.45],
             'pbrMetallicRoughness': {'baseColorFactor': [1, 0.95, 0.8, 1],
                                      'metallicFactor': 0, 'roughnessFactor': 0.2}},
        ],
        'buffers': [{'byteLength': len(buffer),
                     'uri': 'data:application/octet-stream;base64,' +
                            base64.b64encode(buffer).decode()}],
        'bufferViews': [
            {'buffer': 0, 'byteOffset': 0, 'byteLength': len(pos_bytes), 'target': 34962},
            {'buffer': 0, 'byteOffset': len(pos_bytes), 'byteLength': len(nor_bytes),
             'target': 34962},
            {'buffer': 0, 'byteOffset': len(pos_bytes) + len(nor_bytes),
             'byteLength': len(idx_bytes), 'target': 34963},
        ],
        'accessors': accessors,
    }
    path = os.path.join(ROOT, 'assets/models/lighthouse.gltf')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as out:
        json.dump(gltf, out, indent=1)
        out.write('\n')


def main():
    crate()
    lattice()
    signs()
    lighthouse()
    print('Drew the art in', os.path.join(ROOT, 'assets'))


if __name__ == '__main__':
    main()
