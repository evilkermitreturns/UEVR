@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64

echo === Pre-compiling DirectXTK shaders ===
set CompileShadersOutput=C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk-src\Src\Shaders"
call .\CompileShaders.cmd >NUL 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: DirectXTK shader compilation failed
    exit /b 1
)

echo === Pre-compiling DirectXTK12 shaders ===
set CompileShadersOutput=C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk12-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk12-src\Src\Shaders"
call .\CompileShaders.cmd >NUL 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: DirectXTK12 shader compilation failed
    exit /b 1
)

echo === Building UEVR (MSVC 14.44, Release x64) ===
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Dev\UEVR-Mod\UEVR\build\uevr-proj.sln" -p:Configuration=Release -m -verbosity:minimal
echo BUILD_EXIT=%ERRORLEVEL%
