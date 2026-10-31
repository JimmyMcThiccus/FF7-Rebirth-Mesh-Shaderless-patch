"""bin2h.py <in> <out.h> <name>: embed a binary file as a C byte array."""
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write(f"#pragma once\nstatic const unsigned char {sys.argv[3]}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    f.write("};\n")
