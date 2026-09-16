param(
    [string]$TargetIp = "192.168.163.252",
    [int]$Port = 5000,
    [int]$BitrateMbps = 60,
    [int]$Fps = 60
)

$ErrorActionPreference = "Stop"
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) { throw "ffmpeg.exe not found in PATH" }
$rate = "$($BitrateMbps)M"
$url = "udp://$($TargetIp):$($Port)?pkt_size=188&buffer_size=1048576"

Write-Host "Veyra HEVC AMF pacing A/B -> $TargetIp`:$Port"
& $ffmpeg -hide_banner `
    -readrate 1 -readrate_catchup 1.0 `
    -f lavfi -i "color=c=gray:size=3840x2160:rate=$Fps" `
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
    -g 15 `
    -gops_per_idr 1 `
    -forced_idr 1 `
    -bf 0 `
    -async_depth 1 `
    -header_insertion_mode idr `
    -mpegts_flags +resend_headers `
    -muxdelay 0 `
    -muxpreload 0 `
    -flush_packets 1 `
    -f mpegts `
    $url
exit $LASTEXITCODE