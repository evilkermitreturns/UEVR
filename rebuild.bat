@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64

echo === Building UEVR (Release x64) ===
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Dev\Clean\UEVR\build\uevr-proj.sln" -p:Configuration=Release -m -verbosity:minimal
echo BUILD_EXIT=%ERRORLEVEL%
