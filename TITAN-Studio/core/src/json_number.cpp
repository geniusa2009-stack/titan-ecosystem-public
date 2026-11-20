// TITAN JSON number parsing (see titan/detail/json_number.h). Original TITAN code.
#include "titan/detail/json_number.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#if TITAN_HAS_STRTOD_L_FALLBACK
#  if defined(__APPLE__)
#    include <xlocale.h>
#  else
#    include <locale.h>
#  endif
#endif

namespace titan::detail {

namespace {
// One accepted syntax for both implementations: [-]digits[.digits][(e|E)[+-]digits] (at least one digit).
// std::from_chars would also accept inf/infinity/nan, and strtod leading spaces, '+' and hex floats.
bool generalSyntax(const char* p, const char* last) {
    if (p < last && *p == '-') ++p;
    const char* m = p;
    while (p < last && *p >= '0' && *p <= '9') ++p;
    bool digits = p != m;
    if (p < last && *p == '.') {
        ++p;
        const char* f = p;
        while (p < last && *p >= '0' && *p <= '9') ++p;
        digits = digits || p != f;
    }
    if (!digits) return false;
    if (p < last && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < last && (*p == '+' || *p == '-')) ++p;
        const char* e = p;
        while (p < last && *p >= '0' && *p <= '9') ++p;
        if (p == e) return false;
    }
    return p == last;
}
}  // namespace

#if TITAN_HAS_FLOAT_FROM_CHARS
NumberParse parseJsonDoubleFromChars(const char* first, const char* last, double& out) {
    if (!generalSyntax(first, last)) return NumberParse::Invalid;
    double d = 0;
    const auto r = std::from_chars(first, last, d, std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) return NumberParse::OutOfRange;
    if (r.ec != std::errc() || r.ptr != last) return NumberParse::Invalid;
    out = d;
    return NumberParse::Ok;
}
#endif

#if TITAN_HAS_STRTOD_L_FALLBACK
namespace {
locale_t cLocale() {
    static const locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return loc;
}

}  // namespace

NumberParse parseJsonDoubleStrtod(const char* first, const char* last, double& out) {
    if (!generalSyntax(first, last)) return NumberParse::Invalid;
    const locale_t loc = cLocale();
    if (loc == static_cast<locale_t>(nullptr)) return NumberParse::Invalid;
    // strtod_l needs a terminated string; JSON numbers are almost always short.
    char small[96];
    std::string big;
    const size_t n = size_t(last - first);
    const char* text;
    if (n < sizeof small) {
        std::memcpy(small, first, n);
        small[n] = '\0';
        text = small;
    } else {
        big.assign(first, n);
        text = big.c_str();
    }
    const int savedErrno = errno;
    errno = 0;
    char* end = nullptr;
    const double d = strtod_l(text, &end, loc);
    const int err = errno;
    errno = savedErrno;
    if (end != text + n) return NumberParse::Invalid;
    // strtod also sets ERANGE for subnormal results, which std::from_chars accepts: only an infinite result
    // (overflow) or zero from a non-zero input (underflow) is out of range.
    if (std::isinf(d) || (d == 0 && err == ERANGE)) return NumberParse::OutOfRange;
    out = d;
    return NumberParse::Ok;
}
#endif

NumberParse parseJsonDouble(const char* first, const char* last, double& out) {
#if TITAN_HAS_FLOAT_FROM_CHARS
    return parseJsonDoubleFromChars(first, last, out);
#elif TITAN_HAS_STRTOD_L_FALLBACK
    return parseJsonDoubleStrtod(first, last, out);
#else
#  error "no locale-independent floating-point parser available for this platform"
#endif
}

const char* jsonDoubleParserName() {
#if TITAN_HAS_FLOAT_FROM_CHARS
    return "from_chars";
#else
    return "strtod_l";
#endif
}

}  // namespace titan::detail
