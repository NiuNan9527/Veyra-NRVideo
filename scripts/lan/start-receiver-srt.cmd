@echo off
setlocal
set "PORT=5000"
set "VEYRA=C:\Users\Administrator\Desktop\DLSS\Veyra.exe"
if not exist "%VEYRA%" (
  echo [Veyra SRT] Veyra.exe not found at %VEYRA%
  pause
  exit /b 1
)
echo [Veyra SRT] Listening on port %PORT% with 30 ms SRT latency...
start "" "%VEYRA%" "srt://0.0.0.0:%PORT%?mode=listener&transtype=live&latency=30000&tlpktdrop=1"
endlocal