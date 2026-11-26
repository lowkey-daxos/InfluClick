@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title InfluClick - build

rem ====================================================================
rem  InfluClick build script (Windows 10/11)
rem
rem  Usage:  build.bat              build and keep the window open
rem          build.bat nopause      build without the final "pause" (CI, scripts)
rem          build.bat clean        remove build\obj and build\Release
rem          build.bat clean all    also remove downloaded ImGui and MinGW
rem
rem  Needs NOTHING pre-installed. It looks for a compiler in this order:
rem     1) cl.exe   (Visual Studio / Build Tools, found automatically)
rem     2) g++.exe  (MinGW-w64 already in PATH)
rem     3) portable MinGW-w64, downloaded once into tools\  (~250 MB)
rem  Dear ImGui is downloaded once into third_party\imgui.
rem ====================================================================

set "NAME=InfluClick"
set "OUTDIR=build\Release"
set "EXE=%OUTDIR%\%NAME%.exe"
set "IMVER=1.91.9"
set "NOPAUSE=0"
if /i "%~1"=="nopause" set "NOPAUSE=1"

if /i "%~1"=="clean" goto :clean

echo ============================================
echo   %NAME% - build (ImGui %IMVER% + DirectX 11)
echo ============================================

rem ---------- 1) folders + sources ----------
if not exist "src\main.cpp" goto :no_src
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "build\obj" mkdir "build\obj"

set "SRCS="
for %%f in (src\*.cpp) do call set "SRCS=%%SRCS%% %%f"

rem optional icon: src\app.ico (build works without it)
set "RCDEF="
if exist "src\app.ico" (set "RCDEF=-DHAS_ICON") else (echo Note: src\app.ico not found - building without an icon.)

rem ---------- 2) Dear ImGui ----------
set "IMGUI="
if exist "third_party\imgui\imgui.h" if exist "third_party\imgui\backends\imgui_impl_dx11.cpp" set "IMGUI=%CD%\third_party\imgui"
if defined IMGUI goto :imgui_ok
call :get_imgui
if errorlevel 1 goto :fail
set "IMGUI=%CD%\third_party\imgui"
:imgui_ok
echo ImGui: %IMGUI%

set IMSRC="%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_dx11.cpp"

rem ---------- 3) compiler: cl -> Visual Studio (vswhere) -> g++ -> download MinGW ----------
where cl >nul 2>nul
if not errorlevel 1 goto :use_msvc

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_gcc
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :try_gcc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :try_gcc
where cl >nul 2>nul
if errorlevel 1 goto :try_gcc
goto :use_msvc

:try_gcc
if exist "tools\mingw64\bin\g++.exe" goto :local_gcc_ok
where g++ >nul 2>nul
if not errorlevel 1 goto :use_gcc
call :get_mingw
if errorlevel 1 goto :fail
:local_gcc_ok
set "PATH=%CD%\tools\mingw64\bin;%PATH%"
goto :use_gcc

rem ---------- 4) compile ----------
:use_msvc
echo Compiler: cl (MSVC)
set "RES="
rc /nologo %RCDEF% /I src /fo build\obj\res.res src\app.rc
if errorlevel 1 goto :fail
set "RES=build\obj\res.res"
rem /MT = static runtime: the exe runs on a clean Windows 10/11 without the VC++ Redistributable
rem /MANIFEST:NO = manifest is already embedded through app.rc
cl /nologo /std:c++20 /EHsc /O2 /MT /MP /utf-8 /DNOMINMAX /I"%IMGUI%" /I"%IMGUI%\backends" /Fo:build\obj\ /Fe:%EXE% %SRCS% %IMSRC% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO %RES%
if errorlevel 1 goto :fail
goto :ok

:use_gcc
echo Compiler: g++ (MinGW-w64)
set "RES="
windres %RCDEF% -I src src\app.rc -O coff -o build\obj\res.o
if errorlevel 1 goto :fail
set "RES=build\obj\res.o"
g++ %SRCS% %IMSRC% %RES% -o "%EXE%" -std=c++20 -O2 -s -mwindows -static -DNOMINMAX -I"%IMGUI%" -I"%IMGUI%\backends" -ld3d11 -ldxgi -ldxguid -ld3dcompiler_47 -ldwmapi -lwinmm -lgdi32 -luser32 -limm32
if errorlevel 1 goto :fail
goto :ok

