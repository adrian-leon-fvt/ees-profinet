/**
 * @file pn_dispatcher.cpp
 * @brief Example: "fully yours" mode. Only Dispatcher + build(), no Stack,
 *        so it also builds with PROFINET_POSIX=OFF (preset `portable`).
 *
 * Linux uses AF_PACKET; Windows uses the example-only NpcapStack adapter.
 * On an MCU replace eth_open() / eth_rx() / eth_tx() with the Ethernet driver (e.g.
 * HAL_ETH receive / transmit, lwIP netif linkoutput). The rest is unchanged.
 *
 * Sends DCP Identify-All and prints each Identify response for 2 s.
 * Usage: sudo ./pn_dispatcher <interface>
 */
#include "profinet/dispatcher.h"

#ifdef _WIN32
#include <cstdint>
#ifdef PROFINET_NPCAP
#include "npcap_stack.h"
#endif
#else
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <poll.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pn = profinet;

namespace
{
#ifdef PROFINET_NPCAP
std::unique_ptr<profinet_example::NpcapStack> g_stack;
#else
int g_fd = -1;
#endif
int g_ifindex = 0;
pn::Mac g_mac{};

/** @brief Open the port and accept PROFINET multicast; false on error. */
bool eth_open(const char *ifname)
{
#ifdef _WIN32
#ifdef PROFINET_NPCAP
    g_stack = std::make_unique<profinet_example::NpcapStack>(ifname);
    g_mac = g_stack->mac();
    return true;
#else
    (void)ifname;
    return false;
#endif
#else
    g_fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (g_fd < 0 || ::ioctl(g_fd, SIOCGIFINDEX, &ifr) < 0)
    {
        return false;
    }
    g_ifindex = ifr.ifr_ifindex;
    if (::ioctl(g_fd, SIOCGIFHWADDR, &ifr) < 0)
    {
        return false;
    }
    std::memcpy(g_mac.data(), ifr.ifr_hwaddr.sa_data, g_mac.size());
    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = g_ifindex;
    packet_mreq mr{};
    mr.mr_ifindex = g_ifindex;
    mr.mr_type = PACKET_MR_MULTICAST;
    mr.mr_alen = 6;
    std::memcpy(mr.mr_address, pn::k_multicast_dcp.data(), 6);
    return ::bind(g_fd, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) == 0 &&
           ::setsockopt(g_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0;
#endif
}

#ifndef _WIN32
/** @brief Wait up to @p timeout_ms for one frame; returns its length or 0. */
std::size_t eth_rx(std::uint8_t *buf, const std::size_t cap, const int timeout_ms)
{
    pollfd p{g_fd, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0)
    {
        return 0;
    }
    const ssize_t n = ::recv(g_fd, buf, cap, 0);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}
#endif

/** @brief Transmit one complete frame. */
void eth_tx(const pn::Bytes frame)
{
#ifdef _WIN32
#ifdef PROFINET_NPCAP
    g_stack->send_raw(frame);
#else
    (void)frame;
    throw std::runtime_error("Windows Ethernet discovery requires Npcap SDK");
#endif
#else
    sockaddr_ll to{};
    to.sll_family = AF_PACKET;
    to.sll_ifindex = g_ifindex;
    to.sll_halen = 6;
    ::sendto(g_fd, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
#endif
}
} // namespace

int main(const int argc, char **argv)
try
{
#ifdef PROFINET_NPCAP
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        profinet_example::NpcapStack::list();
        return 0;
    }
#endif
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <interface>\n", argv[0]);
        return 1;
    }
    if (!eth_open(argv[1]))
    {
#ifdef _WIN32
#ifdef PROFINET_NPCAP
        std::fprintf(stderr, "pn_dispatcher: Npcap interface open failed\n");
#else
        std::fprintf(
            stderr, "pn_dispatcher: built without Npcap SDK; set NPCAP_SDK, "
            "reconfigure and rebuild\n");
#endif
#else
        std::perror("eth_open");
#endif
        return 1;
    }

#ifdef PROFINET_NPCAP
    pn::Dispatcher &hooks = *g_stack;
#else
    pn::Dispatcher hooks;
#endif
    hooks.on(pn::frame_id::k_dcp_identify_rsp, [](const pn::FrameView &f) {
        const auto h = pn::parse_dcp(f);
        std::printf(
            "Identify response from %02X:%02X:%02X:%02X:%02X:%02X xid=%08X\n",
            f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5],
            h ? h->xid : 0);
    });

    // DCP Identify-All: service 5, request, xid 1, delay 1, block All (FF FF).
    const std::uint8_t identify_all[] = {
        0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0xFF, 0xFF, 0x00, 0x00};
    eth_tx(pn::build(pn::k_multicast_dcp, g_mac, pn::frame_id::k_dcp_identify_req, identify_all));

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::uint8_t buf[1536];
    while (std::chrono::steady_clock::now() < end)
    {
#ifdef PROFINET_NPCAP
        g_stack->poll(100);
#elif !defined(_WIN32)
        if (const std::size_t n = eth_rx(buf, sizeof(buf), 100))
        {
            // Driver strips VLAN tags? Pass it: feed(raw, pn::Vlan{pcp, vid}).
            hooks.feed(pn::Bytes(buf, n));
        }
#endif
    }
#ifdef _WIN32
#ifdef PROFINET_NPCAP
    g_stack.reset();
#endif
#else
    ::close(g_fd);
#endif
    return 0;
}
catch (const std::exception &error)
{
    std::fprintf(stderr, "pn_dispatcher: %s\n", error.what());
    return 1;
}
