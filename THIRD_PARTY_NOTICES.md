# Third-party notices

This project is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).

**Additional permission under GNU GPL version 3 section 7:** If you modify this Program, or any covered
work, by linking or combining it with the DirectX Shader Compiler (`dxc.exe`, `dxcompiler.dll`,
`dxil.dll`), or a modified version of it, containing parts covered by the terms of its license, the
licensors of this Program grant you additional permission to convey the resulting work.

## DirectX Shader Compiler (Microsoft)

The patch uses `dxc.exe`, `dxcompiler.dll` and `dxil.dll` from the
[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) release
[v1.8.2502](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502)
(`dxc_2025_02_20.zip`). They are **not** included in this repository or in the release package. The
installer and `build.ps1` download that official release from GitHub and copy the three files into place.
They are licensed under the terms in that release's license files.

`converter/dxasm.cpp` (part of this project) loads `dxcompiler.dll` and `dxil.dll` at run time through
their public `DxcCreateInstance` interface. It includes `dxcapi.h` from the Windows SDK.

## Windows SDK / Direct3D 12

The proxy is built against the Windows SDK headers and forwards to the system `d3d12.dll`.
