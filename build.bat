@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set UEVR_ROOT=C:\Dev\Clean\UEVR

echo === Configuring CMake ===
%CMAKE% -S "%UEVR_ROOT%" -B "%UEVR_ROOT%\build" -G "Visual Studio 18 2026" -A x64

if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configure failed
    exit /b 1
)

echo === Pre-compiling DirectXTK shaders ===
set CompileShadersOutput=%UEVR_ROOT%\build\_deps\directxtk-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "%UEVR_ROOT%\build\_deps\directxtk-src\Src\Shaders"
call .\CompileShaders.cmd >NUL 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: DirectXTK shader compilation failed
    exit /b 1
)

echo === Pre-compiling DirectXTK12 shaders ===
set CompileShadersOutput=%UEVR_ROOT%\build\_deps\directxtk12-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "%UEVR_ROOT%\build\_deps\directxtk12-src\Src\Shaders"
call .\CompileShaders.cmd >NUL 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: DirectXTK12 shader compilation failed
    exit /b 1
)

echo === Building UEVR (Release x64) ===
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "%UEVR_ROOT%\build\uevr-proj.sln" -p:Configuration=Release -m -verbosity:minimal
echo BUILD_EXIT=%ERRORLEVEL%
