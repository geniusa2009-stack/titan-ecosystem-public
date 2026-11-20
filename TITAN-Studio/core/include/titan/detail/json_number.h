// TITAN JSON number parsing: locale-independent text -> double. Original TITAN code.
//
// The JSON parser validates the number grammar first and then calls parseJsonDouble() on exactly the
// characters of one number. Two implementations exist:
//   * std::from_chars (correctly rounded, locale-independent). Apple's libc++ provides the floating-point
//     overload only from macOS 26.0, so it is used when the deployment target allows it, and on other
//     standard libraries that implement it.
//   * strtod_l with the "C" locale (POSIX; correctly rounded in Apple's libc and glibc), for older
//     deployment targets. tests/test_core.cpp checks it bit-for-bit against std::from_chars.
// Both report the same results: the nearest double (round-half-to-even), subnormals accepted, and
// out-of-range for overflow (result would be infinite) or underflow to zero from a non-zero input. Both accept
// only [-]digits[.digits][(e|E)[+-]digits]; inf, nan, hex, '+' and surrounding spaces are invalid.
#pragma once

#include <charconv>
#include <version>

#if defined(__APPLE__)
#  if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
#    define TITAN_HAS_FLOAT_FROM_CHARS 1
#  else
#    define TITAN_HAS_FLOAT_FROM_CHARS 0
#  endif
#elif defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#  define TITAN_HAS_FLOAT_FROM_CHARS 1
#else
#  define TITAN_HAS_FLOAT_FROM_CHARS 0
#endif

#if !defined(_WIN32)
#  define TITAN_HAS_STRTOD_L_FALLBACK 1
#else
#  define TITAN_HAS_STRTOD_L_FALLBACK 0
#endif

namespace titan::detail {

enum class NumberParse { Ok, Invalid, OutOfRange };

// The implementation the JSON parser uses in this build.
NumberParse parseJsonDouble(const char* first, const char* last, double& out);
const char* jsonDoubleParserName();  // "from_chars" or "strtod_l"

#if TITAN_HAS_STRTOD_L_FALLBACK
// strtod_l("C") implementation; always compiled on POSIX so tests can compare it with std::from_chars.
NumberParse parseJsonDoubleStrtod(const char* first, const char* last, double& out);
#endif

#if TITAN_HAS_FLOAT_FROM_CHARS
NumberParse parseJsonDoubleFromChars(const char* first, const char* last, double& out);
#endif

}  // namespace titan::detail
