/* libc.so -- <time.h> and the sleeping primitives.
 *
 * bionic's `struct tm` matches glibc's first nine ints, which is all the engine
 * reads or writes; struct timeval/timespec are two 32-bit words each on
 * armeabi-v7a. Nothing here memcpys a host struct across the boundary -- the
 * host's layouts differ (MinGW's time_t is 64-bit) and doing so silently
 * shifts every field.
 */

#include <sprout/dependencies/dependency.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

namespace sprout {
namespace {

std::uint64_t now_ns() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* ------------------------------------------------------------- the clock */

void c_time(GuestCall &c) {
    std::uint32_t secs = (std::uint32_t)(now_ns() / 1000000000ull);
    if (c.arg(0) != 0) c.write32(c.arg(0), secs);
    c.set_result(secs);
}

void c_gettimeofday(GuestCall &c) {
    std::uint32_t tv = c.arg(0);
    if (tv != 0) {
        std::uint64_t us = now_ns() / 1000ull;
        c.write32(tv + 0, (std::uint32_t)(us / 1000000ull)); /* tv_sec  */
        c.write32(tv + 4, (std::uint32_t)(us % 1000000ull)); /* tv_usec */
    }
    c.set_result(0);
}

void c_clock_gettime(GuestCall &c) {
    std::uint32_t ts = c.arg(1);
    if (ts != 0) {
        std::uint64_t ns = now_ns();
        c.write32(ts + 0, (std::uint32_t)(ns / 1000000000ull)); /* tv_sec  */
        c.write32(ts + 4, (std::uint32_t)(ns % 1000000000ull)); /* tv_nsec */
    }
    c.set_result(0);
}

void c_clock(GuestCall &c) {
    c.set_result((std::uint32_t)std::clock());
}

/* ------------------------------------------------------------ struct tm */

void write_tm(GuestCall &c, std::uint32_t addr, const std::tm &tm) {
    if (addr == 0) return;
    const int fields[9] = {tm.tm_sec,  tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon,
                           tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; ++i) c.write32(addr + (std::uint32_t)i * 4, (std::uint32_t)fields[i]);
}

void read_tm(GuestCall &c, std::uint32_t addr, std::tm &tm) {
    std::memset(&tm, 0, sizeof(tm));
    if (addr == 0) return;
    int *fields[9] = {&tm.tm_sec,  &tm.tm_min,  &tm.tm_hour,
                      &tm.tm_mday, &tm.tm_mon,  &tm.tm_year,
                      &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
    for (int i = 0; i < 9; ++i) *fields[i] = (int)c.read32(addr + (std::uint32_t)i * 4);
}

/* localtime/gmtime/asctime return a pointer to a static buffer, which must live
 * in GUEST memory. One persistent block per function, allocated on first use --
 * matching the "same pointer every call" contract the caller relies on. */
std::uint32_t static_block(GuestCall &c, std::uint32_t &slot, std::uint32_t size) {
    if (slot == 0) slot = c.rt->heap.alloc(size);
    return slot;
}

std::tm host_localtime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::tm host_gmtime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

void c_localtime(GuestCall &c) {
    static std::uint32_t slot = 0;
    std::tm tmv = host_localtime((std::time_t)(std::int32_t)c.read32(c.arg(0)));
    std::uint32_t addr = static_block(c, slot, 9 * 4);
    write_tm(c, addr, tmv);
    c.set_result(addr);
}

void c_localtime_r(GuestCall &c) {
    write_tm(c, c.arg(1), host_localtime((std::time_t)(std::int32_t)c.read32(c.arg(0))));
    c.set_result(c.arg(1));
}

void c_gmtime(GuestCall &c) {
    static std::uint32_t slot = 0;
    std::tm tmv = host_gmtime((std::time_t)(std::int32_t)c.read32(c.arg(0)));
    std::uint32_t addr = static_block(c, slot, 9 * 4);
    write_tm(c, addr, tmv);
    c.set_result(addr);
}

void c_gmtime_r(GuestCall &c) {
    write_tm(c, c.arg(1), host_gmtime((std::time_t)(std::int32_t)c.read32(c.arg(0))));
    c.set_result(c.arg(1));
}

void c_mktime(GuestCall &c) {
    std::tm tmv{};
    read_tm(c, c.arg(0), tmv);
    c.set_result((std::uint32_t)std::mktime(&tmv));
}

void c_difftime(GuestCall &c) {
    c.set_resultd((double)(std::int32_t)c.arg(0) - (double)(std::int32_t)c.arg(1));
}

void c_strftime(GuestCall &c) {
    std::uint32_t dst = c.arg(0), maxsize = c.arg(1);
    std::tm tmv{};
    read_tm(c, c.arg(3), tmv);
    std::string fmt = c.cstr(c.arg(2), 256);
    std::vector<char> out(std::max<std::uint32_t>(maxsize, 1));
    std::size_t n = std::strftime(out.data(), out.size(), fmt.c_str(), &tmv);
    for (std::size_t i = 0; i < n + 1 && i < out.size(); ++i) {
        c.write8(dst + (std::uint32_t)i, (std::uint8_t)out[i]);
    }
    c.set_result((std::uint32_t)n);
}

/* wcsftime writes 4-byte wchar_t units. */
void c_wcsftime(GuestCall &c) {
    std::uint32_t dst = c.arg(0), maxsize = c.arg(1);
    std::tm tmv{};
    read_tm(c, c.arg(3), tmv);
    std::string fmt;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t ch = c.read32(c.arg(2) + i * 4);
        if (ch == 0) break;
        fmt.push_back(ch < 128 ? (char)ch : '?');
    }
    std::vector<char> out(std::max<std::uint32_t>(maxsize, 1));
    std::size_t n = std::strftime(out.data(), out.size(), fmt.c_str(), &tmv);
    for (std::size_t i = 0; i < n + 1 && i < out.size(); ++i) {
        c.write32(dst + (std::uint32_t)i * 4, (std::uint8_t)out[i]);
    }
    c.set_result((std::uint32_t)n);
}

void c_asctime(GuestCall &c) {
    static std::uint32_t slot = 0;
    std::tm tmv{};
    read_tm(c, c.arg(0), tmv);
    char buf[32];
    std::size_t n = std::strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y\n", &tmv);
    std::uint32_t addr = static_block(c, slot, 32);
    for (std::size_t i = 0; i < n + 1; ++i) c.write8(addr + (std::uint32_t)i, (std::uint8_t)buf[i]);
    c.set_result(addr);
}

void c_ctime(GuestCall &c) {
    static std::uint32_t slot = 0;
    std::tm tmv = host_localtime((std::time_t)(std::int32_t)c.read32(c.arg(0)));
    char buf[32];
    std::size_t n = std::strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y\n", &tmv);
    std::uint32_t addr = static_block(c, slot, 32);
    for (std::size_t i = 0; i < n + 1; ++i) c.write8(addr + (std::uint32_t)i, (std::uint8_t)buf[i]);
    c.set_result(addr);
}

/* strptime parses back what strftime produced. The host has no strptime under
 * MinGW, and the engine only ever round-trips its own save timestamps, so the
 * two formats it actually uses are handled directly. */
void c_strptime(GuestCall &c) {
    std::string input = c.cstr(c.arg(0), 128);
    std::string fmt = c.cstr(c.arg(1), 64);
    std::tm tmv{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int matched = 0;
    if (fmt.find("%Y") != std::string::npos && fmt.find("%H") != std::string::npos) {
        matched = std::sscanf(input.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    } else if (fmt.find("%Y") != std::string::npos) {
        matched = std::sscanf(input.c_str(), "%d-%d-%d", &y, &mo, &d);
    }
    if (matched < 3) { c.set_result(0); return; } /* NULL: no conversion */
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    write_tm(c, c.arg(2), tmv);
    c.set_result(c.arg(0) + (std::uint32_t)input.size()); /* end of the consumed text */
}

/* ----------------------------------------------------------------- sleep
 *
 * These really sleep. Returning immediately turns any guest wait loop into a
 * busy spin that burns a whole host core, which is exactly what a background
 * loader thread does between polls. The cap keeps a bad argument from parking a
 * guest thread for minutes. */
constexpr std::uint64_t kMaxSleepMs = 250;

void sleep_ms(std::uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, kMaxSleepMs)));
}

void c_usleep(GuestCall &c) {
    sleep_ms(c.arg(0) / 1000);
    c.set_result(0);
}

void c_sleep(GuestCall &c) {
    sleep_ms((std::uint64_t)c.arg(0) * 1000);
    c.set_result(0);
}

void c_nanosleep(GuestCall &c) {
    std::uint32_t req = c.arg(0);
    if (req != 0) {
        std::uint64_t ms = (std::uint64_t)c.read32(req) * 1000ull + c.read32(req + 4) / 1000000ull;
        sleep_ms(ms);
    }
    c.set_result(0);
}

}  // namespace

void register_libc_time(ImportTable &t) {
    t.add("time", c_time);
    t.add("gettimeofday", c_gettimeofday);
    t.add("clock_gettime", c_clock_gettime);
    t.add("clock", c_clock);

    t.add("localtime", c_localtime);
    t.add("localtime_r", c_localtime_r);
    t.add("gmtime", c_gmtime);
    t.add("gmtime_r", c_gmtime_r);
    t.add("mktime", c_mktime);
    t.add("difftime", c_difftime);
    t.add("strftime", c_strftime);
    t.add("wcsftime", c_wcsftime);
    t.add("asctime", c_asctime);
    t.add("ctime", c_ctime);
    t.add("strptime", c_strptime);

    t.add("usleep", c_usleep);
    t.add("sleep", c_sleep);
    t.add("nanosleep", c_nanosleep);
}

}  // namespace sprout
