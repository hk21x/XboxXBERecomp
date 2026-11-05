@echo off
setlocal enabledelayedexpansion
rem A prebuilt release's package from build.bat's output: the exes, their runtime DLLs and the compiler, no game code.
rem   release-package.bat <package zip> <exe name> [more exe names]     from the game's folder, after build.bat
rem Writes the zip and <zip>.json (size, sha256), both for the tag's release. A game built per disc runs this per disc.
set "ZIP=%~1"
if "%ZIP%"=="" (echo Usage: release-package.bat ^<package zip^> ^<exe name^> [more exe names] & exit /b 1)
shift
set "OUT=out\build\release"
set "STAGE=out\package"
if not exist "%OUT%\compiler\clang.exe" (echo %OUT%\compiler\clang.exe is missing: run build.bat first, with XBE_GAME_DLL. & exit /b 1)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo The Visual C++ runtime was not found under "%VSDIR%". & exit /b 1)
if exist "%STAGE%" rd /s /q "%STAGE%"
mkdir "%STAGE%"
:exes
if "%~1"=="" goto :copied
if not exist "%OUT%\%~1.exe" (echo %OUT%\%~1.exe is missing. & exit /b 1)
copy /y "%OUT%\%~1.exe" "%STAGE%\" >nul || exit /b 1
shift
goto :exes
:copied
robocopy "%OUT%\compiler" "%STAGE%\compiler" /e /xd work /njh /njs /nfl /ndl >nul
if errorlevel 8 (echo Copying the compiler failed. & exit /b 1)
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do (
    copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\" >nul
    copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\compiler\" >nul
)
set "D3DCOMPILER=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if exist "%D3DCOMPILER%" copy /y "%D3DCOMPILER%" "%STAGE%\" >nul
del /q "%ZIP%" "%ZIP%.json" 2>nul
tar -a -c -f "%ZIP%" -C "%STAGE%" . || (echo Making the zip failed. & exit /b 1)
powershell -NoProfile -Command "$f = Get-Item '%ZIP%'; $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(); '{\"size\": ' + $f.Length + ', \"sha256\": \"' + $h + '\"}' | Set-Content -Encoding ascii '%ZIP%.json'" || exit /b 1
for %%f in ("%ZIP%") do echo %ZIP%: %%~zf bytes
endlocal