:ok
copy /y "%EXE%" "%~dp0%NAME%.exe" >nul
if errorlevel 1 goto :copy_fail
echo.
echo ============================================
echo   DONE:  %~dp0%NAME%.exe
echo ============================================
if "%NOPAUSE%"=="0" pause
exit /b 0

:copy_fail
echo.
echo Build succeeded, but %NAME%.exe in the project folder could not be replaced.
echo Close the running %NAME%.exe and run build.bat again. The new exe is: %EXE%
goto :fail_nomsg

rem ====================================================================
:get_imgui
echo Downloading Dear ImGui %IMVER% from GitHub...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $z=Join-Path $env:TEMP 'imgui_dl.zip'; Invoke-WebRequest -UseBasicParsing 'https://github.com/ocornut/imgui/archive/refs/tags/v%IMVER%.zip' -OutFile $z; New-Item -ItemType Directory -Force 'third_party' | Out-Null; if (Test-Path 'third_party\imgui') { Remove-Item -Recurse -Force 'third_party\imgui' }; Expand-Archive -LiteralPath $z -DestinationPath 'third_party' -Force; Rename-Item 'third_party\imgui-%IMVER%' 'imgui'; Remove-Item $z"
if errorlevel 1 goto :imgui_fail
if not exist "third_party\imgui\imgui.h" goto :imgui_fail
exit /b 0
:imgui_fail
echo [ERROR] Could not download ImGui. Check your internet connection, or download
echo         https://github.com/ocornut/imgui/releases/tag/v%IMVER%
echo         and extract it so that third_party\imgui\imgui.h exists.
exit /b 1

rem ====================================================================
:get_mingw
echo.
echo No C++ compiler found (Visual Studio or MinGW-w64).
echo A portable MinGW-w64 (about 250 MB) can be downloaded into the "tools" folder.
echo It is a one-time download and nothing is installed system-wide.
if "%NOPAUSE%"=="1" goto :mingw_download
choice /c YN /n /m "Download it now? [Y/N] "
if errorlevel 2 exit /b 1
:mingw_download
if not exist "tools" mkdir "tools"
echo Downloading MinGW-w64 (winlibs)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $r=Invoke-RestMethod -UseBasicParsing -Headers @{'User-Agent'='build-bat'} 'https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest'; $a=$r.assets | Where-Object { $_.name -match '^winlibs-x86_64-posix-seh-gcc-[0-9.]+-mingw-w64ucrt-[0-9.]+-r[0-9]+\.zip$' } | Select-Object -First 1; if (-not $a) { throw 'MinGW asset not found' }; Invoke-WebRequest -UseBasicParsing $a.browser_download_url -OutFile (Join-Path $env:TEMP 'build_mingw.zip')"
if errorlevel 1 goto :mingw_fail
echo Extracting (this takes a while)...
tar -xf "%TEMP%\build_mingw.zip" -C "tools" >nul 2>nul
if errorlevel 1 powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath (Join-Path $env:TEMP 'build_mingw.zip') -DestinationPath 'tools' -Force"
del "%TEMP%\build_mingw.zip" >nul 2>nul
if not exist "tools\mingw64\bin\g++.exe" goto :mingw_fail
exit /b 0
:mingw_fail
echo [ERROR] MinGW-w64 download or extraction failed. Install Visual Studio Build Tools
echo         (C++ workload) or MinGW-w64 manually, or check your internet connection.
exit /b 1

rem ====================================================================
:clean
if exist "build\obj" rmdir /s /q "build\obj"
if exist "build\Release" rmdir /s /q "build\Release"
echo Cleaned build\obj and build\Release.
if /i not "%~2"=="all" exit /b 0
if exist "build" rmdir /s /q "build"
if exist "third_party" rmdir /s /q "third_party"
if exist "tools" rmdir /s /q "tools"
echo Also removed build, third_party and tools.
exit /b 0

:no_src
echo [ERROR] src\main.cpp not found. Run build.bat from the project root folder.
goto :fail

:fail
echo.
echo *** BUILD FAILED ***
echo (If InfluClick.exe is currently running, close it and try again.)
:fail_nomsg
if "%NOPAUSE%"=="0" pause
exit /b 1
