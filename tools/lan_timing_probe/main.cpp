#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "veyra/Result.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/media/FFmpegDemuxer.h"
#include "veyra/media/FFmpegVideoDecoder.h"

namespace {
using Clock = std::chrono::steady_clock;

double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = p * double(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(values.size() - 1, lo + 1);
    const double t = pos - double(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}
}

int wmain(int argc, wchar_t** argv) {
    std::wstring input = L"tcp://0.0.0.0:5000?listen=1&tcp_nodelay=1";
    int seconds = 30;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--input" && i + 1 < argc) input = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) seconds = std::max(5, _wtoi(argv[++i]));
    }

    std::wprintf(L"Veyra LAN timing probe\n");
    std::wprintf(L"input: %ls\n", input.c_str());
    std::wprintf(L"duration: %d s\n", seconds);

    veyra::gfx::D3D12DeviceContext ctx;
    veyra::gfx::DeviceContextDesc desc{};
    desc.commandSlotCount = 4;
    veyra::Status st = veyra::Status::Ok;
    if (!ctx.initialize(desc, st)) {
        std::fprintf(stderr, "D3D12 init failed\n");
        return 2;
    }

    veyra::media::FFmpegDemuxer demux;
    if (!demux.open(input)) {
        std::fprintf(stderr, "demux open failed\n");
        return 3;
    }

    veyra::media::FFmpegVideoDecoder dec;
    if (!dec.openD3D12VA(demux.videoCodecParameters(),
            demux.videoTimeBaseNum(), demux.videoTimeBaseDen(),
            ctx.device(), ctx.directQueue())) {
        std::fprintf(stderr, "D3D12VA open failed\n");
        return 4;
    }

    std::printf("decoder: D3D12VA active, %dx%d\n", dec.width(), dec.height());

    const auto start = Clock::now();
    auto lastFrame = Clock::time_point{};
    auto lastPacket = Clock::time_point{};
    std::vector<double> frameGaps;
    std::vector<double> packetGaps;
    uint64_t frames = 0, packets = 0, longFrameGaps = 0, longPacketGaps = 0;

    while (ms(Clock::now() - start) < seconds * 1000.0) {
        bool eof = false;
        if (!demux.readVideoPacket(eof)) {
            if (eof) break;
            Sleep(1);
            continue;
        }
        const auto packetNow = Clock::now();
        if (lastPacket != Clock::time_point{}) {
            const double gap = ms(packetNow - lastPacket);
            packetGaps.push_back(gap);
            if (gap > 40.0) ++longPacketGaps;
        }
        lastPacket = packetNow;
        ++packets;

        if (!dec.sendPacket(demux.currentPacket())) {
            std::fprintf(stderr, "decoder send packet failed\n");
            break;
        }
        while (const AVFrame* frame = dec.receiveFrame()) {
            (void)frame;
            const auto frameNow = Clock::now();
            if (lastFrame != Clock::time_point{}) {
                const double gap = ms(frameNow - lastFrame);
                frameGaps.push_back(gap);
                if (gap > 40.0) ++longFrameGaps;
            }
            lastFrame = frameNow;
            ++frames;
        }
    }

    const double elapsed = ms(Clock::now() - start) / 1000.0;
    std::printf("\nRESULT\n");
    std::printf("frames=%llu packets=%llu elapsed=%.3fs fps=%.2f\n",
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(packets),
        elapsed, elapsed > 0 ? double(frames) / elapsed : 0.0);
    std::printf("frame_gap_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f >40ms=%llu\n",
        percentile(frameGaps, .50), percentile(frameGaps, .95),
        percentile(frameGaps, .99),
        frameGaps.empty() ? 0.0 : *std::max_element(frameGaps.begin(), frameGaps.end()),
        static_cast<unsigned long long>(longFrameGaps));
    std::printf("packet_gap_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f >40ms=%llu\n",
        percentile(packetGaps, .50), percentile(packetGaps, .95),
        percentile(packetGaps, .99),
        packetGaps.empty() ? 0.0 : *std::max_element(packetGaps.begin(), packetGaps.end()),
        static_cast<unsigned long long>(longPacketGaps));
    std::printf("decoder_frames=%llu queue_waits=%llu pts_nonmono=%llu\n",
        static_cast<unsigned long long>(dec.stats().framesDecoded),
        static_cast<unsigned long long>(dec.gpuQueueWaitCount()),
        static_cast<unsigned long long>(dec.stats().ptsNonMonotonicCount + demux.stats().ptsNonMonotonicCount));
    return 0;
}
