#pragma once

#include "veyra/source/IFrameSource.h"
#include "veyra/source/MediaFileSource.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

struct AVFrame;

namespace veyra::source {

struct LanStreamMetrics {
    uint64_t received = 0;
    uint64_t dropped = 0;
    uint64_t delivered = 0;
    double inputFps = 0.0;
    double readAgeMs = 0.0;
};

class LanStreamSource final : public IFrameSource {
public:
    LanStreamSource() = default;
    ~LanStreamSource() override;

    LanStreamSource(const LanStreamSource&) = delete;
    LanStreamSource& operator=(const LanStreamSource&) = delete;

    bool open(const SourceOpenDesc& desc) override;
    const SourceInfo& info() const override { return info_; }
    SourceReadStatus read(pipeline::FramePacket& out, const AVFrame** decodedFrame) override;
    bool seek(const pipeline::Rational&) override { return false; }
    void close() noexcept override;

    LanStreamMetrics metrics() const;

private:
    void pump() noexcept;

    MediaFileSource inner_;
    SourceInfo info_{};

    std::atomic<bool> stop_{false};
    std::thread worker_;

    mutable std::mutex mutex_;
    std::shared_ptr<AVFrame> latestFrame_;
    std::shared_ptr<AVFrame> deliveredFrame_;
    pipeline::FramePacket latestPacket_{};
    bool opened_ = false;
    bool failed_ = false;
    uint64_t received_ = 0;
    uint64_t dropped_ = 0;
    uint64_t delivered_ = 0;
    int64_t rateWindowStart100ns_ = 0;
    uint64_t rateWindowFrames_ = 0;
    double inputFps_ = 0.0;
    int64_t latestDecoded100ns_ = 0;
};

} // namespace veyra::source
