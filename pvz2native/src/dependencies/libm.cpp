/* libm.so -- Android's math library.
 *
 * libPVZ2.so imports 25 symbols from here and, until this module existed, 24
 * of them fell through to the "unimplemented, return 0" stub. A game whose
 * sin/cos/sqrt/pow/atan2 all return zero has broken geometry, broken
 * animation and broken timing, and none of it reports an error -- which is
 * precisely the failure mode that made per-symbol firefighting so expensive.
 *
 * ABI is ARM EABI softfp (Android armeabi-v7a): a float travels in one core
 * register, a double in an even-aligned core register pair. GuestCall's
 * argf/argd/set_resultf/set_resultd encapsulate that, so each entry below is
 * just the host libm call.
 */

#include <sprout/dependencies/dependency.h>

#include <cmath>
#include <cstring>

namespace sprout {
namespace {

/* double f(double) */
#define UNARY_D(name, expr)                          \
    void name(GuestCall &c) {                        \
        double x = c.argd(0);                        \
        c.set_resultd(expr);                         \
    }

/* float f(float) */
#define UNARY_F(name, expr)                          \
    void name(GuestCall &c) {                        \
        float x = c.argf(0);                         \
        c.set_resultf(expr);                         \
    }

/* double f(double, double) */
#define BINARY_D(name, expr)                         \
    void name(GuestCall &c) {                        \
        double x = c.argd(0), y = c.argd(2);         \
        c.set_resultd(expr);                         \
    }

/* float f(float, float) */
#define BINARY_F(name, expr)                         \
    void name(GuestCall &c) {                        \
        float x = c.argf(0), y = c.argf(1);          \
        c.set_resultf(expr);                         \
    }

UNARY_D(m_sin, std::sin(x))
UNARY_D(m_cos, std::cos(x))
UNARY_D(m_tan, std::tan(x))
UNARY_D(m_asin, std::asin(x))
UNARY_D(m_acos, std::acos(x))
UNARY_D(m_atan, std::atan(x))
UNARY_D(m_exp, std::exp(x))
UNARY_D(m_log, std::log(x))
UNARY_D(m_log10, std::log10(x))
UNARY_D(m_sqrt, std::sqrt(x))
UNARY_D(m_ceil, std::ceil(x))
UNARY_D(m_floor, std::floor(x))
UNARY_D(m_fabs, std::fabs(x))

UNARY_F(m_sinf, std::sin(x))
UNARY_F(m_cosf, std::cos(x))
UNARY_F(m_tanf, std::tan(x))
UNARY_F(m_asinf, std::asin(x))
UNARY_F(m_acosf, std::acos(x))
UNARY_F(m_sqrtf, std::sqrt(x))
UNARY_F(m_ceilf, std::ceil(x))
UNARY_F(m_floorf, std::floor(x))
UNARY_F(m_fabsf, std::fabs(x))

BINARY_D(m_pow, std::pow(x, y))
BINARY_D(m_fmod, std::fmod(x, y))
BINARY_D(m_atan2, std::atan2(x, y))

BINARY_F(m_powf, std::pow(x, y))
BINARY_F(m_fmodf, std::fmod(x, y))
BINARY_F(m_atan2f, std::atan2(x, y))

/* double modf(double value, double *iptr) -- the integral part goes to the
 * guest pointer, which under softfp is the register after the double pair. */
void m_modf(GuestCall &c) {
    double value = c.argd(0);
    std::uint32_t iptr = c.arg(2);
    double integral = 0.0;
    double frac = std::modf(value, &integral);
    if (iptr != 0 && c.in_bounds(iptr, 8)) {
        std::uint64_t bits;
        std::memcpy(&bits, &integral, 8);
        c.write32(iptr, (std::uint32_t)(bits & 0xFFFFFFFFu));
        c.write32(iptr + 4, (std::uint32_t)(bits >> 32));
    }
    c.set_resultd(frac);
}

/* --- the rest of <math.h>, added for 4.5.2 ---------------------------------
 *
 * 4.5.2 imports 38 more symbols from libm than 1.6 does. Each is one host call,
 * but leaving them out would not have looked like a missing shim: a math
 * function that silently returns 0 produces wrong geometry and wrong timing,
 * never an error message -- the reason this module exists at all. */

UNARY_D(m_sinh, std::sinh(x))
UNARY_D(m_cosh, std::cosh(x))
UNARY_D(m_tanh, std::tanh(x))
UNARY_D(m_asinh, std::asinh(x))
UNARY_D(m_acosh, std::acosh(x))
UNARY_D(m_atanh, std::atanh(x))
UNARY_D(m_cbrt, std::cbrt(x))
UNARY_D(m_erf, std::erf(x))
UNARY_D(m_erfc, std::erfc(x))
UNARY_D(m_exp2, std::exp2(x))
UNARY_D(m_expm1, std::expm1(x))
UNARY_D(m_log1p, std::log1p(x))
UNARY_D(m_logb, std::logb(x))
UNARY_D(m_lgamma, std::lgamma(x))
UNARY_D(m_tgamma, std::tgamma(x))
UNARY_D(m_nearbyint, std::nearbyint(x))
UNARY_D(m_rint, std::rint(x))

UNARY_F(m_atanf, std::atan(x))
UNARY_F(m_log10f, std::log10(x))
UNARY_F(m_roundf, std::round(x))
UNARY_F(m_truncf, std::trunc(x))

BINARY_D(m_hypot, std::hypot(x, y))
BINARY_D(m_nextafter, std::nextafter(x, y))
BINARY_D(m_remainder, std::remainder(x, y))

BINARY_F(m_nextafterf, std::nextafter(x, y))

/* Integer-returning rounders. `long` is 32 bits on ARM32 and `long long` 64,
 * so they differ in whether the result needs r1 as well. */
void m_lrint(GuestCall &c) { c.set_result((std::uint32_t)(long)std::lrint(c.argd(0))); }
void m_llrint(GuestCall &c) { c.set_result64((std::uint64_t)std::llrint(c.argd(0))); }
void m_lroundf(GuestCall &c) { c.set_result((std::uint32_t)(long)std::lroundf(c.argf(0))); }

/* float modff(float value, float *iptr) -- the float variant of m_modf above,
 * so the out-pointer is the very next register rather than the one after a
 * double pair. */
void m_modff(GuestCall &c) {
    float integral = 0.0f;
    const float frac = std::modf(c.argf(0), &integral);
    const std::uint32_t iptr = c.arg(1);
    if (iptr != 0 && c.in_bounds(iptr, 4)) {
        std::uint32_t bits;
        std::memcpy(&bits, &integral, 4);
        c.write32(iptr, bits);
    }
    c.set_resultf(frac);
}

/* double remquo(double x, double y, int *quo) -- two double pairs (r0:r1 and
 * r2:r3), so the pointer has already spilled onto the guest stack. */
void m_remquo(GuestCall &c) {
    int quo = 0;
    const double r = std::remquo(c.argd(0), c.argd(2), &quo);
    const std::uint32_t out = c.arg(4);
    if (out != 0 && c.in_bounds(out, 4)) c.write32(out, (std::uint32_t)quo);
    c.set_resultd(r);
}

/* scalbn(double, int) and scalbnl(long double, int). On ARM AAPCS `long double`
 * IS `double` -- 8 bytes, same representation -- so the two are the same call
 * with the same argument layout, not a wider one. */
void m_scalbn(GuestCall &c) { c.set_resultd(std::scalbn(c.argd(0), (int)c.arg(2))); }

/* --- classification --------------------------------------------------------
 *
 * These take their FP_* return values from BIONIC, not from the host. bionic
 * (like the BSD libm it descends from) uses 0x01/0x02/0x04/0x08/0x10, while
 * glibc and MinGW each use their own unrelated numbering -- so forwarding the
 * host's std::fpclassify result directly would compile cleanly and then make
 * the guest's `isnan`-by-classification tests answer wrongly. */
constexpr std::uint32_t kBionicFpInfinite = 0x01;
constexpr std::uint32_t kBionicFpNan = 0x02;
constexpr std::uint32_t kBionicFpNormal = 0x04;
constexpr std::uint32_t kBionicFpSubnormal = 0x08;
constexpr std::uint32_t kBionicFpZero = 0x10;

void m_fpclassifyd(GuestCall &c) {
    const double x = c.argd(0);
    std::uint32_t r = kBionicFpNormal;
    switch (std::fpclassify(x)) {
        case FP_INFINITE: r = kBionicFpInfinite; break;
        case FP_NAN: r = kBionicFpNan; break;
        case FP_SUBNORMAL: r = kBionicFpSubnormal; break;
        case FP_ZERO: r = kBionicFpZero; break;
        default: r = kBionicFpNormal; break;
    }
    c.set_result(r);
}

void m_isfinite(GuestCall &c) { c.set_result(std::isfinite(c.argd(0)) ? 1u : 0u); }
void m_isinf(GuestCall &c) { c.set_result(std::isinf(c.argd(0)) ? 1u : 0u); }
void m_isnan(GuestCall &c) { c.set_result(std::isnan(c.argd(0)) ? 1u : 0u); }
void m_signbit(GuestCall &c) { c.set_result(std::signbit(c.argd(0)) ? 1u : 0u); }
void m_signbitf(GuestCall &c) { c.set_result(std::signbit(c.argf(0)) ? 1u : 0u); }

#undef UNARY_D
#undef UNARY_F
#undef BINARY_D
#undef BINARY_F

}  // namespace

void register_libm(ImportTable &t) {
    t.add("sin", m_sin);
    t.add("cos", m_cos);
    t.add("tan", m_tan);
    t.add("asin", m_asin);
    t.add("acos", m_acos);
    t.add("atan", m_atan);
    t.add("exp", m_exp);
    t.add("log", m_log);
    t.add("log10", m_log10);
    t.add("sqrt", m_sqrt);
    t.add("ceil", m_ceil);
    t.add("floor", m_floor);
    t.add("fabs", m_fabs);
    t.add("pow", m_pow);
    t.add("fmod", m_fmod);
    t.add("atan2", m_atan2);
    t.add("modf", m_modf);

    t.add("sinf", m_sinf);
    t.add("cosf", m_cosf);
    t.add("tanf", m_tanf);
    t.add("asinf", m_asinf);
    t.add("acosf", m_acosf);
    t.add("sqrtf", m_sqrtf);
    t.add("ceilf", m_ceilf);
    t.add("floorf", m_floorf);
    t.add("fabsf", m_fabsf);
    t.add("powf", m_powf);
    t.add("fmodf", m_fmodf);
    t.add("atan2f", m_atan2f);

    /* Imported by 4.5.2 but not by 1.6. */
    t.add("sinh", m_sinh);
    t.add("cosh", m_cosh);
    t.add("tanh", m_tanh);
    t.add("asinh", m_asinh);
    t.add("acosh", m_acosh);
    t.add("atanh", m_atanh);
    t.add("cbrt", m_cbrt);
    t.add("erf", m_erf);
    t.add("erfc", m_erfc);
    t.add("exp2", m_exp2);
    t.add("expm1", m_expm1);
    t.add("log1p", m_log1p);
    t.add("logb", m_logb);
    t.add("lgamma", m_lgamma);
    t.add("tgamma", m_tgamma);
    t.add("nearbyint", m_nearbyint);
    t.add("rint", m_rint);
    t.add("hypot", m_hypot);
    t.add("nextafter", m_nextafter);
    t.add("remainder", m_remainder);
    t.add("remquo", m_remquo);
    t.add("scalbn", m_scalbn);
    t.add("scalbnl", m_scalbn); /* long double == double on ARM AAPCS */

    t.add("atanf", m_atanf);
    t.add("log10f", m_log10f);
    t.add("roundf", m_roundf);
    t.add("truncf", m_truncf);
    t.add("modff", m_modff);
    t.add("nextafterf", m_nextafterf);

    t.add("lrint", m_lrint);
    t.add("llrint", m_llrint);
    t.add("lroundf", m_lroundf);

    t.add("__fpclassifyd", m_fpclassifyd);
    t.add("__isfinite", m_isfinite);
    t.add("__isinf", m_isinf);
    t.add("__signbit", m_signbit);
    t.add("__signbitf", m_signbitf);
    t.add("isnan", m_isnan);
}

}  // namespace sprout
