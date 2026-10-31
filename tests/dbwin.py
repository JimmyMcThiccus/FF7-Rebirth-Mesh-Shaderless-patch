"""Capture OutputDebugString output (DBWIN protocol) to a file. Usage: dbwin.py OUTFILE SECONDS"""
import ctypes as C
import sys, time
from ctypes import wintypes as W

k = C.WinDLL("kernel32", use_last_error=True)
k.CreateEventW.restype = W.HANDLE
k.CreateFileMappingW.restype = W.HANDLE
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t]
k.CreateFileMappingW.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, W.DWORD, W.DWORD, W.LPCWSTR]

out = open(sys.argv[1], "w", encoding="utf-8", errors="replace")
deadline = time.time() + float(sys.argv[2])
buf_ready = k.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
data_ready = k.CreateEventW(None, False, False, "DBWIN_DATA_READY")
mapping = k.CreateFileMappingW(W.HANDLE(-1), None, 4, 0, 4096, "DBWIN_BUFFER")
view = k.MapViewOfFile(mapping, 4, 0, 0, 4096)
if not (buf_ready and data_ready and mapping and view):
    sys.exit(f"setup failed, err {C.get_last_error()} (another debug-output viewer running?)")
print("listening", flush=True)
while time.time() < deadline:
    k.SetEvent(buf_ready)
    if k.WaitForSingleObject(data_ready, 500) != 0:
        continue
    pid = C.c_uint32.from_address(view).value
    msg = C.string_at(view + 4).decode("mbcs", errors="replace")
    out.write(f"[{pid}] {msg}" + ("" if msg.endswith("\n") else "\n"))
    out.flush()
