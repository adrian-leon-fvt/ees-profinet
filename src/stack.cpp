/**
 * @file stack.cpp
 * @brief Linux AF_PACKET implementation of profinet::Stack.
 */
#include "profinet/stack.h"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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

Stack::Stack(const std::string &interface_name)
{
    // ponytail: ETH_P_ALL so in-band VLAN-tagged frames are seen too; parse()
    // drops non-PROFINET. Attach a BPF filter if CPU load on busy links matters.
    fd_ = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd_ < 0)
    {
        fail("socket");
    }
    try
    {
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0)
        {
            fail("SIOCGIFINDEX");
        }
        ifindex_ = ifr.ifr_ifindex;
        if (::ioctl(fd_, SIOCGIFHWADDR, &ifr) < 0)
        {
            fail("SIOCGIFHWADDR");
        }
        std::memcpy(mac_.data(), ifr.ifr_hwaddr.sa_data, mac_.size());

        // Recover VLAN tags stripped by NIC offload.
        const int on = 1;
        if (::setsockopt(fd_, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on)) < 0)
        {
            fail("PACKET_AUXDATA");
        }

        sockaddr_ll sll{};
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = ifindex_;
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0)
        {
            fail("bind");
        }
        join_multicast(k_multicast_dcp);
        join_multicast(k_multicast_lldp);
    }
    catch (...)
    {
        ::close(fd_);
        throw;
    }
}

Stack::Stack(const int fd, const Mac &mac, const int ifindex)
    : fd_(fd), ifindex_(ifindex), mac_(mac)
{
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    connected_ = ::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0 &&
                 addr.ss_family != AF_PACKET;
}

Stack::~Stack()
{
    ::close(fd_);
}

void Stack::join_multicast(const Mac &group)
{
    packet_mreq mr{};
    mr.mr_ifindex = ifindex_;
    mr.mr_type = PACKET_MR_MULTICAST;
    mr.mr_alen = 6;
    std::copy(group.begin(), group.end(), mr.mr_address);
    if (::setsockopt(fd_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
    {
        fail("PACKET_ADD_MEMBERSHIP");
    }
}

int Stack::poll(const int timeout_ms)
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
    std::uint8_t buf[2048];
    alignas(cmsghdr) std::uint8_t ctrl[CMSG_SPACE(sizeof(tpacket_auxdata))];
    for (;;)
    {
        sockaddr_ll from{};
        iovec iov{buf, sizeof(buf)};
        msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        const ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                break;
            }
            fail("recvmsg");
        }
        if (from.sll_pkttype == PACKET_OUTGOING)
        {
            continue;
        }

        std::optional<Vlan> vlan;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        {
            if (c->cmsg_level != SOL_PACKET || c->cmsg_type != PACKET_AUXDATA)
            {
                continue;
            }
            tpacket_auxdata aux;
            std::memcpy(&aux, CMSG_DATA(c), sizeof(aux));
            if (aux.tp_status & TP_STATUS_VLAN_VALID)
            {
                vlan = Vlan{
                    static_cast<std::uint8_t>(aux.tp_vlan_tci >> 13),
                    static_cast<std::uint16_t>(aux.tp_vlan_tci & 0x0FFF)};
            }
        }
        handled += feed(Bytes(buf, static_cast<std::size_t>(n)), vlan);
    }
    return handled;
}

void Stack::run()
{
    running_ = true;
    while (running_)
    {
        poll(100);
    }
}

void Stack::stop()
{
    running_ = false;
}

void Stack::send(
    const Mac &dst,
    const std::uint16_t id,
    const Bytes payload,
    const std::optional<Vlan> &vlan)
{
    const auto frame = build(dst, mac_, id, payload, vlan);
    send_raw(frame);
}

void Stack::send_raw(const Bytes frame)
{
    if (connected_)
    {
        if (::send(fd_, frame.data(), frame.size(), 0) < 0)
        {
            fail("send");
        }
        return;
    }
    sockaddr_ll to{};
    to.sll_family = AF_PACKET;
    to.sll_ifindex = ifindex_;
    to.sll_halen = 6;
    std::copy_n(frame.begin(), std::min<std::size_t>(6, frame.size()), to.sll_addr);
    if (::sendto(
            fd_,
            frame.data(),
            frame.size(),
            0,
            reinterpret_cast<sockaddr *>(&to),
            sizeof(to)) < 0)
    {
        fail("sendto");
    }
}

} // namespace profinet
