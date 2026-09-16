#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic_bool g_stop{false};

BOOL WINAPI consoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

std::string fferr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

double ms(Clock::duration d)
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

void printGapStats(const char* name, const std::vector<double>& gaps)
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

struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    std::atomic_bool busy{false};
};

struct SlotGuard {
    Slot* slot = nullptr;
    ID3D11Texture2D* texture = nullptr;
};

void releaseSlot(void* opaque, uint8_t* data)
{
    auto* guard = static_cast<SlotGuard*>(opaque);
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(data);
    if (texture) texture->Release();
    if (guard && guard->slot) guard->slot->busy.store(false, std::memory_order_release);
    delete guard;
}

bool setOpt(void* obj, const char* name, const char* value)
{
    const int r = av_opt_set(obj, name, value, 0);
    if (r < 0) {
        std::fprintf(stderr, "warning: AMF option %s=%s rejected: %s\n", name, value, fferr(r).c_str());
        return false;
    }
    return true;
}

bool setOptInt(void* obj, const char* name, int64_t value)
{
    const int r = av_opt_set_int(obj, name, value, 0);
    if (r < 0) {
        std::fprintf(stderr, "warning: AMF option %s=%lld rejected: %s\n",
            name, static_cast<long long>(value), fferr(r).c_str());
        return false;
    }
    return true;
}

struct EncoderMux {
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFormatContext* mux = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    int fps = 60;
    int64_t ptsStep = 1500;
    std::vector<double> packetWriteGaps;
    Clock::time_point lastPacketWrite{};
    uint64_t packetsWritten = 0;

    ~EncoderMux() { close(); }

    void close()
    {
        if (encoder) {
            avcodec_send_frame(encoder, nullptr);
            drain(true);
        }
        if (mux) {
            if (mux->pb) {
                av_write_trailer(mux);
                avio_closep(&mux->pb);
            }
            avformat_free_context(mux);
            mux = nullptr;
        }
        if (packet) av_packet_free(&packet);
        if (encoder) avcodec_free_context(&encoder);
        av_buffer_unref(&hwFrames);
        av_buffer_unref(&hwDevice);
        stream = nullptr;
    }

    bool init(ID3D11Device* device, int width, int height, int targetFps,
              int bitrateMbps, const std::string& url)
    {
        fps = targetFps;
        ptsStep = 90000 / std::max(1, fps);

        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice) {
            std::fprintf(stderr, "av_hwdevice_ctx_alloc(D3D11VA) failed\n");
            return false;
        }
        auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(hwDevice->data);
        auto* d3dCtx = reinterpret_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
        device->AddRef();
        d3dCtx->device = device;
        int r = av_hwdevice_ctx_init(hwDevice);
        if (r < 0) {
            std::fprintf(stderr, "av_hwdevice_ctx_init failed: %s\n", fferr(r).c_str());
            return false;
        }

