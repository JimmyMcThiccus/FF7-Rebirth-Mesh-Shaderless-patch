"""Log MSI Afterburner monitoring data (framerate, frametime, GPU load...) from its shared memory.

    python -I fpslog.py --list                 show the available sources
    python -I fpslog.py out.csv [seconds]      sample once per second (default: until Ctrl+C)

Afterburner must be running; framerate needs RTSS (Afterburner's on-screen display server).
"""
import ctypes as C
import struct
import sys
import time

FILE_MAP_READ = 4
WANTED = ("Framerate", "Frametime", "GPU usage", "GPU1 usage", "GPU2 usage", "Memory usage", "GPU1 memory usage",
          "GPU temperature", "GPU1 temperature", "CPU usage", "Power")

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]


def read_entries():
    h = k.OpenFileMappingW(FILE_MAP_READ, False, "MAHMSharedMemory")
    if not h:
        sys.exit("MSI Afterburner shared memory not found (is Afterburner running?)")
    view = k.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)
    try:
        sig, ver, hdr, n, esz = struct.unpack_from("<IIIII", C.string_at(view, 20))
        if sig == 0xDEAD:
            return None  # Afterburner is shutting down
        out = {}
        for i in range(n):
            e = C.string_at(view + hdr + i * esz, esz)
            name = e[0:260].split(b"\0")[0].decode("mbcs", "replace")
            units = e[260:520].split(b"\0")[0].decode("mbcs", "replace")
            value = struct.unpack_from("<f", e, 5 * 260)[0]
            gpu = struct.unpack_from("<I", e, 5 * 260 + 16)[0] if esz >= 5 * 260 + 20 else 0
            key = name if name not in out else f"{name} (gpu{gpu})"
            out[key] = (value, units)
        return out
    finally:
        k.UnmapViewOfFile(C.c_void_p(view))
        k.CloseHandle(C.c_void_p(h))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--list":
        for name, (v, u) in read_entries().items():
            print(f"{name:40} {v:10.2f} {u}")
        return
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else float("inf")
    first = read_entries()
    cols = [c for c in first if c.split(" (")[0] in WANTED]
    t0 = time.time()
    with open(out, "w") as f:
        f.write("t," + ",".join(cols) + "\n")
        while time.time() - t0 < seconds:
            e = read_entries()
            if e is None:
                break
            f.write(f"{time.time() - t0:.1f}," + ",".join(f"{e.get(c, (float('nan'),))[0]:.2f}" for c in cols) + "\n")
            f.flush()
            time.sleep(1)


if __name__ == "__main__":
    main()
