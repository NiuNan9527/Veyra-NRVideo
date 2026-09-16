#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

double qpcMs(LONGLONG delta, LONGLONG freq)
{
    return freq > 0 ? (1000.0 * double(delta) / double(freq)) : 0.0;
}

double durationMs(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = p * double(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(values.size() - 1, lo + 1);
    const double t = pos - double(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

void printStats(const char* name, const std::vector<double>& gaps)
{
    uint64_t gt25 = 0, gt40 = 0;
    for (double v : gaps) {
        if (v > 25.0) ++gt25;
        if (v > 40.0) ++gt40;
    }
    const double maxv = gaps.empty() ? 0.0 : *std::max_element(gaps.begin(), gaps.end());
    std::printf("%s p50=%.2f p95=%.2f p99=%.2f max=%.2f >25ms=%llu >40ms=%llu\n",
        name,
        percentile(gaps, .50), percentile(gaps, .95), percentile(gaps, .99), maxv,
        static_cast<unsigned long long>(gt25),
        static_cast<unsigned long long>(gt40));
}

bool isSoftwareAdapter(const DXGI_ADAPTER_DESC1& d)
{
    return (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

}

int wmain(int argc, wchar_t** argv)
{
    int seconds = 30;
    int wantedAdapter = -1;
    int wantedOutput = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = std::max(5, _wtoi(argv[++i]));
        else if (a == L"--adapter" && i + 1 < argc) wantedAdapter = _wtoi(argv[++i]);
        else if (a == L"--output" && i + 1 < argc) wantedOutput = std::max(0, _wtoi(argv[++i]));
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "CreateDXGIFactory1 failed\n");
        return 2;
    }

    struct Candidate {
        UINT index = 0;
        DXGI_ADAPTER_DESC1 desc{};
        ComPtr<IDXGIAdapter1> adapter;
        std::vector<ComPtr<IDXGIOutput>> outputs;
    };
    std::vector<Candidate> candidates;

    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;

        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (isSoftwareAdapter(ad)) continue;

        Candidate c;
        c.index = ai;
        c.desc = ad;
        c.adapter = adapter;
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) break;
            c.outputs.push_back(output);
        }
        if (!c.outputs.empty()) candidates.push_back(std::move(c));
    }

    if (candidates.empty()) {
        std::fprintf(stderr, "No hardware adapter with desktop outputs found\n");
        return 3;
    }

    std::printf("Adapters with outputs:\n");
    for (const auto& c : candidates) {
        std::wprintf(L"  adapter=%u vendor=0x%04X outputs=%zu name=%ls\n",
            c.index, c.desc.VendorId, c.outputs.size(), c.desc.Description);
    }

    Candidate* selected = nullptr;
    if (wantedAdapter >= 0) {
        for (auto& c : candidates) if (int(c.index) == wantedAdapter) selected = &c;
    } else {
        for (auto& c : candidates) {
            if (c.desc.VendorId == 0x1002) { selected = &c; break; } // AMD first
        }
        if (!selected) selected = &candidates.front();
    }

    if (!selected || wantedOutput >= int(selected->outputs.size())) {
        std::fprintf(stderr, "Requested adapter/output is unavailable\n");
        return 4;
    }

    DXGI_OUTPUT_DESC od{};
    selected->outputs[wantedOutput]->GetDesc(&od);
    std::wprintf(L"Selected adapter=%u output=%d monitor=%ls rect=%ld,%ld %ldx%ld\n",
        selected->index, wantedOutput, selected->desc.Description,
        od.DesktopCoordinates.left, od.DesktopCoordinates.top,
        od.DesktopCoordinates.right - od.DesktopCoordinates.left,
        od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL actual{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(
        selected->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, &actual, &context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed hr=0x%08X\n", unsigned(hr));
        return 5;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = selected->outputs[wantedOutput].As(&output1);
    if (FAILED(hr)) {
        std::fprintf(stderr, "IDXGIOutput1 unavailable hr=0x%08X\n", unsigned(hr));
        return 6;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed hr=0x%08X\n", unsigned(hr));
        std::fprintf(stderr, "Close other duplication/capture apps and retry.\n");
        return 7;
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    std::printf("DXGI Desktop Duplication timing probe: %d seconds\n", seconds);
    std::printf("Move windows / play motion on this output during the test.\n");

    const auto start = Clock::now();
    auto lastContentAcquire = Clock::time_point{};
    LONGLONG lastPresent = 0;
    std::vector<double> contentAcquireGaps;
    std::vector<double> presentGaps;
    uint64_t callbacks = 0;
    uint64_t contentFrames = 0;
    uint64_t timeouts = 0;
    uint64_t pointerOnly = 0;
    uint64_t accumulatedExtra = 0;

    while (durationMs(Clock::now() - start) < seconds * 1000.0) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(1000, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            ++timeouts;
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::fprintf(stderr, "DuplicateOutput access lost (display mode/session changed)\n");
            break;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "AcquireNextFrame failed hr=0x%08X\n", unsigned(hr));
            break;
        }

        ++callbacks;
        const auto now = Clock::now();

        if (info.LastPresentTime.QuadPart != 0) {
            if (lastContentAcquire != Clock::time_point{}) {
                contentAcquireGaps.push_back(durationMs(now - lastContentAcquire));
            }
            lastContentAcquire = now;
            ++contentFrames;

            if (lastPresent != 0) {
                presentGaps.push_back(qpcMs(info.LastPresentTime.QuadPart - lastPresent, freq.QuadPart));
            }
            lastPresent = info.LastPresentTime.QuadPart;

            if (info.AccumulatedFrames > 1) accumulatedExtra += (info.AccumulatedFrames - 1);
        } else {
            ++pointerOnly;
        }

        duplication->ReleaseFrame();
    }

    const double elapsed = durationMs(Clock::now() - start) / 1000.0;
    std::printf("\nRESULT\n");
    std::printf("callbacks=%llu content_frames=%llu elapsed=%.3fs content_fps=%.2f timeouts=%llu pointer_only=%llu accumulated_extra=%llu\n",
        static_cast<unsigned long long>(callbacks),
        static_cast<unsigned long long>(contentFrames), elapsed,
        elapsed > 0.0 ? double(contentFrames) / elapsed : 0.0,
        static_cast<unsigned long long>(timeouts),
        static_cast<unsigned long long>(pointerOnly),
        static_cast<unsigned long long>(accumulatedExtra));
    printStats("content_acquire_gap_ms", contentAcquireGaps);
    printStats("present_gap_ms", presentGaps);
    if (contentFrames < static_cast<uint64_t>(seconds * 30)) {
        std::printf("NOTE: too few content presents for a 60-fps pacing judgement. Keep a game/video continuously moving and keep the mouse still, then rerun.\n");
    }
    return 0;
}
