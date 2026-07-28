/* libc.so -- <sys/socket.h> and friends.
 *
 * New in 4.5.2, which links Crashlytics and EA's analytics; 1.6 imports none of
 * these and plays perfectly without them. So this port has no network, and the
 * question is only how to SAY so.
 *
 * It says so by failing, immediately and permanently: socket() returns -1 with
 * EAFNOSUPPORT and every call that could only have been reached with a valid
 * descriptor returns -1 with EBADF. The alternative -- pretending a socket was
 * created and then never delivering data -- is the trap this project has been
 * caught by twice (see the FEATURE_UNSUPPORTED note in libopensles.cpp, and the
 * null-jstring one in the DEX layer): code that believes it succeeded goes on to
 * wait for something that will never arrive, and the symptom surfaces far away
 * from the cause. A reporting library that cannot open a socket, by contrast,
 * has a well-trodden error path -- it is what happens on a device in aeroplane
 * mode.
 *
 * Two exceptions, because they are honest work rather than a refusal:
 * inet_addr() is pure string parsing and needs no network at all, and
 * gethostname() has a real answer.
 *
 * If a future version turns out to need a working socket, this is the file to
 * replace -- the failure is confined here and announced in the log.
 */

#include <sprout/dependencies/dependency.h>

#include <cstdio>

namespace sprout {
namespace {

/* Linux/bionic values -- the guest compares against its own headers' numbers,
 * not the host's. */
constexpr std::uint32_t kEBADF = 9;
constexpr std::uint32_t kEAFNOSUPPORT = 97;

constexpr std::uint32_t kMinusOne = (std::uint32_t)-1;

/* Announced once rather than per call: a reporting library retries, and the
 * point is to make "this build has no network" visible in the log exactly once,
 * not to drown it. */
void note_no_network(GuestCall &c, const char *what) {
    static bool said = false;
    if (said) return;
    said = true;
    c.log("[net] %s -- this port has no network stack; sockets fail with EAFNOSUPPORT "
          "(see src/dependencies/libc_socket.cpp)", what);
}

void s_socket(GuestCall &c) {
    note_no_network(c, "socket()");
    c.set_errno(kEAFNOSUPPORT);
    c.set_result(kMinusOne);
}

/* Every one of these needs a descriptor socket() never handed out. */
void s_ebadf(GuestCall &c) {
    c.set_errno(kEBADF);
    c.set_result(kMinusOne);
}

/* select(nfds, readfds, writefds, exceptfds, timeout).
 *
 * 0 -- "the timeout expired and nothing is ready" -- rather than the -1 the
 * calls above return. A caller that gets -1 from select() has hit an error it
 * may well retry immediately in a tight loop; "nothing ready yet" is both the
 * truthful answer when no descriptor exists and the one that leaves normal
 * timeout handling in charge. */
void s_select(GuestCall &c) { c.set_result(0); }

/* in_addr_t inet_addr(const char *cp) -- dotted quad to a NETWORK-order 32-bit
 * address, or INADDR_NONE (0xFFFFFFFF) if it does not parse. No network
 * involved, so this is implemented rather than refused.
 *
 * Deliberately the strict four-part form only: inet_addr historically also
 * accepts "a", "a.b" and "a.b.c" with the trailing part widened, but nothing
 * writes addresses that way today and accepting them silently turns a typo into
 * a wrong address rather than an error. */
void s_inet_addr(GuestCall &c) {
    const std::string text = c.cstr(c.arg(0), 64);
    unsigned parts[4];
    char extra = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3],
                    &extra) != 4) {
        c.set_result(kMinusOne); /* INADDR_NONE */
        return;
    }
    std::uint32_t addr = 0;
    for (int i = 0; i < 4; ++i) {
        if (parts[i] > 255) {
            c.set_result(kMinusOne);
            return;
        }
        /* Network byte order is big-endian, and the guest is little-endian, so
         * the first part ends up in the LOW byte of the returned word. */
        addr |= (std::uint32_t)parts[i] << (8 * i);
    }
    c.set_result(addr);
}

/* int gethostname(char *name, size_t len) */
void s_gethostname(GuestCall &c) {
    const std::uint32_t buf = c.arg(0);
    const std::uint32_t len = c.arg(1);
    static const char kName[] = "localhost";
    if (buf == 0 || len == 0) {
        c.set_result(kMinusOne);
        return;
    }
    const std::size_t n = sizeof(kName) - 1;
    if (len <= n) {
        /* Truncation is an error, and the buffer is left alone. */
        c.set_errno(28 /* ENAMETOOLONG */);
        c.set_result(kMinusOne);
        return;
    }
    c.put_cstr(buf, kName);
    c.set_result(0);
}

}  // namespace

void register_libc_socket(ImportTable &t) {
    t.add("socket", s_socket);

    t.add("accept", s_ebadf);
    t.add("bind", s_ebadf);
    t.add("connect", s_ebadf);
    t.add("getsockname", s_ebadf);
    t.add("listen", s_ebadf);
    t.add("recv", s_ebadf);
    t.add("recvfrom", s_ebadf);
    t.add("send", s_ebadf);
    t.add("sendto", s_ebadf);
    t.add("setsockopt", s_ebadf);
    t.add("shutdown", s_ebadf);

    t.add("select", s_select);
    t.add("inet_addr", s_inet_addr);
    t.add("gethostname", s_gethostname);
}

}  // namespace sprout
