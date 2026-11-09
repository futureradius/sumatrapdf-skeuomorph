@echo off
rem Builds curl-layer-spike.exe next to this file.
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /std:c++17 /W3 curl-layer-spike.cpp /Fe:curl-layer-spike.exe /link /SUBSYSTEM:CONSOLE
