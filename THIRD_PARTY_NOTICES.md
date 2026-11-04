# Third-party notices

This project is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).

**Additional permission under GNU GPL version 3 section 7:** If you modify this Program, or any covered
work, by linking or combining it with the DirectX Shader Compiler (`dxc.exe`, `dxcompiler.dll`,
`dxil.dll`), or a modified version of it, containing parts covered by the terms of its license, the
licensors of this Program grant you additional permission to convey the resulting work.

## DirectX Shader Compiler (Microsoft)

The release package includes `dxcompiler.dll` (x64), unmodified, from the official
[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) release
[v1.8.2502](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502)
(`dxc_2025_02_20.zip`), in `meshemu\tools`. The DirectX Shader Compiler is open source, built from LLVM and
Clang: it is licensed under the University of Illinois/NCSA Open Source License (`LICENSE-LLVM.txt`) and
the MIT License (`LICENSE-MIT.txt`). Those files and the release's `LICENSE-MS.txt` are included unmodified
in `meshemu\tools\dxc-licenses`. The package does not include `dxil.dll`, Microsoft's separately licensed
validator: since v1.8.2502, `dxcompiler.dll` signs shaders itself, and the result is byte-identical.

`build.ps1` downloads that official release from GitHub (and `dxc.exe` and `dxil.dll` for building and
testing, which are not redistributed). `meshemu-conv.exe` (`conv/src/dxc.rs`) and
`converter/dxasm.cpp`, both part of this project, load `dxcompiler.dll` (and `dxil.dll` if present) at run
time through their public `DxcCreateInstance` interface; `dxasm.cpp` includes `dxcapi.h` from the Windows
SDK.

## Rust crates (meshemu-conv.exe)

`meshemu-conv.exe` statically links the Rust crates `lz4_flex`, `regex`, `regex-automata`, `regex-syntax`,
`aho-corasick` and `memchr`, each used under the MIT License. Their copyright and license notices are in
[conv/THIRD_PARTY_LICENSES.txt](conv/THIRD_PARTY_LICENSES.txt), included in the release package as
`meshemu\tools\meshemu-conv-licenses.txt`.

## DirectMesh (lonewolf0622)

Splitting a mesh draw that overflows the working buffer into an ordered series of draws, rather than
dropping the overflow, follows the overflow handling of the index route in
[DirectMesh](https://github.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-), lonewolf0622's mesh shader
support for AMD BC-250 / RDNA 1 in Mesa's RADV driver (MIT license), used with the author's permission.
Only the idea was used; no code from that project is included.

## Windows SDK / Direct3D 12

The proxy is built against the Windows SDK headers and forwards to the system `d3d12.dll`.
