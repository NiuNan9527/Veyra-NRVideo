#include "veyra/media/FFmpegDemuxer.h"

#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <format>
#include <chrono>
#include <thread>

#include "veyra/Log.h"
#include "veyra/media/InputUrl.h"

namespace veyra::media {

FFmpegDemuxer::~FFmpegDemuxer()
{
    close();
}

bool FFmpegDemuxer::open(const std::wstring& path)
{
    if (context_ != nullptr) {
        close();
    }

    // avformat_open_input takes a UTF-8 path; convert from wide first.
    std::string utf8Path;
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
        utf8Path.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), utf8Path.data(), size, nullptr, nullptr);
    }
    AVFormatContext* raw = nullptr;
    AVDictionary* openOptions = nullptr;
    if (isRealtimeNetworkUrl(utf8Path)) {
        // Realtime LAN input: keep demux buffering and probe bounded. The
        // decoded-frame mailbox still owns freshness; these options prevent
        // libavformat from adding a large hidden queue before decode.
        av_dict_set(&openOptions, "fflags", "nobuffer", 0);
        av_dict_set(&openOptions, "probesize", "1048576", 0);
        av_dict_set(&openOptions, "analyzeduration", "500000", 0);
        av_dict_set(&openOptions, "rw_timeout", "2000000", 0);
    }
    const int openResult = avformat_open_input(&raw, utf8Path.c_str(), nullptr, &openOptions);
    av_dict_free(&openOptions);
    if (openResult < 0) {
        char errorText[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(openResult, errorText, sizeof(errorText));
        log::error("media", std::format("demuxer: avformat_open_input failed code={} text={}", openResult, errorText));
        return false;
    }

    if (avformat_find_stream_info(raw, nullptr) < 0) {
        log::error("media", "demuxer: avformat_find_stream_info failed");
        avformat_close_input(&raw);
        return false;
    }

    const int streamIndex = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        log::error("media", "demuxer: no video stream");
        avformat_close_input(&raw);
        return false;
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        avformat_close_input(&raw);
        return false;
    }

    context_ = raw;
    videoStreamIndex_ = streamIndex;
    stats_ = DemuxerStats{};
    const AVStream* stream = raw->streams[streamIndex];
    log::info("media", std::format("demuxer: opened codecId={} streams={} videoStream={} durationUs={} timeBase={}/{}",
        static_cast<int>(stream->codecpar->codec_id), raw->nb_streams, streamIndex,
        stream->duration >= 0 ? stream->duration : raw->duration,
        stream->time_base.num, stream->time_base.den));
    return true;
}

void FFmpegDemuxer::close()
{
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (context_ != nullptr) {
        avformat_close_input(&context_);
    }
    videoStreamIndex_ = -1;
    havePendingPacket_ = false;
}

int64_t FFmpegDemuxer::durationUs() const
{
    if (context_ == nullptr) {
        return 0;
    }
    const AVStream* stream = context_->streams[videoStreamIndex_];
    if (stream->duration > 0 && stream->time_base.den > 0) {
        return av_rescale_q(stream->duration, stream->time_base, { 1, 1000000 });
    }
    return context_->duration; // AV_TIME_BASE units == microseconds
}

double FFmpegDemuxer::averageFps() const
{
    if (context_ == nullptr) {
        return 0.0;
    }
    const AVStream* stream = context_->streams[videoStreamIndex_];
    if (stream->avg_frame_rate.den == 0) {
        return 0.0;
    }
    return static_cast<double>(stream->avg_frame_rate.num) / stream->avg_frame_rate.den;
}

int FFmpegDemuxer::nominalRateNum() const {
    return context_ && videoStreamIndex_ >= 0 ? context_->streams[videoStreamIndex_]->r_frame_rate.num : 0;
}
int FFmpegDemuxer::nominalRateDen() const {
    return context_ && videoStreamIndex_ >= 0 ? context_->streams[videoStreamIndex_]->r_frame_rate.den : 0;
}

const AVCodecParameters* FFmpegDemuxer::videoCodecParameters() const
{
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return nullptr;
    }
    return context_->streams[videoStreamIndex_]->codecpar;
}

int FFmpegDemuxer::videoTimeBaseNum() const
{
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return 0;
    }
    return context_->streams[videoStreamIndex_]->time_base.num;
}

int FFmpegDemuxer::videoTimeBaseDen() const
{
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return 0;
    }
    return context_->streams[videoStreamIndex_]->time_base.den;
}

std::string FFmpegDemuxer::formatName() const
{
    if (context_ == nullptr || context_->iformat == nullptr || context_->iformat->name == nullptr) {
        return {};
    }
    return context_->iformat->name;
}

bool FFmpegDemuxer::readVideoPacket(bool& endOfFile)
{
    endOfFile = false;
    if (context_ == nullptr) {
        return false;
    }
    for (;;) {
        // Explicit ownership: release the previous packet's buffers before
        // reading the next one. av_read_frame would do this implicitly, but
        // the demuxer owns packet_ and makes that release unconditional and
        // observable here (Playbook: every path frees exactly once - the
        // EOF/error paths below receive a blank packet either way).
        av_packet_unref(packet_);
        const int result = av_read_frame(context_, packet_);
        if (result == AVERROR_EOF) {
            endOfFile = true;
            return false;
        }
        if (result == AVERROR(EAGAIN)) {
            // Live network protocols may transiently have no packet ready.
            // Do not convert a harmless would-block into a fatal source error.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (result < 0) {
            char errorText[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(result, errorText, sizeof(errorText));
            log::error("media", std::format("demuxer: av_read_frame failed code={} text={}", result, errorText));
            return false;
        }
        if (packet_->stream_index != videoStreamIndex_) {
            av_packet_unref(packet_);
            continue;
        }
        ++stats_.packetsRead;
        const int64_t ptsUs = av_rescale_q(packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts,
            context_->streams[videoStreamIndex_]->time_base, { 1, 1000000 });
        if (stats_.packetsRead == 1) {
            stats_.firstPts = ptsUs;
        }
        else if (ptsUs < stats_.lastPts) {
            ++stats_.ptsNonMonotonicCount;
        }
        stats_.lastPts = ptsUs;
        return true;
    }
}

bool FFmpegDemuxer::seekToUs(int64_t targetUs)
{
    if (context_ == nullptr) {
        return false;
    }
    const int64_t target = av_rescale_q(targetUs, { 1, 1000000 },
        context_->streams[videoStreamIndex_]->time_base);
    const int result = av_seek_frame(context_, videoStreamIndex_, target, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        char errorText[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, errorText, sizeof(errorText));
        log::error("media", std::format("demuxer: seek failed targetUs={} code={} text={}", targetUs, result, errorText));
        return false;
    }
    return true;
}

} // namespace veyra::media
