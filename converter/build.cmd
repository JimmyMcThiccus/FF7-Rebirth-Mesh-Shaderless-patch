@echo off
rem Builds dxasm.exe (assembles + validates DXIL text with dxcompiler.dll / dxil.dll from next to it).
setlocal
pushd "%~dp0"
call ..\vc.cmd cl /nologo /O2 /EHsc /std:c++17 /W3 dxasm.cpp /Fe:dxasm.exe || goto fail
popd
echo dxasm build ok
exit /b 0
:fail
popd
exit /b 1
