@echo off
rem Puts a Snitch Meters shortcut on the desktop and copies the VST3 into the
rem system VST3 folder (that part asks for administrator rights).
setlocal
set "ROOT=%~dp0.."
set "ART=%ROOT%\build\SnitchMeters_artefacts\Release"
set "APP=%ART%\Standalone\Snitch Meters.exe"
set "VST=%ART%\VST3\Snitch Meters.vst3"

if not exist "%APP%" (
    echo Build it first:  cmake --build build --config Release
    pause
    exit /b 1
)

powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')+'\Snitch Meters.lnk'); $s.TargetPath='%APP%'; $s.WorkingDirectory='%ART%\Standalone'; $s.Save()"
echo Desktop shortcut created.

net session >nul 2>&1
if errorlevel 1 (
    echo Asking for administrator rights to copy the VST3...
    powershell -NoProfile -Command "Start-Process cmd -Verb RunAs -ArgumentList '/c','xcopy /E /I /Y \"%VST%\" \"%CommonProgramFiles%\VST3\Snitch Meters.vst3\" & pause'"
) else (
    xcopy /E /I /Y "%VST%" "%CommonProgramFiles%\VST3\Snitch Meters.vst3"
)
echo Done. In Bitwig, rescan plug-ins and put Snitch Meters last on the master.
pause
