@echo off
setlocal
set "PORT=5000"
set "ROOT=%~dp0..\.."
set "VEYRA=%ROOT%\Veyra.exe"
if not exist "%VEYRA%" set "VEYRA=%ROOT%\out\build\x64-release\Veyra.exe"
if not exist "%VEYRA%" (
  echo [Veyra LAN] Veyra.exe not found.
  echo Put this script inside the Veyra source or portable folder.
  pause
  exit /b 1
)
echo [Veyra LAN] Listening on UDP port %PORT%...
start "" "%VEYRA%" "udp://0.0.0.0:%PORT%?fifo_size=2048&overrun_nonfatal=1&buffer_size=1048576"
endlocal
