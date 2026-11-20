// TITAN Engine — product version constants. Original TITAN code.
#pragma once

namespace titan {
inline constexpr const char* kProductName = "TITAN Engine";
inline constexpr const char* kEngineVersion = "0.6.0-alpha";
// Editor protocol. Bump kProtocolVersion for incompatible changes; servers advertise the
// [kProtocolMin, kProtocolVersion] range they accept.
inline constexpr int kProtocolVersion = 1;
inline constexpr int kProtocolMin = 1;
inline constexpr const char* kProtocolName = "titan.editor";
}  // namespace titan
