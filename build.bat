@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
if not exist "%BUILD%" mkdir "%BUILD%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if defined VSPATH (
  if exist "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul
    if errorlevel 1 goto :build_gcc
    goto :build_msvc
  )
)
goto :build_gcc

:build_msvc
echo === Building Coincidence with MSVC ===
pushd "%BUILD%"
rc /nologo /fo resource.res "%ROOT%src\resource.rc"
if errorlevel 1 goto :err
cl /nologo /W4 /O2 /MT /utf-8 ^
   /I "%ROOT%src" ^
   /Fe:Coincidence.exe ^
   "%ROOT%src\main.c" "%ROOT%src\mft.c" "%ROOT%src\search.c" "%ROOT%src\journal.c" ^
   resource.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   user32.lib gdi32.lib comctl32.lib dwmapi.lib shell32.lib advapi32.lib
if errorlevel 1 goto :err
popd
goto :ok

:build_gcc
echo === Building Coincidence with MinGW gcc ===
where gcc >nul 2>&1
if errorlevel 1 (
  echo No C compiler found. Install Visual Studio Build Tools or MinGW.
  exit /b 1
)
pushd "%BUILD%"
windres "%ROOT%src\resource.rc" -O coff -o resource.o
if errorlevel 1 goto :err
gcc -O2 -Wall -municode -mwindows ^
    -I "%ROOT%src" ^
    -o Coincidence.exe ^
    "%ROOT%src\main.c" "%ROOT%src\mft.c" "%ROOT%src\search.c" "%ROOT%src\journal.c" ^
    resource.o ^
    -luser32 -lgdi32 -lcomctl32 -ldwmapi -lshell32 -ladvapi32 -lole32 -luxtheme
if errorlevel 1 goto :err
popd
goto :ok

:err
echo === BUILD FAILED ===
popd 2>nul
exit /b 1

:ok
echo === Built %BUILD%\Coincidence.exe ===
exit /b 0
