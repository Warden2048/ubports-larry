@echo off
REM Shows a 1920x1080 Lomiri desktop on a virtual second screen on this PC, with this PC's keyboard and mouse.
REM Uses H.265 (sharper than H.264 at the same bitrate). If it fails to start: scrcpy-desktop.bat --video-codec=h264
REM Needs scrcpy for Windows (https://github.com/Genymobile/scrcpy/releases) unzipped into ubports-larry\scrcpy
REM The mouse is captured like in a game (raw 1:1 movement, no acceleration). Press Left-Alt to release it.
setlocal
cd /d "%~dp0"
set SCRCPY=
for /r "%~dp0..\scrcpy" %%f in (scrcpy.exe) do if exist "%%f" set SCRCPY=%%f
if "%SCRCPY%"=="" for %%f in (scrcpy.exe) do set SCRCPY=%%~$PATH:f
if "%SCRCPY%"=="" (
  echo scrcpy.exe not found. Download scrcpy-win64 from https://github.com/Genymobile/scrcpy/releases
  echo and unzip it into %~dp0..\scrcpy
  pause & exit /b 1
)
echo Using %SCRCPY%
"%SCRCPY%" --no-audio --video-codec=h265 --keyboard=uhid --mouse=uhid --mouse-bind=++++:++++ --print-fps --new-display=1920x1080 --video-bit-rate=20M --max-fps=60 --window-title="Ubuntu Touch - desktop" %* 2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath scrcpy-log.txt"
echo.
echo scrcpy exited. If something went wrong, copy the text above to Claude.
pause
