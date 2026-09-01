#include "net/link.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dnp::net {
namespace {

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

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

bool LinkServer::listen(u16 port, const char* host) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(); return false; }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[link] bind %s:%u failed: %s\n", host, port, std::strerror(errno));
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
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

void LinkServer::drop_edge() {
    if (edge_fd_ >= 0) { ::close(edge_fd_); edge_fd_ = -1; }
    in_.clear();
    out_.clear();
}

void LinkServer::accept_edge() {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
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
        ::close(fd);
        return;
    }

    int yes = 1;
    // Every frame here is small and latency-critical, so Nagle's batching is
    // exactly the wrong trade: it would hold a snapshot for up to 40 ms to save
    // a header, adding more delay than the network path it sits in front of.
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    set_nonblocking(fd);
    edge_fd_ = fd;
    std::fprintf(stderr, "[link] edge connected\n");
}

void LinkServer::read_edge() {
    u8 buf[65536];
    for (;;) {
        const ssize_t n = ::recv(edge_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            in_.insert(in_.end(), buf, buf + n);
            if (static_cast<size_t>(n) < sizeof(buf)) break;
            continue;
        }
        if (n == 0) { std::fprintf(stderr, "[link] edge disconnected\n"); drop_edge(); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        std::fprintf(stderr, "[link] recv failed: %s\n", std::strerror(errno));
        drop_edge();
        return;
    }

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

void LinkServer::flush_out() {
    size_t sent = 0;
    while (sent < out_.size()) {
        const ssize_t n = ::send(edge_fd_, out_.data() + sent, out_.size() - sent, 0);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        drop_edge();
        return;
    }
    if (sent > 0) out_.erase(out_.begin(), out_.begin() + static_cast<long>(sent));
}

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

bool LinkServer::poll(int timeout_ms) {
    if (listen_fd_ < 0) return false;

    pollfd fds[2];
    int n = 0;
    fds[n++] = pollfd{listen_fd_, POLLIN, 0};
    if (edge_fd_ >= 0) {
        short events = POLLIN;
        if (!out_.empty()) events = static_cast<short>(events | POLLOUT);
        fds[n++] = pollfd{edge_fd_, events, 0};
    }

    const int rc = ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
    if (rc < 0) return errno == EINTR;
    if (rc == 0) return true;

    if (fds[0].revents & POLLIN) accept_edge();

    if (n > 1 && edge_fd_ >= 0) {
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { drop_edge(); return true; }
        if (fds[1].revents & POLLOUT) flush_out();
        if (edge_fd_ >= 0 && (fds[1].revents & POLLIN)) read_edge();
    }
    return true;
}

}  // namespace dnp::net
