param(
    [string]$TargetIp = "192.168.163.252",
    [int]$Port = 5000,
    [int]$BitrateMbps = 80,
    [int]$OutputIndex = 0,
    [int]$Fps = 60
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($TargetIp)) {
    $TargetIp = Read-Host "RTX/Veyra PC IPv4 address"
}

$ffmpeg = Join-Path $PSScriptRoot "ffmpeg.exe"
if (-not (Test-Path $ffmpeg)) {
    $cmd = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($cmd) { $ffmpeg = $cmd.Source }
}
if (-not (Test-Path $ffmpeg)) {
    Write-Host "ffmpeg.exe not found." -ForegroundColor Red
    Write-Host "Place a Windows FFmpeg build with ddagrab and hevc_amf beside this script, or add ffmpeg.exe to PATH."
    Read-Host "Press Enter to exit"
    exit 1
}

$encoders = & $ffmpeg -hide_banner -encoders 2>&1 | Out-String
if ($encoders -notmatch "hevc_amf") {
    Write-Host "This FFmpeg build has no HEVC AMF encoder." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 2
}
$filters = & $ffmpeg -hide_banner -filters 2>&1 | Out-String
if ($filters -notmatch "ddagrab") {
    Write-Host "This FFmpeg build has no ddagrab filter." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 3
}

$rate = "$($BitrateMbps)M"
$buffer = "2M"
$url = "udp://$($TargetIp):$($Port)?pkt_size=1316&buffer_size=1048576"

Write-Host ""
Write-Host "Veyra LAN sender"
Write-Host " Target : $($TargetIp):$Port"
Write-Host " Capture: display $OutputIndex at $Fps fps"
Write-Host " Codec  : HEVC AMF / ultra-low-latency / no B-frames"
Write-Host " Bitrate: $rate"
Write-Host " Press Ctrl+C to stop."
Write-Host ""

$args = @(
    "-hide_banner",
    "-loglevel", "warning",
    "-filter_complex", "ddagrab=output_idx=$($OutputIndex):framerate=$($Fps):draw_mouse=1,setpts=N/($($Fps)*TB)",
    "-an",
    "-c:v", "hevc_amf",
    "-usage", "ultralowlatency",
    "-quality", "speed",
    "-latency", "1",
    "-preanalysis", "0",
    "-preencode", "0",
    "-vbaq", "0",
    "-rc", "cbr",
    "-b:v", $rate,
    "-maxrate", $rate,
    "-bufsize", $buffer,
    "-g", "$([Math]::Max(15, [Math]::Round($Fps / 2)))",
    "-bf", "0",
    "-async_depth", "1",
    "-header_insertion_mode", "idr",
    "-muxdelay", "0",
    "-muxpreload", "0",
    "-flush_packets", "1",
    "-f", "mpegts",
    $url
)

& $ffmpeg @args
exit $LASTEXITCODE
