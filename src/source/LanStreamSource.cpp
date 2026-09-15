#include "veyra/source/LanStreamSource.h"

#include "veyra/Log.h"
#include "veyra/media/InputUrl.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
}

namespace veyra::source {
namespace {

int64_t monotonic100ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}

std::shared_ptr<AVFrame> cloneFrame(const AVFrame* frame)
{
    if (frame == nullptr) {
        return {};
    }
    AVFrame* clone = av_frame_clone(frame);
    if (clone == nullptr) {
        return {};
    }
    return std::shared_ptr<AVFrame>(clone, [](AVFrame* value) {
        av_frame_free(&value);
    });
}

} // namespace

LanStreamSource::~LanStreamSource()
{
    close();
}

bool LanStreamSource::open(const SourceOpenDesc& desc)
{
    close();

    if (!media::isRealtimeNetworkUrl(desc.path)) {
        log::error("lan-source", "open rejected: URL must use tcp://, udp://, rtp://, or srt://");
        return false;
    }

    if (!inner_.open(desc)) {
        log::error("lan-source", "FFmpeg network input failed to open");
        return false;
    }

    info_ = inner_.info();
    info_.kind = pipeline::SourceKind::LanStream;
    info_.duration = pipeline::Rational::unknown();

    {
        std::lock_guard lock(mutex_);
        opened_ = true;
        failed_ = false;
        received_ = 0;
        dropped_ = 0;
        delivered_ = 0;
        rateWindowStart100ns_ = 0;
        rateWindowFrames_ = 0;
        inputFps_ = 0.0;
        latestDecoded100ns_ = 0;
    }

    stop_ = false;
    worker_ = std::thread(&LanStreamSource::pump, this);

    log::info("lan-source", std::format(
        "opened {}x{} avgFps={:.3f} codec={} hw={} capacity=1 latest-frame mailbox",
        info_.width, info_.height, info_.averageFps, info_.videoCodecName,
        info_.hardwareDecodeActive));
    return true;
}

void LanStreamSource::pump() noexcept
{
    while (!stop_) {
        pipeline::FramePacket packet;
        const AVFrame* frame = nullptr;
        const auto status = inner_.read(packet, &frame);

        if (status == SourceReadStatus::Waiting) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (status != SourceReadStatus::Frame || frame == nullptr) {
            if (!stop_) {
                std::lock_guard lock(mutex_);
                failed_ = true;
            }
            break;
        }

        auto owned = cloneFrame(frame);
        if (!owned) {
            std::lock_guard lock(mutex_);
            failed_ = true;
            log::error("lan-source", "av_frame_clone failed");
            break;
        }

        const int64_t now = monotonic100ns();
        packet.sourceKind = pipeline::SourceKind::LanStream;
        if (packet.arrivalHost100ns == 0) {
            packet.arrivalHost100ns = now;
        }
        packet.decodedHost100ns = now;

        {
            std::lock_guard lock(mutex_);
            ++received_;
            if (latestFrame_) {
                ++dropped_;
                packet.flags |= static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Drop);
            }
            latestFrame_ = std::move(owned);
            latestPacket_ = packet;
            latestDecoded100ns_ = now;

            if (rateWindowStart100ns_ == 0) {
                rateWindowStart100ns_ = now;
                rateWindowFrames_ = 0;
            }
            ++rateWindowFrames_;
            const int64_t elapsed = now - rateWindowStart100ns_;
            if (elapsed >= 10000000) {
                inputFps_ = double(rateWindowFrames_) * 10000000.0 / double(elapsed);
                rateWindowStart100ns_ = now;
                rateWindowFrames_ = 0;
            }
        }
    }

    log::info("lan-source", "network pump stopped");
}

SourceReadStatus LanStreamSource::read(pipeline::FramePacket& out, const AVFrame** decodedFrame)
{
    if (decodedFrame != nullptr) {
        *decodedFrame = nullptr;
    }
    out = pipeline::FramePacket{};

    std::lock_guard lock(mutex_);
    deliveredFrame_.reset();

    if (latestFrame_) {
        deliveredFrame_ = std::move(latestFrame_);
        out = latestPacket_;
        ++delivered_;
        if (decodedFrame != nullptr) {
            *decodedFrame = deliveredFrame_.get();
        }
        return SourceReadStatus::Frame;
    }

    if (!opened_) {
        return SourceReadStatus::Error;
    }
    return failed_ ? SourceReadStatus::Error : SourceReadStatus::Waiting;
}

LanStreamMetrics LanStreamSource::metrics() const
{
    std::lock_guard lock(mutex_);
    LanStreamMetrics result;
    result.received = received_;
    result.dropped = dropped_;
    result.delivered = delivered_;
    result.inputFps = inputFps_;
    if (latestDecoded100ns_ > 0) {
        result.readAgeMs = double(std::max<int64_t>(0, monotonic100ns() - latestDecoded100ns_)) / 10000.0;
    }
    return result;
}

void LanStreamSource::close() noexcept
{
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }

    inner_.close();

    std::lock_guard lock(mutex_);
    deliveredFrame_.reset();
    latestFrame_.reset();
    latestPacket_ = {};
    info_ = {};
    opened_ = false;
    failed_ = false;
    latestDecoded100ns_ = 0;
}

} // namespace veyra::source