        hwFrames = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrames) {
            std::fprintf(stderr, "av_hwframe_ctx_alloc failed\n");
            return false;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(hwFrames->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = AV_PIX_FMT_BGRA;
        frames->width = width;
        frames->height = height;
        frames->initial_pool_size = 0;
        r = av_hwframe_ctx_init(hwFrames);
        if (r < 0) {
            std::fprintf(stderr, "av_hwframe_ctx_init BGRA failed: %s\n", fferr(r).c_str());
            return false;
        }

        const AVCodec* codec = avcodec_find_encoder_by_name("hevc_amf");
        if (!codec) {
            std::fprintf(stderr, "hevc_amf encoder not found in this FFmpeg build\n");
            return false;
        }
        encoder = avcodec_alloc_context3(codec);
        if (!encoder) return false;

        encoder->width = width;
        encoder->height = height;
        encoder->pix_fmt = AV_PIX_FMT_D3D11;
        encoder->time_base = AVRational{1, 90000};
        encoder->framerate = AVRational{fps, 1};
        encoder->bit_rate = int64_t(bitrateMbps) * 1000 * 1000;
        encoder->rc_max_rate = encoder->bit_rate;
        encoder->rc_buffer_size = 2 * 1000 * 1000;
        encoder->gop_size = 15;
        encoder->max_b_frames = 0;
        encoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
        encoder->hw_frames_ctx = av_buffer_ref(hwFrames);
        if (!encoder->hw_frames_ctx) return false;

        setOpt(encoder->priv_data, "usage", "ultralowlatency");
        setOpt(encoder->priv_data, "quality", "speed");
        setOpt(encoder->priv_data, "rc", "cbr");
        setOptInt(encoder->priv_data, "latency", 1);
        setOptInt(encoder->priv_data, "preanalysis", 0);
        setOptInt(encoder->priv_data, "preencode", 0);
        setOptInt(encoder->priv_data, "vbaq", 0);
        setOptInt(encoder->priv_data, "gops_per_idr", 1);
        setOptInt(encoder->priv_data, "forced_idr", 1);
        setOpt(encoder->priv_data, "header_insertion_mode", "idr");

        r = avcodec_open2(encoder, codec, nullptr);
        if (r < 0) {
            std::fprintf(stderr, "avcodec_open2(hevc_amf) failed: %s\n", fferr(r).c_str());
            return false;
        }

        r = avformat_alloc_output_context2(&mux, nullptr, "mpegts", url.c_str());
        if (r < 0 || !mux) {
            std::fprintf(stderr, "avformat_alloc_output_context2 failed: %s\n", fferr(r).c_str());
            return false;
        }
        mux->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        mux->max_delay = 0;

        stream = avformat_new_stream(mux, nullptr);
        if (!stream) return false;
        stream->time_base = encoder->time_base;
        r = avcodec_parameters_from_context(stream->codecpar, encoder);
        if (r < 0) {
            std::fprintf(stderr, "avcodec_parameters_from_context failed: %s\n", fferr(r).c_str());
            return false;
        }

        AVDictionary* ioOpts = nullptr;
        r = avio_open2(&mux->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &ioOpts);
        av_dict_free(&ioOpts);
        if (r < 0) {
            std::fprintf(stderr, "avio_open2(%s) failed: %s\n", url.c_str(), fferr(r).c_str());
            return false;
        }

        AVDictionary* muxOpts = nullptr;
        av_dict_set(&muxOpts, "mpegts_flags", "+resend_headers", 0);
        av_dict_set(&muxOpts, "muxdelay", "0", 0);
        av_dict_set(&muxOpts, "muxpreload", "0", 0);
        r = avformat_write_header(mux, &muxOpts);
        av_dict_free(&muxOpts);
        if (r < 0) {
            std::fprintf(stderr, "avformat_write_header failed: %s\n", fferr(r).c_str());
            return false;
        }

        packet = av_packet_alloc();
        if (!packet) return false;
        return true;
    }

    bool drain(bool flushing = false)
    {
        if (!encoder || !packet) return false;
        for (;;) {
            const int r = avcodec_receive_packet(encoder, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                std::fprintf(stderr, "avcodec_receive_packet failed: %s\n", fferr(r).c_str());
                return false;
            }

            av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;

            const auto now = Clock::now();
            if (lastPacketWrite != Clock::time_point{}) {
                packetWriteGaps.push_back(ms(now - lastPacketWrite));
            }
            lastPacketWrite = now;

            const int wr = av_interleaved_write_frame(mux, packet);
            av_packet_unref(packet);
            if (wr < 0) {
                std::fprintf(stderr, "av_interleaved_write_frame failed: %s\n", fferr(wr).c_str());
                return false;
            }
            ++packetsWritten;
            if (mux->pb) avio_flush(mux->pb);
        }
    }

    bool submit(ID3D11Texture2D* texture, Slot& slot, int64_t pts)
    {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;

        frame->format = AV_PIX_FMT_D3D11;
        frame->width = encoder->width;
        frame->height = encoder->height;
        frame->pts = pts;
        frame->duration = ptsStep;
        frame->hw_frames_ctx = av_buffer_ref(hwFrames);
        if (!frame->hw_frames_ctx) {
            av_frame_free(&frame);
            return false;
        }

        texture->AddRef();
        auto* guard = new (std::nothrow) SlotGuard{&slot, texture};
        if (!guard) {
            texture->Release();
            av_frame_free(&frame);
            return false;
        }
        frame->buf[0] = av_buffer_create(
            reinterpret_cast<uint8_t*>(texture), 1, releaseSlot, guard, 0);
        if (!frame->buf[0]) {
            delete guard;
            texture->Release();
            av_frame_free(&frame);
            return false;
        }
        frame->data[0] = reinterpret_cast<uint8_t*>(texture);
        frame->data[1] = nullptr;

        int r = avcodec_send_frame(encoder, frame);
        if (r == AVERROR(EAGAIN)) {
            if (!drain()) {
                av_frame_free(&frame);
                return false;
            }
            r = avcodec_send_frame(encoder, frame);
        }
        av_frame_free(&frame);

        if (r < 0) {
            std::fprintf(stderr, "avcodec_send_frame failed: %s\n", fferr(r).c_str());
            return false;
        }
        return drain();
    }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::string targetIp = "192.168.163.252";
    int port = 5000;
    int fps = 60;
    int bitrateMbps = 60;
    int seconds = 30;
    int wantedAdapter = -1;
    int wantedOutput = 0;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--target" && i + 1 < argc) {
            const std::wstring w = argv[++i];
            targetIp.assign(w.begin(), w.end());
        } else if (a == L"--port" && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        } else if (a == L"--fps" && i + 1 < argc) {
            fps = std::clamp(_wtoi(argv[++i]), 30, 120);
        } else if (a == L"--bitrate" && i + 1 < argc) {
            bitrateMbps = std::clamp(_wtoi(argv[++i]), 10, 200);
        } else if (a == L"--seconds" && i + 1 < argc) {
            seconds = std::max(0, _wtoi(argv[++i]));
        } else if (a == L"--adapter" && i + 1 < argc) {
            wantedAdapter = _wtoi(argv[++i]);
        } else if (a == L"--output" && i + 1 < argc) {
            wantedOutput = std::max(0, _wtoi(argv[++i]));
        }
    }

    SetConsoleCtrlHandler(consoleHandler, TRUE);
    avformat_network_init();

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

    Candidate* selected = nullptr;
    if (wantedAdapter >= 0) {
        for (auto& c : candidates) if (int(c.index) == wantedAdapter) selected = &c;
    } else {
        for (auto& c : candidates) {
            if (c.desc.VendorId == 0x1002) { selected = &c; break; }
        }
        if (!selected && !candidates.empty()) selected = &candidates.front();
    }
    if (!selected || wantedOutput >= int(selected->outputs.size())) {
        std::fprintf(stderr, "No requested hardware adapter/output found\n");
        return 3;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    selected->outputs[wantedOutput]->GetDesc(&outputDesc);
    const int width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    const int height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    std::wprintf(L"Veyra native LAN sender\n");
    std::wprintf(L"adapter=%u vendor=0x%04X name=%ls output=%d size=%dx%d\n",
        selected->index, selected->desc.VendorId, selected->desc.Description,
        wantedOutput, width, height);
    std::printf("target=%s:%d fps=%d bitrate=%d Mbps duration=%s\n",
        targetIp.c_str(), port, fps, bitrateMbps, seconds ? std::to_string(seconds).c_str() : "until Ctrl+C");

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
        return 4;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = selected->outputs[wantedOutput].As(&output1);
    if (FAILED(hr)) {
        std::fprintf(stderr, "IDXGIOutput1 unavailable hr=0x%08X\n", unsigned(hr));
        return 5;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed hr=0x%08X\n", unsigned(hr));
        return 6;
    }

    const std::string url =
        "udp://" + targetIp + ":" + std::to_string(port) +
        "?pkt_size=188&buffer_size=1048576";

    std::array<Slot, 12> slots;
    bool slotsReady = false;
    EncoderMux enc;

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    const LONGLONG periodQpc = std::max<LONGLONG>(1, qpcFreq.QuadPart / fps);
    LONGLONG nextDueQpc = 0;
    int64_t nextPts = 0;

    const auto startWall = Clock::now();
    auto lastSubmitWall = Clock::time_point{};
    std::vector<double> submitGaps;
    uint64_t contentFrames = 0;
    uint64_t submitted = 0;
    uint64_t skippedByPacer = 0;
    uint64_t noFreeSlot = 0;
    uint64_t accumulatedExtra = 0;

    std::printf("Waiting for desktop presents...\n");

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (seconds > 0 && ms(Clock::now() - startWall) >= seconds * 1000.0) break;

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(1000, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::fprintf(stderr, "DuplicateOutput access lost\n");
            break;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "AcquireNextFrame failed hr=0x%08X\n", unsigned(hr));
            break;
        }

        bool releaseNeeded = true;
        auto releaseFrame = [&]() {
            if (releaseNeeded) {
                duplication->ReleaseFrame();
                releaseNeeded = false;
            }
        };

        if (info.LastPresentTime.QuadPart == 0 || !resource) {
            releaseFrame();
            continue;
        }

        ++contentFrames;
        if (info.AccumulatedFrames > 1) accumulatedExtra += info.AccumulatedFrames - 1;

        ComPtr<ID3D11Texture2D> sourceTexture;
        hr = resource.As(&sourceTexture);
        if (FAILED(hr) || !sourceTexture) {
            releaseFrame();
            continue;
        }

        D3D11_TEXTURE2D_DESC srcDesc{};
        sourceTexture->GetDesc(&srcDesc);
        if (srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            std::fprintf(stderr,
                "Unsupported desktop texture format=%u. First prototype requires SDR BGRA8.\n",
                unsigned(srcDesc.Format));
            releaseFrame();
            return 7;
        }

        if (!slotsReady) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = srcDesc.Width;
            d.Height = srcDesc.Height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

            for (auto& slot : slots) {
                hr = device->CreateTexture2D(&d, nullptr, &slot.texture);
                if (FAILED(hr)) {
                    std::fprintf(stderr, "CreateTexture2D sender pool failed hr=0x%08X\n", unsigned(hr));
                    releaseFrame();
                    return 8;
                }
            }

            if (!enc.init(device.Get(), int(srcDesc.Width), int(srcDesc.Height),
                    fps, bitrateMbps, url)) {
                releaseFrame();
                return 9;
            }
            slotsReady = true;
            nextDueQpc = info.LastPresentTime.QuadPart;
            std::printf("Native DXGI -> D3D11 -> HEVC AMF -> UDP initialized.\n");
        }

        const LONGLONG presentQpc = info.LastPresentTime.QuadPart;
        if (presentQpc < nextDueQpc) {
            ++skippedByPacer;
            releaseFrame();
            continue;
        }

        // Advance the fixed 60-Hz media clock. If Windows delivered several
        // compositor presents before this callback, skip old periods instead
        // of trying to burst them later.
        int skippedPeriods = 0;
        while (nextDueQpc + periodQpc <= presentQpc) {
            nextDueQpc += periodQpc;
            nextPts += enc.ptsStep;
            ++skippedPeriods;
        }
        if (skippedPeriods > 0) skippedByPacer += skippedPeriods;
        nextDueQpc += periodQpc;

        Slot* chosen = nullptr;
        for (auto& slot : slots) {
            bool expected = false;
            if (slot.busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                chosen = &slot;
                break;
            }
        }
        if (!chosen) {
            ++noFreeSlot;
            nextPts += enc.ptsStep;
            releaseFrame();
            continue;
        }

        context->CopyResource(chosen->texture.Get(), sourceTexture.Get());
        context->Flush();
        releaseFrame();

        const auto submitNow = Clock::now();
        if (lastSubmitWall != Clock::time_point{}) {
            submitGaps.push_back(ms(submitNow - lastSubmitWall));
        }
        lastSubmitWall = submitNow;

        if (!enc.submit(chosen->texture.Get(), *chosen, nextPts)) {
            chosen->busy.store(false, std::memory_order_release);
            break;
        }

        ++submitted;
        nextPts += enc.ptsStep;
    }

    const double elapsed = ms(Clock::now() - startWall) / 1000.0;
    std::printf("\nRESULT\n");
    std::printf("content_frames=%llu submitted=%llu packets=%llu elapsed=%.3fs submit_fps=%.2f accumulated_extra=%llu skipped_by_pacer=%llu no_free_slot=%llu\n",
        static_cast<unsigned long long>(contentFrames),
        static_cast<unsigned long long>(submitted),
        static_cast<unsigned long long>(enc.packetsWritten),
        elapsed,
        elapsed > 0.0 ? double(submitted) / elapsed : 0.0,
        static_cast<unsigned long long>(accumulatedExtra),
        static_cast<unsigned long long>(skippedByPacer),
        static_cast<unsigned long long>(noFreeSlot));
    printGapStats("submit_gap_ms", submitGaps);
    printGapStats("packet_write_gap_ms", enc.packetWriteGaps);

    avformat_network_deinit();
    return 0;
}
