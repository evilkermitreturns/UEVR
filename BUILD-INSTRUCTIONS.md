# UEVR Build Instructions

## Prerequisites

- **Visual Studio 2026** (v18) with C++ Desktop workload
- **MSVC Toolset 14.44.35207** — pinned to match release builds. VS auto-updates may install newer toolsets (14.50+) that produce broken binaries. Always pin.
- **Windows SDK 10.0.26100.0**

## Build Steps

### 1. CMake Configure (one-time, or after CMakeLists.txt changes)

```bash
cd C:\Dev\UEVR-Mod\UEVR
rm -rf build
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" ..
```

Key flag: `-T "v143,version=14.44"` pins the MSVC toolset. Without this, cmake picks the newest installed toolset which may produce broken DLLs.

### 2. Pre-compile Shaders (required after cmake configure)

There is a cmake bug on Windows where `cmake -E env CompileShaders.cmd` can't find the .cmd in the current working directory. Workaround: pre-compile shaders manually before MSBuild.

Use the `build.bat` script in the repo root, or manually:

```batch
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64

set CompileShadersOutput=C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk-src\Src\Shaders"
call .\CompileShaders.cmd

set CompileShadersOutput=C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk12-build\Shaders\Compiled
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"
cd /d "C:\Dev\UEVR-Mod\UEVR\build\_deps\directxtk12-src\Src\Shaders"
call .\CompileShaders.cmd
```

After shaders are compiled, touch all `.inc` outputs so MSBuild doesn't re-run the broken custom build step:

```bash
find build/_deps/directxtk-build/Shaders/Compiled -name "*.inc" -exec touch {} +
find build/_deps/directxtk12-build/Shaders/Compiled -name "*.inc" -exec touch {} +
```

### 3. Build

```bash
# Using build.bat (recommended):
cmd.exe //c "C:\Dev\UEVR-Mod\UEVR\build.bat"

# Or manual MSBuild:
MSBuild.exe build\uevr-proj.sln -p:Configuration=Release -m -verbosity:minimal
```

### 4. Deploy

Copy `build\bin\uevr\UEVRBackend.dll` to `C:\Users\blaer\Desktop\UEVRMonitor Mode - Copy\`

## Clean Rebuild

When struct headers change (especially `UE3D_MonitorState.hpp`), always do a full clean:

```bash
rm -rf build
# Then follow steps 1-4 above
```

Never do incremental builds after header renames or struct modifications (lesson 139).

## Known Issues

- **cmake -E env bug**: DirectXTK shader compilation fails in MSBuild custom build steps. Workaround: pre-compile + touch (see step 2).
- **MSVC 14.50 produces broken DLLs**: VS auto-update added 14.50.35717. Always pin to 14.44 via `-T "v143,version=14.44"`.
- **Shader re-run**: Even after pre-compilation, MSBuild may re-run the shader step if CMakeLists.txt is newer than outputs. Touch the .inc files to prevent this.

## Release Verification

Before deploying a build:
1. Compare DLL size against known-working release (~11.4 MB for v1.2)
2. Test in-game: no stutter, no crash on level load
3. Verify shared memory protocol matches between UEVR and VRto3D

## VRto3D Build

```bash
MSBuild.exe "C:\Dev\VRto3D\vrto3d.sln" -p:Configuration=Release -p:Platform=x64 -m -verbosity:minimal
```

Deploy to: `C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\vrto3d\bin\win64\driver_vrto3d.dll`
