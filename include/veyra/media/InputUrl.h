#pragma once

#include <string_view>

namespace veyra::media {

inline bool isRealtimeNetworkUrl(std::wstring_view path)
{
    return path.starts_with(L"tcp://")\n        || path.starts_with(L"udp://")
        || path.starts_with(L"rtp://")
        || path.starts_with(L"srt://");
}

inline bool isRealtimeNetworkUrl(std::string_view path)
{
    return path.starts_with("tcp://")\n        || path.starts_with("udp://")
        || path.starts_with("rtp://")
        || path.starts_with("srt://");
}

} // namespace veyra::media
