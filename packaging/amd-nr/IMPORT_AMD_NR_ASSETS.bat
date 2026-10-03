@echo off
chcp 65001 >nul
set "SRC=%~1"
if "%SRC%"=="" (
  echo Drag the native-game-tiled-assets folder onto this BAT file.
  pause
  exit /b 2
)
if not exist "%SRC%\HIP\SHA256SUMS" (
  echo Wrong folder: HIP\SHA256SUMS was not found.
  pause
  exit /b 3
)
set "DST=%~dp0runtime\experimental\amd-lmxxf"
robocopy "%SRC%" "%DST%" /E /R:1 /W:1
if errorlevel 8 (
  echo Import failed.
  pause
  exit /b 4
)
echo.
echo AMD NR assets imported successfully.
echo Start Veyra-AMD-NR.exe and select AMD RDNA4 - lmxxf for NR runtime.
pause
