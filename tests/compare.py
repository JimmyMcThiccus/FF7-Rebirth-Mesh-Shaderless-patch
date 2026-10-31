"""compare.py ref.rgba emu.rgba: per-pixel comparison of two 256x256 RGBA8 images."""
import sys

W = H = 256
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
n = W * H
diff = big = 0
cov_a = cov_b = 0
for i in range(n):
    pa, pb = a[4 * i:4 * i + 4], b[4 * i:4 * i + 4]
    cov_a += pa != b"\0\0\0\xff"
    cov_b += pb != b"\0\0\0\xff"
    if pa != pb:
        diff += 1
        if max(abs(pa[c] - pb[c]) for c in range(3)) > 2:
            big += 1
print(f"coverage ref {cov_a} emu {cov_b}; differing pixels {diff}, by more than 2/255: {big}")
sys.exit(0 if big == 0 else 1)
