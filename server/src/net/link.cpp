#include "net/link.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

// THE ONE PLATFORM SEAM in the link: BSD sockets everywhere but Windows, which
// has Winsock -- the same calls under other names, error codes from
// WSAGetLastError() rather than errno, and WSAPoll for poll. Everything below
// the shim is one code path. Socket handles are kept as int, as they always
// were here: a Winsock SOCKET is a small kernel handle in practice, and
// INVALID_SOCKET converts to -1, which is what the checks below test.
#ifndef __EMSCRIPTEN__   // the web build has no socket: see the end of this file
#ifdef _WIN32
// WSAPoll and inet_pton exist from Windows Vista; MinGW declares them only when
// told the target is at least that new. Windows 10 is the floor anyway.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace dnp::net {
namespace {

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
using sock_poll_t = WSAPOLLFD;
int  sock_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
int  sock_last() { return ::WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
bool interrupted(int e) { return e == WSAEINTR; }
int  sock_poll(sock_poll_t* f, int n, int ms) { return ::WSAPoll(f, static_cast<ULONG>(n), ms); }
bool set_nonblocking(int fd) {
    u_long on = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on) == 0;
}
const char* sock_error(int e) {
    static char buf[48];
    std::snprintf(buf, sizeof(buf), "winsock error %d", e);
    return buf;
}
// Winsock must be started once per process before the first socket call.
bool sock_startup() {
    static const bool ok = [] { WSADATA d; return ::WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
#else
using sock_poll_t = pollfd;
int  sock_close(int fd) { return ::close(fd); }
int  sock_last() { return errno; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool interrupted(int e) { return e == EINTR; }
int  sock_poll(sock_poll_t* f, int n, int ms) { return ::poll(f, static_cast<nfds_t>(n), ms); }
bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
const char* sock_error(int e) { return std::strerror(e); }
bool sock_startup() { return true; }
#endif

sock_poll_t poll_entry(int fd, short events) {
    sock_poll_t p{};
    p.fd = fd;
    p.events = events;
    return p;
}

// setsockopt's value is `const char*` under Winsock and `const void*` elsewhere;
// a char pointer converts to both.
void set_int_option(int fd, int level, int name, int value) {
    ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}
#endif  // !__EMSCRIPTEN__

u32 read_u32_le(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

void write_u32_le(u8* p, u32 v) {
    p[0] = static_cast<u8>(v & 0xFF);
    p[1] = static_cast<u8>((v >> 8) & 0xFF);
    p[2] = static_cast<u8>((v >> 16) & 0xFF);
    p[3] = static_cast<u8>((v >> 24) & 0xFF);
}

}  // namespace

LinkServer::~LinkServer() { close(); }

#ifndef __EMSCRIPTEN__
bool LinkServer::listen(u16 port, const char* host) {
    if (!sock_startup()) return false;
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) return false;

    set_int_option(listen_fd_, SOL_SOCKET, SO_REUSEADDR, 1);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(); return false; }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[link] bind %s:%u failed: %s\n", host, port, sock_error(sock_last()));
        close();
        return false;
    }
    if (::listen(listen_fd_, 4) != 0) { close(); return false; }
    if (!set_nonblocking(listen_fd_)) { close(); return false; }

    in_.reserve(1 << 16);
    out_.reserve(1 << 16);
    return true;
}

void LinkServer::close() {
    drop_edge();
    if (listen_fd_ >= 0) { sock_close(listen_fd_); listen_fd_ = -1; }
}

void LinkServer::drop_edge() {
    if (edge_fd_ >= 0) { sock_close(edge_fd_); edge_fd_ = -1; }
    in_.clear();
    out_.clear();
}

void LinkServer::accept_edge() {
    const int fd = static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
    if (fd < 0) return;

    if (edge_fd_ >= 0) {
        // One edge at a time. A second connection is more likely a stale process
        // than a legitimate peer, and silently serving both would interleave two
        // sessions' frames onto one id space.
        //
        // SAY SO. Closing in silence is correct behaviour with a terrible
        // symptom: the rejected edge sees the TCP handshake succeed and the
        // socket close at once, so it logs "upstream connected" then "upstream
        // lost" and retries every half second, for ever. That reads exactly
        // like a server that keeps crashing -- and the server is fine, and the
        // FIRST edge is still playing on it. It has cost a debugging session
        // more than once, so the rejection is now audible from the only process
        // that knows the reason.
        //
        // Rate-limited: a spinning edge produces two of these a second, and a
        // log that scrolls is a log nobody reads.
        ++rejected_edges_;
        if (rejected_edges_ == 1 || rejected_edges_ % 20 == 0) {
            std::fprintf(stderr,
                "[link] refused a second edge (%llu so far): one edge at a time, "
                "and one is already connected. Another stack is probably running "
                "on this port -- see scripts/run_stack.sh.\n",
                static_cast<unsigned long long>(rejected_edges_));
        }
        sock_close(fd);
        return;
    }

    // Every frame here is small and latency-critical, so Nagle's batching is
    // exactly the wrong trade: it would hold a snapshot for up to 40 ms to save
    // a header, adding more delay than the network path it sits in front of.
    set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    set_nonblocking(fd);
    edge_fd_ = fd;
    std::fprintf(stderr, "[link] edge connected\n");
}

void LinkServer::read_edge() {
    u8 buf[65536];
    for (;;) {
        const long n = ::recv(edge_fd_, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
        if (n > 0) {
            in_.insert(in_.end(), buf, buf + n);
            if (static_cast<size_t>(n) < sizeof(buf)) break;
            continue;
        }
        if (n == 0) { std::fprintf(stderr, "[link] edge disconnected\n"); drop_edge(); return; }
        const int e = sock_last();
        if (would_block(e)) break;
        if (interrupted(e)) continue;
        std::fprintf(stderr, "[link] recv failed: %s\n", sock_error(e));
        drop_edge();
        return;
    }

    parse_in();
}

#endif  // !__EMSCRIPTEN__

void LinkServer::parse_in() {
    // Drain complete frames. A TCP read boundary is not a frame boundary, so
    // this loop is the only thing standing between us and a parser that works
    // on loopback and fails under fragmentation.
    size_t off = 0;
    while (in_.size() - off >= LINK_HEADER_BYTES) {
        const u8* p = in_.data() + off;
        const u8  type = p[0];
        const u32 session = read_u32_le(p + 1);
        const u32 len = read_u32_le(p + 5);

        if (len > LINK_MAX_PAYLOAD) {
            std::fprintf(stderr, "[link] frame length %u exceeds cap; dropping edge\n", len);
            drop_edge();
            return;
        }
        if (in_.size() - off < LINK_HEADER_BYTES + len) break;

        const u8* payload = p + LINK_HEADER_BYTES;
        switch (static_cast<LinkFrame>(type)) {
            case LinkFrame::SessionOpen:
                if (on_open_) on_open_(session);
                break;
            case LinkFrame::SessionClose:
                if (on_close_) on_close_(session, len >= 2 ? static_cast<u16>(payload[0] | (payload[1] << 8)) : 0);
                break;
            case LinkFrame::Datagram:
                if (on_datagram_) on_datagram_(session, payload, len);
                break;
            case LinkFrame::Control:
                if (on_control_) on_control_(session, payload, len);
                break;
            default:
                // Unknown frame types are skipped, not fatal: it is how a newer
                // edge stays usable against an older server.
                break;
        }
        off += LINK_HEADER_BYTES + len;
    }

    if (off > 0) in_.erase(in_.begin(), in_.begin() + static_cast<long>(off));
}

#ifndef __EMSCRIPTEN__
void LinkServer::flush_out() {
    size_t sent = 0;
    while (sent < out_.size()) {
        const long n = ::send(edge_fd_, reinterpret_cast<const char*>(out_.data() + sent),
                              static_cast<int>(out_.size() - sent), 0);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        const int e = n < 0 ? sock_last() : 0;
        if (n < 0 && would_block(e)) break;
        if (n < 0 && interrupted(e)) continue;
        drop_edge();
        return;
    }
    if (sent > 0) out_.erase(out_.begin(), out_.begin() + static_cast<long>(sent));
}

#endif  // !__EMSCRIPTEN__

bool LinkServer::send_frame(LinkFrame type, u32 session, const u8* data, size_t len) {
    if (edge_fd_ < 0 || len > LINK_MAX_PAYLOAD) return false;

    // A backed-up output buffer means the edge is not draining. Dropping the
    // oldest queued bytes would corrupt a frame mid-stream, so the whole link
    // goes instead -- the edge reconnects and sessions are re-established, which
    // is recoverable in a way a desynchronised byte stream is not.
    if (out_.size() > (4u << 20)) {
        std::fprintf(stderr, "[link] output backlog exceeded; dropping edge\n");
        drop_edge();
        return false;
    }

    u8 header[LINK_HEADER_BYTES];
    header[0] = static_cast<u8>(type);
    write_u32_le(header + 1, session);
    write_u32_le(header + 5, static_cast<u32>(len));
    out_.insert(out_.end(), header, header + LINK_HEADER_BYTES);
    if (len > 0) out_.insert(out_.end(), data, data + len);
    return true;
}

bool LinkServer::send_datagram(u32 session, const u8* data, size_t len) {
    return send_frame(LinkFrame::Datagram, session, data, len);
}

bool LinkServer::send_control(u32 session, const u8* data, size_t len) {
    return send_frame(LinkFrame::Control, session, data, len);
}

void LinkServer::close_session(u32 session, u16 reason) {
    const u8 body[2] = {static_cast<u8>(reason & 0xFF), static_cast<u8>((reason >> 8) & 0xFF)};
    send_frame(LinkFrame::SessionClose, session, body, sizeof(body));
}

#ifndef __EMSCRIPTEN__
bool LinkServer::poll(int timeout_ms) {
    if (listen_fd_ < 0) return false;

    sock_poll_t fds[2];
    int n = 0;
    fds[n++] = poll_entry(listen_fd_, POLLIN);
    if (edge_fd_ >= 0) {
        short events = POLLIN;
        if (!out_.empty()) events = static_cast<short>(events | POLLOUT);
        fds[n++] = poll_entry(edge_fd_, events);
    }

    const int rc = sock_poll(fds, n, timeout_ms);
    if (rc < 0) return interrupted(sock_last());
    if (rc == 0) return true;

    if (fds[0].revents & POLLIN) accept_edge();

    if (n > 1 && edge_fd_ >= 0) {
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { drop_edge(); return true; }
        if (fds[1].revents & POLLOUT) flush_out();
        if (edge_fd_ >= 0 && (fds[1].revents & POLLIN)) read_edge();
    }
    return true;
}

#endif  // !__EMSCRIPTEN__

// THE WEB BUILD (docs/WEB.md): no socket. The page is the edge, connected from
// the start; its link bytes come in through feed() and go out through take(),
// moved by the Worker that hosts this server, and poll() parses what came in.
#ifdef __EMSCRIPTEN__
bool LinkServer::listen(u16, const char*) {
    listen_fd_ = 0;
    edge_fd_ = 0;
    in_.reserve(1 << 16);
    out_.reserve(1 << 16);
    return true;
}

void LinkServer::close() { drop_edge(); listen_fd_ = -1; }

void LinkServer::drop_edge() {
    if (edge_fd_ >= 0) std::fprintf(stderr, "[link] the page's link dropped\n");
    edge_fd_ = -1;
    in_.clear();
    out_.clear();
}

bool LinkServer::poll(int) {
    if (listen_fd_ < 0) return false;
    if (edge_fd_ >= 0) parse_in();
    return true;
}
#endif

void LinkServer::feed(const u8* data, size_t len) {
    if (edge_fd_ >= 0) in_.insert(in_.end(), data, data + len);
}

size_t LinkServer::take(u8* out, size_t cap) {
    const size_t n = out_.size() < cap ? out_.size() : cap;
    if (n == 0) return 0;
    std::memcpy(out, out_.data(), n);
    out_.erase(out_.begin(), out_.begin() + static_cast<long>(n));
    return n;
}

}  // namespace dnp::net
