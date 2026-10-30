#!/usr/bin/env python3
"""Draws Neon Sprint's pixel art (original, generated) into ../assets.

Run it again after changing a drawing; the PNGs are what the game uses. Pure
Python (zlib PNG writer), no image libraries needed.
"""
import math
import os
import random
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', 'assets')

# The palette: night blues and neon.
CLEAR = (0, 0, 0, 0)
INK = (12, 10, 28, 255)
NAVY = (22, 24, 56, 255)
STEEL = (48, 54, 96, 255)
SLATE = (78, 86, 140, 255)
CYAN = (60, 236, 255, 255)
CYAN_DIM = (30, 140, 190, 255)
MAGENTA = (255, 60, 190, 255)
MAGENTA_DIM = (150, 30, 120, 255)
YELLOW = (255, 226, 80, 255)
ORANGE = (255, 140, 40, 255)
WHITE = (240, 248, 255, 255)
RED = (255, 64, 80, 255)


class Image:
    def __init__(self, width, height, fill=CLEAR):
        self.width = width
        self.height = height
        self.pixels = [list(fill) for _ in range(width * height)]

    def put(self, x, y, color):
        if 0 <= x < self.width and 0 <= y < self.height and color[3] > 0:
            self.pixels[y * self.width + x] = list(color)

    def rect(self, x, y, w, h, color):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.put(i, j, color)

    def save(self, path):
        raw = b''.join(
            b'\x00' + bytes(sum((self.pixels[y * self.width + x] for x in range(self.width)), []))
            for y in range(self.height))

        def chunk(kind, data):
            return (struct.pack('>I', len(data)) + kind + data +
                    struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))

        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as out:
            out.write(b'\x89PNG\r\n\x1a\n')
            out.write(chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 6, 0, 0, 0)))
            out.write(chunk(b'IDAT', zlib.compress(raw, 9)))
            out.write(chunk(b'IEND', b''))


def stamp(sheet, frame, rows):
    """Draws a 16x16 frame from 16 strings (one character a pixel)."""
    keys = {'.': CLEAR, 'k': INK, 'n': NAVY, 's': STEEL, 'l': SLATE, 'c': CYAN, 'd': CYAN_DIM,
            'm': MAGENTA, 'p': MAGENTA_DIM, 'y': YELLOW, 'o': ORANGE, 'w': WHITE, 'r': RED}
    for y, row in enumerate(rows):
        for x, key in enumerate(row):
            sheet.put(frame * 16 + x, y, keys[key])


# --- The runner: 10 frames. 0-1 idle, 2-5 run, 6 jump, 7 fall, 8 dash, 9 wall.
HEAD = [
    '......kkkk......',
    '.....kssssk.....',
    '....kslllllk....',
    '....kccccccck...',
    '....kcwwccwck...',
    '....kssssssk....',
]


def body(legs, arms='....ksmmmmsk....', lean=0):
    torso = ['.....kmmmmk.....', arms, '.....kmmmmk.....', '.....kssssk.....']
    rows = HEAD + torso + legs
    rows = rows + ['................'] * (16 - len(rows))
    if lean:
        rows = [(r[-lean:] + r[:-lean]) if i < 10 else r for i, r in enumerate(rows)]
    return rows


RUNNER = [
    body(['.....ks..sk.....', '.....kl..lk.....', '.....kk..kk.....', '................', '................', '................']),
    body(['.....ks..sk.....', '.....kl..lk.....', '....kkk..kkk....', '................', '................', '................'],
         arms='...kssmmmmssk...'),
    body(['....ks....sk....', '...kl......lk...', '..kk........k...', '................', '................', '................'], lean=1),
    body(['.....ks..sk.....', '.....kl.kl......', '.....kk.kk......', '................', '................', '................'], lean=1),
    body(['...ks......sk...', '..kl........lk..', '..k..........kk.', '................', '................', '................'], lean=1),
    body(['......kssk......', '.....kl.lk......', '.....kk.kk......', '................', '................', '................'], lean=1),
    body(['....kss..ssk....', '...kl......lk...', '................', '................', '................', '................'],
         arms='..kssmmmmmmssk..'),
    body(['.....ks..sk.....', '....kl....lk....', '....kk....kk....', '................', '................', '................'],
         arms='.kss.mmmmmm.ssk.'),
    body(['..kssssssssk....', '..kllllllllk....', '................', '................', '................', '................'],
         arms='kccssmmmmmmssk..', lean=2),
    body(['.....ks..sk.....', '.....kl..lk.....', '.....kk..kk.....', '................', '................', '................'],
         arms='....ksmmmmsskk..'),
]


def make_runner():
    sheet = Image(16 * len(RUNNER), 16)
    for frame, rows in enumerate(RUNNER):
        stamp(sheet, frame, rows)
    sheet.save(os.path.join(ASSETS, 'sprites', 'runner.png'))


