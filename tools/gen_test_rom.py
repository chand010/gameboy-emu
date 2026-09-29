#!/usr/bin/env python3
"""
Generate a tiny homebrew Game Boy ROM that exercises the emulator.

The ROM draws a green "GAME BOY / HELLO!" test card with a checkerboard
strip and a few sprites, then sits in a tight loop. It is 100% original
code/data, so it can be committed to the repository and used for
screenshots without any copyright concerns.

usage: python3 tools/gen_test_rom.py [output.gb]
"""

import sys

# ----------------------------------------------------------------------
# A teeny two-pass assembler (just enough for the demo program).
# ----------------------------------------------------------------------

class Asm:
    def __init__(self, base=0x0000):
        self.base = base
        self.code = bytearray()
        self.labels = {}
        self.patches = []   # (offset, label, kind)  kind: 'rel8' | 'abs16'

    def org(self, addr):
        while self.pc() < addr:
            self.code.append(0x00)

    def pc(self):
        return self.base + len(self.code)

    def label(self, name):
        self.labels[name] = self.pc()

    def emit(self, *bs):
        self.code.extend(bs)

    def db(self, *data):
        if len(data) == 1 and isinstance(data[0], (bytes, bytearray)):
            self.code.extend(data[0])
        else:
            self.code.extend(data)

    def _abs16(self, value):
        self.emit(value & 0xFF, (value >> 8) & 0xFF)

    def _ref_abs16(self, name):
        pos = len(self.code)
        self.patches.append((pos, name, 'abs16'))
        self.emit(0x00, 0x00)

    def _ref_rel8(self, name):
        pos = len(self.code)
        self.patches.append((pos, name, 'rel8'))
        self.emit(0x00)

    # --- instructions used by the demo --------------------------------

    def nop(self):          self.emit(0x00)
    def di(self):           self.emit(0xF3)
    def xor_a(self):        self.emit(0xAF)
    def ld_a_b(self):       self.emit(0x78)
    def or_c(self):         self.emit(0xB1)
    def ld_a_hlp(self):     self.emit(0x2A)
    def ld_de_a(self):      self.emit(0x12)
    def inc_de(self):       self.emit(0x13)
    def dec_bc(self):       self.emit(0x0B)

    def ld_sp_nn(self, n):  self.emit(0x31); self._abs16(n)
    def ld_hl_nn(self, n):  self.emit(0x21); self._abs16(n)
    def ld_de_nn(self, n):  self.emit(0x11); self._abs16(n)
    def ld_bc_nn(self, n):  self.emit(0x01); self._abs16(n)
    def ld_a_n(self, n):    self.emit(0x3E, n & 0xFF)
    def cp_n(self, n):      self.emit(0xFE, n & 0xFF)
    def ldh_a_n(self, n):   self.emit(0xF0, n & 0xFF)
    def ldh_n_a(self, n):   self.emit(0xE0, n & 0xFF)

    def ld_hl_ref(self, name): self.emit(0x21); self._ref_abs16(name)

    def jr(self, name):     self.emit(0x18); self._ref_rel8(name)
    def jr_nz(self, name):  self.emit(0x20); self._ref_rel8(name)

    def resolve(self):
        for pos, name, kind in self.patches:
            target = self.labels[name]
            if kind == 'rel8':
                rel = target - (self.base + pos + 1)
                if rel < -128 or rel > 127:
                    raise ValueError(f'relative jump out of range at {pos:#x}')
                self.code[pos] = rel & 0xFF
            else:
                self.code[pos] = target & 0xFF
                self.code[pos + 1] = (target >> 8) & 0xFF


# ----------------------------------------------------------------------
# 5x7 pixel font (bit 4 = leftmost pixel).
# ----------------------------------------------------------------------

