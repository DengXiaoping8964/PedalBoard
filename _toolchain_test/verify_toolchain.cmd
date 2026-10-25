@echo off
rem  Verifies the toolchain is not just present but usable, by asking the
rem  compiler itself rather than trusting a directory listing.
setlocal

echo ============ vswhere ============
if exist "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" (
  "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -all -prerelease -products * -format value -property installationPath
) else (
  echo MISSING: vswhere.exe
)

echo ============ instances under D:\BuildTools ============
if exist "D:\BuildTools" (dir /b "D:\BuildTools") else (echo MISSING: D:\BuildTools)

echo ============ MSVC toolsets ============
if exist "D:\BuildTools\VC\Tools\MSVC" (dir /b "D:\BuildTools\VC\Tools\MSVC") else (echo none)

echo ============ Windows SDK (this one lands on C: by design) ============
if exist "C:\Program Files (x86)\Windows Kits\10\Include" (dir /b "C:\Program Files (x86)\Windows Kits\10\Include") else (echo none)

echo ============ call the compiler through vcvars64 ============
if not exist "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
  echo MISSING: vcvars64.bat
  exit /b 1
)
call "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl 2>&1 | findstr /i "Version"
where cmake
cmake --version 2>&1 | findstr /i "version"
where ninja
ninja --version 2>&1
where rc
echo ============ done ============
