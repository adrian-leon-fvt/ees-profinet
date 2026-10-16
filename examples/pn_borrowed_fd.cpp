/**
 * @file pn_borrowed_fd.cpp
 * @brief Example: "your fd, library loop" mode. The application opens and
 *        configures the AF_PACKET socket; Stack borrows it for receive,
 *        dispatch and send, and leaves it open.
 *
 * Typical reason: a privileged helper opens the socket and passes the fd to
 * an unprivileged process, or other code shares the socket.
 *
 * Sends DCP Identify-All and prints each Identify response for 2 s.
 * Usage: sudo ./pn_borrowed_fd <interface>
 */
#include "profinet/stack.h"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace pn = profinet;

int main(const int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <interface>\n", argv[0]);
        return 1;
    }

    // Everything Stack(interface) would do, done by the application.
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
    if (fd < 0 || ::ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        std::perror("socket/SIOCGIFINDEX");
        return 1;
    }
    const int ifindex = ifr.ifr_ifindex;
    if (::ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
    {
        std::perror("SIOCGIFHWADDR");
        return 1;
    }
    pn::Mac mac{};
    std::memcpy(mac.data(), ifr.ifr_hwaddr.sa_data, mac.size());
    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifindex;
    const int on = 1; // VLAN tags stripped by the NIC come back as auxdata.
    if (::bind(fd, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0 ||
        ::setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on)) < 0)
    {
        std::perror("bind/PACKET_AUXDATA");
        return 1;
    }

    {
        pn::Stack stack(fd, mac, ifindex, pn::fd_ownership_t::BORROW);
        stack.join_multicast(pn::k_multicast_dcp); // Not done for adopted fds.
        stack.on(pn::frame_id::k_dcp_identify_rsp, [](const pn::FrameView &f) {
            const auto h = pn::parse_dcp(f);
            std::printf(
                "Identify response from %02X:%02X:%02X:%02X:%02X:%02X xid=%08X\n",
                f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5],
                h ? h->xid : 0);
        });

        const std::uint8_t identify_all[] = {
            0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0xFF, 0xFF, 0x00, 0x00};
        stack.send(pn::k_multicast_dcp, pn::frame_id::k_dcp_identify_req, identify_all);

        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end)
        {
            stack.poll(100);
        }
    } // Stack gone; fd still open (BORROW).

    ::close(fd);
    return 0;
}
