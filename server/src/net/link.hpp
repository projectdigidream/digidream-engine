#pragma once
//
// The authoritative server's end of the edge link.
//
// The edge process terminates WebTransport and connects here as a TCP client;
// this side listens. Frames are:
//
//     u8 type | u32 session_id | u32 length | payload
//
// matching server/edge/src/framing.js exactly. This framing is NOT part of the
// wire protocol and is deliberately kept out of core/: it is an internal
// transport detail between two processes we control, so it must not end up
// frozen alongside the format that forked servers have to interoperate with
// (design section 2).
//
#include <cstddef>
#include <functional>
#include <vector>

#include "dnp/math/fixed.hpp"

namespace dnp::net {

using namespace dnp;

enum class LinkFrame : u8 {
    SessionOpen  = 0x01,
    SessionClose = 0x02,
    Control      = 0x03,
    Datagram     = 0x04
};

inline constexpr size_t LINK_HEADER_BYTES = 9;

// Same reasoning as the JS side: an unbounded length prefix turns a corrupt
// frame into an allocation request.
inline constexpr size_t LINK_MAX_PAYLOAD = 1u << 20;

class LinkServer {
public:
    using SessionOpenFn  = std::function<void(u32 session)>;
    using SessionCloseFn = std::function<void(u32 session, u16 reason)>;
    using MessageFn      = std::function<void(u32 session, const u8* data, size_t len)>;

    LinkServer() = default;
    ~LinkServer();

    LinkServer(const LinkServer&) = delete;
    LinkServer& operator=(const LinkServer&) = delete;

    bool listen(u16 port, const char* host = "127.0.0.1");
    void close();

    // Waits up to timeout_ms for socket activity, then drains whatever arrived,
    // invoking the handlers. Returns false if the listening socket has failed.
    bool poll(int timeout_ms);

    bool send_datagram(u32 session, const u8* data, size_t len);
    bool send_control(u32 session, const u8* data, size_t len);
    void close_session(u32 session, u16 reason);

    bool edge_connected() const { return edge_fd_ >= 0; }

    void on_session_open(SessionOpenFn f) { on_open_ = std::move(f); }
    void on_session_close(SessionCloseFn f) { on_close_ = std::move(f); }
    void on_datagram(MessageFn f) { on_datagram_ = std::move(f); }
    void on_control(MessageFn f) { on_control_ = std::move(f); }

private:
    bool send_frame(LinkFrame type, u32 session, const u8* data, size_t len);
    void accept_edge();
    void read_edge();
    void flush_out();
    void drop_edge();

    int listen_fd_ = -1;
    int edge_fd_   = -1;

    std::vector<u8> in_;
    std::vector<u8> out_;

    SessionOpenFn  on_open_;
    SessionCloseFn on_close_;
    MessageFn      on_datagram_;
    MessageFn      on_control_;
};

}  // namespace dnp::net
