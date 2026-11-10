@echo off
rem Runs SumatraPDF's code generator (bun cmd/gen-code.ts) with what it needs on PATH:
rem vswhere (VS Installer), cl.exe (vcvars64), clang-format (MSBuild folder) and bun.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin;%USERPROFILE%\.bun\bin;%PATH%"
cd /d "%~dp0..\.."
bun cmd/gen-code.ts