# --- Things in the world: 16x16 frames.
#  0 block top, 1 block body, 2 spikes, 3-6 orb spin, 7-8 drone, 9 beacon, 10 jump pad
def make_props():
    sheet = Image(16 * 11, 16)
    # Block top: a lit neon edge over steel.
    sheet.rect(0, 0, 16, 16, NAVY)
    sheet.rect(0, 0, 16, 2, CYAN)
    sheet.rect(0, 2, 16, 1, CYAN_DIM)
    for x in range(1, 16, 4):
        sheet.rect(x, 5, 2, 2, STEEL)
    sheet.rect(0, 15, 16, 1, INK)
    # Block body: riveted panels.
    sheet.rect(16, 0, 16, 16, NAVY)
    sheet.rect(16, 0, 16, 1, STEEL)
    sheet.rect(16, 15, 16, 1, INK)
    for x, y in ((18, 3), (28, 3), (18, 12), (28, 12)):
        sheet.put(x, y, SLATE)
    sheet.rect(23, 6, 2, 4, STEEL)
    # Spikes: four teeth on a strip, white tips.
    for i in range(4):
        left = 32 + i * 4
        for row in range(8):  # row 0 is the tip, at y = 5
            half = row // 4
            for x in range(left + 2 - half - 1, left + 2 + half + 1):
                sheet.put(x, 5 + row, WHITE if row < 2 else RED)
    sheet.rect(32, 13, 16, 3, MAGENTA_DIM)
    # Orb, spinning: a ring seen from turning angles.
    for f, squash in enumerate((1.0, 0.6, 0.2, 0.6)):
        ox = 48 + f * 16
        for y in range(16):
            for x in range(16):
                dx = (x - 7.5) / max(squash, 0.15)
                dy = y - 7.5
                d = math.hypot(dx, dy)
                if d < 5.5:
                    sheet.put(ox + x, y, WHITE if d < 2.5 else YELLOW)
                elif d < 6.5:
                    sheet.put(ox + x, y, ORANGE)
    # Drone, two frames: a hovering eye with flickering rotors.
    for f in range(2):
        ox = 112 + f * 16
        sheet.rect(ox + 3, 5, 10, 7, STEEL)
        sheet.rect(ox + 4, 4, 8, 1, SLATE)
        sheet.rect(ox + 5, 6, 6, 4, INK)
        sheet.rect(ox + 6, 7, 4, 2, MAGENTA if f == 0 else RED)
        sheet.rect(ox + (1 if f == 0 else 3), 2, 4, 1, WHITE)
        sheet.rect(ox + (11 if f == 0 else 9), 2, 4, 1, WHITE)
        sheet.rect(ox + 7, 12, 2, 2, MAGENTA_DIM)
    # Beacon: a lamp post's head.
    sheet.rect(144 + 7, 4, 2, 12, STEEL)
    sheet.rect(144 + 4, 1, 8, 4, YELLOW)
    sheet.rect(144 + 5, 2, 6, 2, WHITE)
    # Jump pad.
    sheet.rect(160, 10, 16, 6, STEEL)
    sheet.rect(160, 9, 16, 1, MAGENTA)
    sheet.rect(162, 11, 12, 2, MAGENTA_DIM)
    sheet.save(os.path.join(ASSETS, 'sprites', 'props.png'))


# --- The glitch wall chasing the runner: 2 frames of 16x64.
def make_glitch():
    rng = random.Random(7)
    sheet = Image(32, 64)
    for f in range(2):
        for y in range(64):
            band = rng.random()
            for x in range(16):
                edge = x / 15.0
                if rng.random() < 0.25 + edge * 0.6:
                    color = MAGENTA if band < 0.5 else (CYAN if band < 0.75 else WHITE)
                    if x < 5 and rng.random() < 0.6:
                        color = MAGENTA_DIM
                    sheet.put(f * 16 + x, y, color)
    sheet.save(os.path.join(ASSETS, 'sprites', 'glitch.png'))


# --- The backdrop: a sky, and two skylines that scroll slower than the world.
def make_backdrop():
    rng = random.Random(3)
    sky = Image(128, 72)
    for y in range(72):
        t = y / 71.0
        color = (int(14 + 30 * t), int(10 + 12 * t), int(40 + 40 * t), 255)
        sky.rect(0, y, 128, 1, color)
    for _ in range(60):
        x, y = rng.randrange(128), rng.randrange(50)
        sky.put(x, y, WHITE if rng.random() < 0.3 else SLATE)
    for y in range(18, 34):  # a big low moon
        for x in range(90, 110):
            if math.hypot(x - 100, y - 26) < 8:
                sky.put(x, y, (255, 120, 200, 255) if y > 26 else (255, 170, 220, 255))
    sky.save(os.path.join(ASSETS, 'backdrop', 'sky.png'))

    def skyline(name, height, base, lit, seed):
        rng = random.Random(seed)
        img = Image(128, height)
        x = 0
        while x < 128:
            w = rng.randrange(8, 20)
            h = rng.randrange(height // 3, height)
            img.rect(x, height - h, w, h, base)
            for wy in range(height - h + 3, height - 2, 4):
                for wx in range(x + 2, x + w - 2, 3):
                    if rng.random() < 0.35:
                        img.put(wx, wy, lit if rng.random() < 0.7 else YELLOW)
            x += w + rng.randrange(0, 3)
        img.save(os.path.join(ASSETS, 'backdrop', name))

    skyline('city_far.png', 48, (28, 26, 66, 255), (90, 70, 160, 255), 11)
    skyline('city_near.png', 64, (18, 16, 40, 255), CYAN_DIM, 23)


if __name__ == '__main__':
    make_runner()
    make_props()
    make_glitch()
    make_backdrop()
    print('Drew the art into', os.path.normpath(ASSETS))
