# Counts pixels brighter than the viewport's clear colour in a screenshot.
#
# The argument is the file --screenshot wrote (a BMP, whatever the extension says); the exit
# code says whether anything was drawn. Exists for pingu-viewport-check.bat: every byte
# comparison there would pass on a model that loads and draws nothing, and this cannot.
import struct
import sys

data = open(sys.argv[1], 'rb').read()
if data[:2] != b'BM':
    print('not a BMP')
    sys.exit(1)
offset = struct.unpack_from('<I', data, 10)[0]
w = struct.unpack_from('<i', data, 18)[0]
h = abs(struct.unpack_from('<i', data, 22)[0])
bpp = struct.unpack_from('<H', data, 28)[0] // 8
row = (w * bpp + 3) & ~3
lit = 0
for y in range(h):
    base = offset + y * row
    for x in range(w):
        i = base + x * bpp
        if data[i] > 40 or data[i + 1] > 40 or data[i + 2] > 40:
            lit += 1
print(f'{lit} of {w * h} pixels lit')
sys.exit(0 if lit > (w * h) // 100 else 1)
