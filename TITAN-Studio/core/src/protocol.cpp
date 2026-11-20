// TITAN Engine — protocol framing. Original TITAN code.
#include "titan/protocol.h"

namespace titan::protocol {

std::string encodeFrame(std::string_view payload) {
    if (payload.size() > kMaxFrameBytes) throw std::length_error("protocol frame too large");
    const auto n = static_cast<uint32_t>(payload.size());
    std::string out;
    out.reserve(4 + payload.size());
    out += static_cast<char>((n >> 24) & 0xFF);
    out += static_cast<char>((n >> 16) & 0xFF);
    out += static_cast<char>((n >> 8) & 0xFF);
    out += static_cast<char>(n & 0xFF);
    out.append(payload);
    return out;
}

void FrameDecoder::feed(const char* data, size_t len) {
    if (failed_) return;
    // Compact consumed bytes occasionally so the buffer does not grow without bound.
    if (offset_ > 0 && offset_ * 2 > buffer_.size()) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    buffer_.append(data, len);
}

std::optional<std::string> FrameDecoder::next() {
    if (failed_ || buffer_.size() - offset_ < 4) return std::nullopt;
    const auto* p = reinterpret_cast<const unsigned char*>(buffer_.data() + offset_);
    uint32_t n = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    if (n > kMaxFrameBytes) {
        failed_ = true;
        return std::nullopt;
    }
    if (buffer_.size() - offset_ < 4 + size_t(n)) return std::nullopt;
    std::string payload = buffer_.substr(offset_ + 4, n);
    offset_ += 4 + n;
    return payload;
}

Json makeRequest(uint64_t id, const std::string& method, Json params) {
    Json j = Json::object();
    j["id"] = id;
    j["method"] = method;
    j["params"] = std::move(params);
    return j;
}

Json makeResult(const Json& id, Json result) {
    Json j = Json::object();
    j["id"] = id;
    j["result"] = std::move(result);
    return j;
}

Json makeError(const Json& id, int code, const std::string& message) {
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    Json j = Json::object();
    j["id"] = id;
    j["error"] = e;
    return j;
}

Json makeEvent(const std::string& name, Json data) {
    Json j = Json::object();
    j["event"] = name;
    j["data"] = std::move(data);
    return j;
}

}  // namespace titan::protocol
