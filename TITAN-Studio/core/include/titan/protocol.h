// TITAN Engine — editor protocol framing and message helpers. Original TITAN code.
//
// Wire format: each message is a 4-byte big-endian payload length followed by a UTF-8
// JSON document. Messages:
//   request : {"id": <uint>, "method": "<name>", "params": {...}}
//   response: {"id": <uint>, "result": {...}}  or  {"id": <uint>, "error": {"code", "message"}}
//   event   : {"event": "<name>", "data": {...}}
// The full method/event catalogue lives in docs/EDITOR_INTEGRATION.md.
#pragma once

#include "titan/json.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace titan::protocol {

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

enum ErrorCode : int {
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternalError = -32603,
    kNotNegotiated = 1000,
    kNoProject = 1001,
    kNotFound = 1002,
    kIoError = 1003,
    kVersionMismatch = 1004,
    kExtensionError = 1005,
    kConflict = 1006,
    kUnsupported = 1007,  // the method belongs to a capability this backend does not advertise
};

std::string encodeFrame(std::string_view payload);
inline std::string encodeFrame(const Json& message) { return encodeFrame(std::string_view(message.dump())); }

// Incremental decoder: feed raw bytes, pop complete payloads. A frame larger than
// kMaxFrameBytes puts the decoder in a failed state (the connection must be closed).
class FrameDecoder {
public:
    void feed(const char* data, size_t len);
    std::optional<std::string> next();
    bool failed() const { return failed_; }

private:
    std::string buffer_;
    size_t offset_ = 0;
    bool failed_ = false;
};

Json makeRequest(uint64_t id, const std::string& method, Json params = Json::object());
Json makeResult(const Json& id, Json result);
Json makeError(const Json& id, int code, const std::string& message);
Json makeEvent(const std::string& name, Json data);

}  // namespace titan::protocol
