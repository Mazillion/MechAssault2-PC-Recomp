@echo off
rem Builds stub.exe, the single-file installer's native shell (pack_installer.py appends the kit).
rem The version resources come from ..\VERSION (line 1 the public version, line 2 the release date).
rem Needs the MSVC x64 tools: run from a "x64 Native Tools" prompt, or let this script find Visual Studio
rem (2019 or newer, or Build Tools) with vswhere.
rem /Brepro and /PDBALTPATH keep build paths and time stamps out of the exe (reproducible, no local paths).
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call :vcvars || exit /b 1
set PUBVER=
set PUBDATE=
for /f "usebackq tokens=* delims=" %%a in ("%~dp0..\VERSION") do call :take "%%a"
if not defined PUBVER (echo VERSION is missing or empty & exit /b 1)
if not defined PUBDATE (echo VERSION needs the release date on line 2 & exit /b 1)
rem A preview build (1.2.3-preview.4) keeps the numbers of 1.2.3 in the version resource.
for /f "tokens=1 delims=-" %%s in ("%PUBVER%") do set PUBNUM=%%s
for /f "tokens=1-3 delims=." %%x in ("%PUBNUM%") do (set V1=%%x& set V2=%%y& set V3=%%z)
if not defined V3 set V3=0
> version.h echo #define MA2_VER_COMMA %V1%,%V2%,%V3%,0
>> version.h echo #define MA2_VER_STR "%PUBVER%"
>> version.h echo #define MA2_VER_FILE "%PUBNUM%.0"
>> version.h echo #define MA2_VER_DATE "%PUBDATE%"
rc /nologo /fo stub.res stub.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /Brepro /DUNICODE /D_UNICODE stub.cpp stub.res /Fe:stub.exe /link /SUBSYSTEM:WINDOWS /Brepro /PDBALTPATH:%%_PDB%% user32.lib || exit /b 1
del stub.obj stub.res
echo STUB OK (version %PUBVER%, %PUBDATE%)
exit /b 0

:take
if not defined PUBVER (set PUBVER=%~1& exit /b 0)
if not defined PUBDATE set PUBDATE=%~1
exit /b 0

:vcvars
rem vswhere ships with every Visual Studio 2017+ and Build Tools install. pushd avoids the ")" of
rem "Program Files (x86)" inside the for /f command.
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (echo Visual Studio / Build Tools not found: install the "Desktop development with C++" workload & exit /b 1)
pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
popd
if not defined VSDIR (echo No Visual Studio with the MSVC x64 tools found & exit /b 1)
rem vcvars64.bat runs vswhere by name: put it on PATH so it does not print "not recognized".
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
