/**
 * @file rpc_endpoint.cpp
 * @brief POSIX UDP transport for DCE/RPC (RpcEndpoint).
 */
#include "profinet/rpc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>

namespace profinet
{

namespace
{
[[noreturn]] void fail(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}
} // namespace

RpcEndpoint::RpcEndpoint(const std::uint16_t port, const std::string &bind_ip)
    : server_boot_(static_cast<std::uint32_t>(std::time(nullptr))),
      buf_(65536)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1)
    {
        throw std::invalid_argument("RpcEndpoint: bad bind_ip " + bind_ip);
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
    {
        fail("socket");
    }
    try
    {
        auto try_bind = [&](const std::uint16_t p) {
            addr.sin_port = htons(p);
            return ::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
        };
        bool bound = port != 0 && try_bind(port);
        // ponytail: linear scan of up to 16k ports, once at startup.
        for (std::uint32_t p = k_ephemeral_port_first;
             port == 0 && !bound && p <= k_ephemeral_port_last;
             ++p)
        {
            bound = try_bind(static_cast<std::uint16_t>(p));
            if (!bound && errno != EADDRINUSE)
            {
                break;
            }
        }
        if (!bound)
        {
            fail("bind");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) < 0)
        {
            fail("getsockname");
        }
        port_ = ntohs(addr.sin_port);
    }
    catch (...)
    {
        ::close(fd_);
        throw;
    }
}

RpcEndpoint::~RpcEndpoint()
{
    ::close(fd_);
}

void RpcEndpoint::on(const std::uint16_t opnum, RpcHook hook)
{
    hooks_.push_back({opnum, std::move(hook)});
}

void RpcEndpoint::on_any(RpcHook hook)
{
    hooks_.push_back({-1, std::move(hook)});
}

int RpcEndpoint::poll(const int timeout_ms)
{
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready < 0)
    {
        if (errno == EINTR)
        {
            return 0;
        }
        fail("poll");
    }
    if (ready == 0)
    {
        return 0;
    }

    int handled = 0;
    for (;;)
    {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = ::recvfrom(
            fd_,
            buf_.data(),
            buf_.size(),
            MSG_DONTWAIT,
            reinterpret_cast<sockaddr *>(&from),
            &from_len);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                break;
            }
            fail("recvfrom");
        }
        const auto packet = parse_rpc(Bytes(buf_.data(), static_cast<std::size_t>(n)));
        if (!packet)
        {
            continue;
        }
        Ipv4Endpoint peer;
        std::memcpy(peer.ip.data(), &from.sin_addr, 4);
        peer.port = ntohs(from.sin_port);
        for (const Entry &e : hooks_)
        {
            if (e.opnum < 0 || e.opnum == packet->header.opnum)
            {
                e.hook(*packet, peer);
            }
        }
        ++handled;
    }
    return handled;
}

void RpcEndpoint::run()
{
    running_ = true;
    while (running_)
    {
        poll(100);
    }
}

void RpcEndpoint::stop()
{
    running_ = false;
}

void RpcEndpoint::send(const Ipv4Endpoint &to, const RpcHeader &header, const Bytes body)
{
    // ponytail: no DCE/RPC fragmentation; bodies > ~1400 bytes rely on IP
    // fragmentation. Add fragment_number/k_fragment splitting if a peer rejects that.
    const auto datagram = build_rpc(header, body);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    std::memcpy(&addr.sin_addr, to.ip.data(), 4);
    addr.sin_port = htons(to.port);
    if (::sendto(
            fd_,
            datagram.data(),
            datagram.size(),
            0,
            reinterpret_cast<sockaddr *>(&addr),
            sizeof(addr)) < 0)
    {
        fail("sendto");
    }
}

void RpcEndpoint::reply(const RpcPacket &request, const Ipv4Endpoint &to, const Bytes body)
{
    RpcHeader h = make_response(request.header);
    h.server_boot = server_boot_;
    send(to, h, body);
}

} // namespace profinet
