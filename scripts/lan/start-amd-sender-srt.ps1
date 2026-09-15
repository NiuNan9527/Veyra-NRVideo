param(
    [string]$TargetIp = "192.168.163.252",
    [int]$Port = 5000,
    [int]$BitrateMbps = 60,
    [int]$OutputIndex = 0,
    [int]$Fps = 60,
    [int]$SrtLatencyUs = 30000
)

$ErrorActionPreference = "Stop"
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    Write-Host "ffmpeg.exe not found in PATH." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}
$rate = "$($BitrateMbps)M"
$url = "srt://$($TargetIp):$($Port)?mode=caller&transtype=live&latency=$($SrtLatencyUs)&tlpktdrop=1"
$gop = [Math]::Max(15, [Math]::Round($Fps / 4))
Write-Host "Veyra SRT sender -> $TargetIp`:$Port"
Write-Host "HEVC AMF $BitrateMbps Mbps, $Fps fps, SRT latency $($SrtLatencyUs / 1000) ms"
Write-Host "Press Ctrl+C to stop."
& $ffmpeg -hide_banner `
    -filter_complex "ddagrab=output_idx=$($OutputIndex):framerate=$($Fps):draw_mouse=1,setpts=N/($($Fps)*TB)" `
    -an `
    -c:v hevc_amf `
    -usage ultralowlatency `
    -quality speed `
    -latency 1 `
    -preanalysis 0 `
    -preencode 0 `
    -vbaq 0 `
    -rc cbr `
    -b:v $rate `
    -maxrate $rate `
    -bufsize 2M `
    -g $gop `
    -gops_per_idr 1 `
    -bf 0 `
    -async_depth 1 `
    -forced_idr 1 `
    -header_insertion_mode idr `
    -muxdelay 0 `
    -muxpreload 0 `
    -flush_packets 1 `
    -mpegts_flags +resend_headers `
    -f mpegts `
    $url
exit $LASTEXITCODE