FONT = {
    'A': [0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001],
    'B': [0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110],
    'C': [0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110],
    'D': [0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110],
    'E': [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111],
    'F': [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000],
    'G': [0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111],
    'H': [0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001],
    'I': [0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
    'J': [0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100],
    'K': [0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001],
    'L': [0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111],
    'M': [0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001],
    'N': [0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001],
    'O': [0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110],
    'P': [0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000],
    'Q': [0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101],
    'R': [0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001],
    'S': [0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110],
    'T': [0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100],
    'U': [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110],
    'V': [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100],
    'W': [0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001],
    'X': [0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001],
    'Y': [0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100],
    'Z': [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111],
    '0': [0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110],
    '1': [0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
    '2': [0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111],
    '3': [0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110],
    '4': [0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010],
    '5': [0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110],
    '6': [0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110],
    '7': [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000],
    '8': [0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110],
    '9': [0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100],
    '!': [0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100],
    ' ': [0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000],
}


def glyph_tile(ch):
    rows = FONT[ch]
    tile = bytearray()
    for r in rows:
        b = (r << 3) & 0xFF
        tile.append(b)   # bit plane 0
        tile.append(b)   # bit plane 1 (same, so colour 3 = black)
    while len(tile) < 16:
        tile.append(0)
    return bytes(tile)


# ----------------------------------------------------------------------
# Tile set layout
# ----------------------------------------------------------------------

# tile 0: blank, 1: solid black, 2: checker, 3: inverse checker
BLANK   = 0
BLACK   = 1
CHECKER = 2

tiles = bytearray()

tiles.extend(bytes(16))                          # tile 0 blank
tiles.extend(bytes([0xFF] * 16))                 # tile 1 solid
checker = bytearray()
for i in range(8):
    checker.extend([0x55, 0x55] if i % 2 == 0 else [0xAA, 0xAA])
tiles.extend(checker)                            # tile 2
tiles.extend(bytes([0x00] * 16))                 # tile 3 (unused)

glyph_index = {}
order = (list('ABCDEFGHIJKLMNOPQRSTUVWXYZ') +
         list('0123456789') + ['!'])
for ch in order:
    glyph_index[ch] = 4 + len(glyph_index)
    tiles.extend(glyph_tile(ch))


def tile_for(ch):
    if ch == ' ':
        return BLANK
    return glyph_index[ch]


TILE_BYTES = len(tiles)

# ----------------------------------------------------------------------
# Tile map (32x32) and OAM
# ----------------------------------------------------------------------

tilemap = bytearray([BLANK] * (32 * 32))


def put(row, col, tile):
    tilemap[row * 32 + col] = tile


def put_text(row, col, text):
    for i, ch in enumerate(text):
        put(row, col + i, tile_for(ch))


def put_bar(row, tile):
    for c in range(32):
        put(row, c, tile)


# top black border
put_bar(0, BLACK)
put_bar(1, BLACK)

# title
put_text(4, 6, 'GAME BOY')

# checker strip
for c in range(32):
    put(8, c, CHECKER)
    put(9, c, CHECKER)

# subtitle
put_text(12, 7, 'HELLO!')

# digits strip as a little extra
put_text(15, 5, '1 2 3 4')

# bottom black border
put_bar(17, BLACK)

# sprites: (screen_x, screen_y, tile) -> stored with the +16/+8 offsets
SPRITES = [
    (20, 100, BLACK),       # solid block
    (40, 110, CHECKER),     # dither block
    (80, 40, tile_for('G')),# letter
    (130, 120, BLACK),      # solid block
]

oam = bytearray(160)
for i, (sx, sy, tile) in enumerate(SPRITES):
    off = i * 4
    oam[off + 0] = sy + 16
    oam[off + 1] = sx + 8
    oam[off + 2] = tile
    oam[off + 3] = 0

# ----------------------------------------------------------------------
# Program
# ----------------------------------------------------------------------

a = Asm(base=0x0000)
a.org(0x100)

a.nop()
a.emit(0xC3)            # JP to start
a._ref_abs16('start')

# --- header ------------------------------------------------------------
a.org(0x104)
LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
])
assert len(LOGO) == 48
a.db(LOGO)

title = bytearray(16)
name = b'GBEMU TEST'
title[:len(name)] = name
title[15] = 0x80          # CGB flag
a.db(bytes(title))

a.db(0x00, 0x00)          # new licensee code
a.db(0x00)                # SGB flag
a.db(0x00)                # cartridge type: ROM only
a.db(0x00)                # ROM size: 32KB
a.db(0x00)                # RAM size: none
a.db(0x01)                # destination: non-Japanese
a.db(0x33)                # old licensee
a.db(0x00)                # mask ROM version

# header checksum over 0x134..0x14C
chk = 0
for b in a.code[0x134:0x14D]:
    chk = (chk - b - 1) & 0xFF
a.db(chk)

# global checksum placeholder (patched after the ROM is assembled)
a.db(0x00, 0x00)

# --- actual program -----------------------------------------------------
a.org(0x150)
a.label('start')

a.di()
a.ld_sp_nn(0xFFFE)

a.label('wait_vblank')
a.ldh_a_n(0x44)            # LY
a.cp_n(0x90)               # 144
a.jr_nz('wait_vblank')

a.ld_a_n(0x00)
a.ldh_n_a(0x40)            # LCD off
a.ld_a_n(0xE4)
a.ldh_n_a(0x47)            # BGP
a.ld_a_n(0xE4)
a.ldh_n_a(0x48)            # OBP0
a.ld_a_n(0xE4)
a.ldh_n_a(0x49)            # OBP1

# copy tiles
a.ld_hl_ref('tiles')
a.ld_de_nn(0x8000)
a.ld_bc_nn(TILE_BYTES)
a.label('copy_tiles')
a.ld_a_hlp()
a.ld_de_a()
a.inc_de()
a.dec_bc()
a.ld_a_b()
a.or_c()
a.jr_nz('copy_tiles')

# copy tile map
a.ld_hl_ref('tilemap')
a.ld_de_nn(0x9800)
a.ld_bc_nn(0x0400)
a.label('copy_map')
a.ld_a_hlp()
a.ld_de_a()
a.inc_de()
a.dec_bc()
a.ld_a_b()
a.or_c()
a.jr_nz('copy_map')

# copy OAM
a.ld_hl_ref('oam')
a.ld_de_nn(0xFE00)
a.ld_bc_nn(0x00A0)
a.label('copy_oam')
a.ld_a_hlp()
a.ld_de_a()
a.inc_de()
a.dec_bc()
a.ld_a_b()
a.or_c()
a.jr_nz('copy_oam')

# clear scroll
a.xor_a()
a.ldh_n_a(0x42)            # SCY
a.ldh_n_a(0x43)            # SCX

# LCD on + BG + OBJ
a.ld_a_n(0x93)
a.ldh_n_a(0x40)

a.label('forever')
a.jr('forever')

# --- data section -------------------------------------------------------
a.label('tiles')
a.db(tiles)

a.label('tilemap')
a.db(tilemap)

a.label('oam')
a.db(oam)

# resolve labels, pad to 32KB and fix the global checksum
a.resolve()

rom = bytearray(a.code)
rom.extend(bytes(0x8000 - len(rom)))

total = sum(rom) & 0xFFFF
rom[0x14E] = (total >> 8) & 0xFF
rom[0x14F] = total & 0xFF

out = sys.argv[1] if len(sys.argv) > 1 else 'roms/test.gb'
with open(out, 'wb') as f:
    f.write(rom)

print(f'wrote {out} ({len(rom)} bytes, {TILE_BYTES} bytes of tiles)')
