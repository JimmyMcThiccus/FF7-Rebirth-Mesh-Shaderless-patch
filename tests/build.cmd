@echo off
rem Builds the test harness and its shaders. Needs dxc.exe in ..\converter (build.ps1 puts it there).
setlocal
pushd "%~dp0"
set "DXC=%~dp0..\converter\dxc.exe"
"%DXC%" -nologo -T ms_6_6 -E main ms_simple.hlsl -Fo ms_simple.dxil || goto fail
"%DXC%" -nologo -T ms_6_6 -E main ms_game.hlsl -Fo ms_game.dxil || goto fail
"%DXC%" -nologo -T ps_6_6 -E main ps_simple.hlsl -Fo ps_simple.dxil || goto fail
"%DXC%" -nologo -T ps_6_6 -E main ps_wide.hlsl -Fo ps_wide.dxil || goto fail
python -I ..\proxy\bin2h.py ms_simple.dxil ms_simple.h kMs || goto fail
python -I ..\proxy\bin2h.py ms_game.dxil ms_game.h kMsGame || goto fail
python -I ..\proxy\bin2h.py ps_simple.dxil ps_simple.h kPs || goto fail
python -I ..\proxy\bin2h.py ps_wide.dxil ps_wide.h kPsWide || goto fail
call ..\vc.cmd cl /nologo /O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS harness.cpp /Fe:harness.exe d3d12.lib dxgi.lib || goto fail
popd
echo harness build ok
exit /b 0
:fail
popd
exit /b 1
