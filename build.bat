@echo off
rem Builds the mod and assembles dist\MafiaAimAssist-<version>.zip. Needs Visual Studio with the C++ desktop tools.
setlocal EnableDelayedExpansion
set "VERSION=1.0.2"
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "PKG=%ROOT%dist\MafiaAimAssist-%VERSION%"
set "XIDI_URL=https://github.com/samuelgr/Xidi/releases/download/v5.0.0/Xidi-v5.0.0.zip"
set "XIDI_SHA256=41b6d23692d7e8043deef032ae5e619ee96aaef1586f58e4a541d4c15429b8cd"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo Visual Studio was not found. & exit /b 1 )
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS ( echo Visual Studio with the C++ x86/x64 tools was not found. & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

if not exist "%BUILD%" mkdir "%BUILD%"
pushd "%BUILD%"

echo [1/3] Building MafiaAimLogic.dll
cl /nologo /W4 /O2 /MT /TC "%ROOT%src\MafiaAimLogic.c" /LD /link /MACHINE:X86 user32.lib /OUT:MafiaAimLogic.dll || goto :fail

echo [2/3] Building dinput8.dll (proxy)
cl /nologo /W4 /O2 /MT /TC /DBUILD_PROXY "%ROOT%src\MafiaAimHost.c" /LD /link /MACHINE:X86 user32.lib dxguid.lib /OUT:dinput8.dll || goto :fail

echo [3/3] Fetching Xidi and packaging
if not exist Xidi-v5.0.0.zip curl -L --fail -o Xidi-v5.0.0.zip "%XIDI_URL%" || goto :fail
set "ACTUAL="
for /f "skip=1 tokens=*" %%h in ('certutil -hashfile Xidi-v5.0.0.zip SHA256') do if not defined ACTUAL set "ACTUAL=%%h"
set "ACTUAL=!ACTUAL: =!"
if /i not "!ACTUAL!"=="%XIDI_SHA256%" ( echo Xidi download failed the SHA-256 check. & del Xidi-v5.0.0.zip & goto :fail )
if exist xidi-extracted rmdir /s /q xidi-extracted
mkdir xidi-extracted
tar -xf Xidi-v5.0.0.zip -C xidi-extracted || goto :fail
popd

if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%\files\xidi"
copy /y "%BUILD%\dinput8.dll" "%PKG%\files\dinput8.dll" >nul
copy /y "%BUILD%\MafiaAimLogic.dll" "%PKG%\files\MafiaAimLogic.dll" >nul
copy /y "%ROOT%config\MafiaAimAssist.ini" "%PKG%\files\MafiaAimAssist.ini" >nul
copy /y "%ROOT%config\Xidi.ini" "%PKG%\files\xidi\Xidi.ini" >nul
set "XSRC=%BUILD%\xidi-extracted\Xidi-v5.0.0"
for %%f in ("%XSRC%\Win32\dinput.dll" "%XSRC%\Win32\Xidi.32.dll" "%XSRC%\LICENSE") do if not exist %%f ( echo Missing from the Xidi package: %%f & exit /b 1 )
copy /y "%XSRC%\Win32\dinput.dll" "%PKG%\files\xidi\dinput.dll" >nul
copy /y "%XSRC%\Win32\Xidi.32.dll" "%PKG%\files\xidi\Xidi.32.dll" >nul
copy /y "%XSRC%\LICENSE" "%PKG%\files\xidi\LICENSE" >nul
copy /y "%ROOT%README.md" "%PKG%\README.md" >nul
copy /y "%ROOT%LICENSE" "%PKG%\LICENSE" >nul
copy /y "%ROOT%THIRD_PARTY_NOTICES.md" "%PKG%\THIRD_PARTY_NOTICES.md" >nul

if exist "%ROOT%dist\MafiaAimAssist-%VERSION%.zip" del "%ROOT%dist\MafiaAimAssist-%VERSION%.zip"
tar -a -cf "%ROOT%dist\MafiaAimAssist-%VERSION%.zip" -C "%ROOT%dist" "MafiaAimAssist-%VERSION%" || goto :fail
echo.
echo Done: %ROOT%dist\MafiaAimAssist-%VERSION%.zip
exit /b 0

:fail
popd
echo Build failed.
exit /b 1
