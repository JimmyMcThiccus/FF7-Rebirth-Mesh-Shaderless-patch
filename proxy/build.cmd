@echo off
rem Builds the d3d12.dll proxy. DXC_DIR: folder with dxc.exe (default ..\third_party\dxc; build.ps1 fills it).
setlocal
pushd "%~dp0"
if not defined DXC_DIR set "DXC_DIR=%~dp0..\third_party\dxc"
"%DXC_DIR%\dxc.exe" -nologo -T vs_6_0 -E main nullvs.hlsl -Fo nullvs.dxil || goto fail
python -I bin2h.py nullvs.dxil nullvs.h kNullVs || goto fail
call ..\vc.cmd ml64 /nologo /c /Fo exports.obj exports.asm || goto fail
call ..\vc.cmd cl /nologo /LD /O2 /Zi /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS proxy.cpp exports.obj ^
    /Fe:d3d12.dll /link /DEF:d3d12.def /DEBUG /OPT:REF /OPT:ICF || goto fail
popd
echo proxy build ok
exit /b 0
:fail
popd
exit /b 1
