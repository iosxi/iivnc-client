@echo off
rem Build iivnc-client.exe with MSVC (VS 2022 Build Tools). /MT links the CRT
rem statically, so the exe needs no runtime install.
rem
rem   build.bat          release build -> iivnc-client.exe
rem   build.bat debug    debug build   -> build\iivnc-client-debug.exe
rem
rem The D3D11 shaders (src\view.hlsl) are compiled here with fxc into
rem build\shader_vs.h / build\shader_ps.h and embedded, so the exe does not
rem need d3dcompiler_47.dll at run time.
rem
rem ASCII only and CRLF on purpose: cmd parses batch files in the OEM code
rem page (932 on Japanese Windows), and goto/labels can misbehave with LF.
setlocal
if defined VCINSTALLDIR goto :build
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSINSTALLER%\vswhere.exe" goto :novs
set "VSPATH="
pushd "%VSINSTALLER%"
for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
popd
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto :novs

:build
cd /d "%~dp0"
if not exist build\obj mkdir build\obj
fxc /nologo /T vs_4_0 /E vs /O3 /Vn g_vsCode /Fh build\shader_vs.h src\view.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps /O3 /Vn g_psCode /Fh build\shader_ps.h src\view.hlsl >nul || exit /b 1
rc /nologo /fo build\obj\iivnc-client.res src\iivnc-client.rc || exit /b 1

set "SRC=src\main.c src\config.c src\conn.c src\decode.c src\jpeg.c src\view.c src\keymap.c src\clip.c src\ui.c src\theme.c src\zdeflate.c src\zinflate.c src\vncdes.c src\fwrules.c"
set "LIBS=user32.lib gdi32.lib shell32.lib comctl32.lib dwmapi.lib uxtheme.lib ole32.lib ws2_32.lib d3d11.lib dxgi.lib dxguid.lib windowscodecs.lib imm32.lib advapi32.lib oleaut32.lib"
set "CFLAGS=/nologo /utf-8 /W4 /wd4201 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /Ibuild"

if /i "%~1"=="debug" goto :debug
cl %CFLAGS% /O2 /Fobuild\obj\ /Feiivnc-client.exe %SRC% build\obj\iivnc-client.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || exit /b 1
for %%F in (iivnc-client.exe) do echo built iivnc-client.exe (%%~zF bytes)
exit /b 0

:debug
cl %CFLAGS% /Od /Zi /Fobuild\obj\ /Fdbuild\ /Febuild\iivnc-client-debug.exe %SRC% build\obj\iivnc-client.res /link /DEBUG /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || exit /b 1
echo built build\iivnc-client-debug.exe
exit /b 0

:novs
echo [error] Visual Studio Build Tools (vcvars64.bat) not found.
exit /b 1
