/**
 * @file pn_sniff.cpp
 * @brief Example: print PROFINET frames and send a DCP Identify-All.
 *
 * Usage: sudo ./pn_sniff <interface>
 */
#include "profinet/stack.h"

#include <cstdio>
#include <csignal>

namespace
{
profinet::Stack *g_stack = nullptr;
}

int main(const int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <interface>\n", argv[0]);
        return 1;
    }
    profinet::Stack stack(argv[1]);
    g_stack = &stack;
    std::signal(SIGINT, [](int) { g_stack->stop(); });

    stack.on(
        profinet::frame_id::k_dcp_hello,
        profinet::frame_id::k_dcp_identify_rsp,
        [](const profinet::FrameView &f) {
            const auto h = profinet::parse_dcp(f);
            std::printf(
                "DCP id=%04X svc=%u type=%u xid=%08X len=%u\n",
                f.frame_id,
                h ? h->service_id : 0,
                h ? h->service_type : 0,
                h ? h->xid : 0,
                h ? h->data_length : 0);
        });
    stack.on(
        profinet::frame_id::k_rt_class3_first,
        profinet::frame_id::k_rt_class1_last,
        [](const profinet::FrameView &f) {
            const auto s = profinet::parse_apdu_status(f);
            std::printf(
                "RT  id=%04X data=%zu cc=%u ds=%02X\n",
                f.frame_id,
                profinet::cyclic_data(f).size(),
                s ? s->cycle_counter : 0,
                s ? s->data_status : 0);
        });

    // DCP Identify-All request to the PROFINET multicast address.
    const std::uint8_t identify_all[] = {
        0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
        0xFF, 0xFF, 0x00, 0x00};
    stack.send(
        {0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00},
        profinet::frame_id::k_dcp_identify_req,
        identify_all);

    stack.run();
    return 0;
}
