/**
 * @file profinet_loopback.cpp
 * @brief Manual/integration test: send PROFINET frames out of one interface
 *        and check the hooks on another interface receive them intact.
 *
 * Usage: profinet_loopback <tx_if> <rx_if>   (needs CAP_NET_RAW)
 * The two interfaces must be linked (veth pair, cable, switch).
 * Exit code 0 = pass, 1 = fail.
 */
#include "profinet/stack.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

namespace
{
/** @return true if the kernel lists @p mac in @p ifname's multicast filter. */
bool in_dev_mcast(const std::string &ifname, const std::string &mac)
{
    std::ifstream in("/proc/net/dev_mcast");
    std::string idx, name, refs, global, addr;
    while (in >> idx >> name >> refs >> global >> addr)
    {
        if (name == ifname && addr == mac)
        {
            return true;
        }
    }
    return false;
}
} // namespace

int main(const int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <tx_if> <rx_if>\n", argv[0]);
        return 2;
    }
    try
    {
        profinet::Stack tx(argv[1]);
        profinet::Stack rx(argv[2]);

        int rt_ok = 0;
        int dcp_ok = 0;
        int lldp_ok = 0;
        rx.on_ethertype(profinet::k_ethertype_lldp, [&](const auto &f) {
            const auto l = profinet::parse_lldp(f);
            lldp_ok += l && l->chassis_id == "loopback-tx" && l->port_id == "port-001";
        });
        rx.on(
            profinet::frame_id::k_rt_class1_first,
            profinet::frame_id::k_rt_class1_last,
            [&](const profinet::FrameView &f) {
                const auto s = profinet::parse_apdu_status(f);
                rt_ok += f.src == tx.mac() && f.frame_id == 0xC001 && s &&
                         s->cycle_counter == 0x1234 && s->data_status == 0x35 &&
                         profinet::cyclic_data(f).size() == 40;
            });
        rx.on(profinet::frame_id::k_dcp_identify_req, [&](const auto &f) {
            const auto h = profinet::parse_dcp(f);
            dcp_ok += h && h->xid == 0xCAFE0001;
        });

        std::vector<std::uint8_t> rt(40, 0xAB);
        rt.insert(rt.end(), {0x12, 0x34, 0x35, 0x00});
        const std::uint8_t dcp[] = {
            0x05, 0x00, 0xCA, 0xFE, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
            0xFF, 0xFF, 0x00, 0x00};
        const profinet::Mac multicast{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};

        tx.send(rx.mac(), 0xC001, rt);
        tx.send(multicast, profinet::frame_id::k_dcp_identify_req, dcp);
        tx.send_raw(profinet::build_lldp(tx.mac(), "loopback-tx", "port-001"));

        // ponytail: fixed 10 x 100 ms window; raise if testing over slow links.
        for (int i = 0; i < 10 && (rt_ok == 0 || dcp_ok == 0 || lldp_ok == 0); ++i)
        {
            rx.poll(100);
        }

        const bool mcast_ok = in_dev_mcast(argv[2], "010ecf000000") &&
                              in_dev_mcast(argv[2], "0180c200000e");

        std::printf("RT  : %s\nDCP : %s\nLLDP: %s\nMCAST: %s\n",
                    rt_ok ? "PASS" : "FAIL",
                    dcp_ok ? "PASS" : "FAIL",
                    lldp_ok ? "PASS" : "FAIL",
                    mcast_ok ? "PASS" : "FAIL");
        return rt_ok && dcp_ok && lldp_ok && mcast_ok ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
