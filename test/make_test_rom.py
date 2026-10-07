# Builds a tiny LoROM test cart: Mode 1, BG2 checker (far), BG1 ground + pillars (playfield),
# BG3 high-priority HUD bar, and a 16x16 sprite. Used to verify the per-layer stereo warp.
import struct, sys
ROM = bytearray(b'\xff' * 0x20000)
code = bytearray()
data = {}
DATA_BASE = 0x1000  # file offset of data blobs (bank 0, $9000)
blobs = bytearray()
def blob(name, b):
    data[name] = (0x8000 + DATA_BASE + len(blobs), len(b)); blobs.extend(b)
def lda(v): code.extend([0xA9, v])
def sta(a): code.extend([0x8D, a & 0xff, a >> 8])
def ldx(v): code.extend([0xA2, v & 0xff, v >> 8])
def stx(a): code.extend([0x8E, a & 0xff, a >> 8])
def w(reg, v): lda(v); sta(reg)
def dma(dest, mode, name):
    src, size = data[name]
    w(0x4300, mode); w(0x4301, dest); ldx(src); stx(0x4302); w(0x4304, 0); ldx(size); stx(0x4305); w(0x420B, 1)
def vram(addr): w(0x2115, 0x80); ldx(addr); stx(0x2116)

def solid4(c):  # 4bpp 8x8 tile of colour c
    planes = [0xff if (c >> p) & 1 else 0 for p in range(4)]
    return bytes([planes[0], planes[1]] * 8 + [planes[2], planes[3]] * 8)
blob('chr', bytes(32) + solid4(1) + solid4(2))
blob('chr3', bytes(16) + bytes([0xff, 0xff] * 8))  # 2bpp tile 1 = colour 3
def tm(fn):
    return b''.join(struct.pack('<H', fn(x, y)) for y in range(32) for x in range(32))
blob('bg2', tm(lambda x, y: 1 | ((2 if ((x // 2 + y // 2) & 1) else 3) << 10)))
def bg1(x, y):
    if y >= 22: return 1 | (1 << 10)
    if y >= 12 and x in (6, 7, 22, 23): return 2 | (1 << 10)
    return 0
blob('bg1', tm(bg1))
blob('bg3', tm(lambda x, y: (1 | 0x2000) if (y in (1, 2) and 2 <= x < 30) else 0))
def rgb(r, g, b): return struct.pack('<H', r | (g << 5) | (b << 10))
pal = bytearray(512)
def setc(i, c): pal[i*2:i*2+2] = c
setc(0, rgb(4, 8, 20)); setc(3, rgb(31, 31, 31))
setc(17, rgb(20, 12, 4)); setc(18, rgb(28, 22, 8))
setc(33, rgb(4, 16, 6)); setc(49, rgb(2, 10, 4))
setc(129, rgb(31, 4, 4))
blob('pal', bytes(pal))
oam = bytearray()
for i in range(128):
    if i < 4: oam += bytes([120 + (i & 1) * 8, 140 + (i >> 1) * 8, 0, 0x20])
    else: oam += bytes([0, 240, 0, 0])
oam += bytes(32)
blob('oam', bytes(oam))
blob('objchr', solid4(1))

code.extend([0x78, 0x18, 0xFB, 0xC2, 0x10, 0xE2, 0x20])  # sei clc xce rep#$10 sep#$20
w(0x2100, 0x80)
w(0x2105, 0x09)            # mode 1, BG3 priority
w(0x2107, 0x04); w(0x2108, 0x08); w(0x2109, 0x0C)
w(0x210B, 0x00); w(0x210C, 0x01)
w(0x2101, 0x02)            # OBJ chr at $4000 words
for r in (0x210D, 0x210E, 0x210F, 0x2110, 0x2111, 0x2112): w(r, 0); w(r, 0)
vram(0x0000); dma(0x18, 1, 'chr')
vram(0x1000); dma(0x18, 1, 'chr3')
vram(0x0400); dma(0x18, 1, 'bg1')
vram(0x0800); dma(0x18, 1, 'bg2')
vram(0x0C00); dma(0x18, 1, 'bg3')
vram(0x4000); dma(0x18, 1, 'objchr')
w(0x2121, 0); dma(0x22, 0, 'pal')
w(0x2102, 0); w(0x2103, 0); dma(0x04, 0, 'oam')
w(0x212C, 0x17)
w(0x2100, 0x0F)
loop = 0x8000 + len(code); code.extend([0x4C, loop & 0xff, loop >> 8])
rti = 0x8000 + len(code); code.append(0x40)
assert len(code) < DATA_BASE
ROM[0:len(code)] = code
ROM[DATA_BASE:DATA_BASE + len(blobs)] = blobs
h = 0x7FC0
ROM[h:h+21] = b'POPUP16 LAYER TEST   '
ROM[h+0x15] = 0x20; ROM[h+0x16] = 0; ROM[h+0x17] = 0x07; ROM[h+0x18] = 0; ROM[h+0x19] = 1; ROM[h+0x1A] = 0; ROM[h+0x1B] = 0
for v in range(0x7FE4, 0x8000, 2): ROM[v:v+2] = struct.pack('<H', rti)
ROM[0x7FFC:0x7FFE] = struct.pack('<H', 0x8000)
ROM[0x7FDC:0x7FE0] = b'\x00\x00\x00\x00'
s = sum(ROM) & 0xffff
ROM[0x7FDC:0x7FE0] = struct.pack('<HH', s ^ 0xffff, s)
open(sys.argv[1], 'wb').write(ROM)
