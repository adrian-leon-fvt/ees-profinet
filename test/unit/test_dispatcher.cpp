/**
 * @file test_dispatcher.cpp
 * @brief Unit tests for socket-free profinet::Dispatcher::feed().
 */
#include "profinet/dispatcher.h"

#include <gtest/gtest.h>

using namespace profinet;

namespace
{
const Mac k_dst{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
const Mac k_src{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
} // namespace

TEST(Dispatcher, FeedParsesAndRunsMatchingHooksOnly)
{
    Dispatcher d;
    int dcp = 0;
    int cyclic = 0;
    d.on(frame_id::k_dcp_identify_req, [&](const FrameView &f) {
        EXPECT_EQ(f.src, k_src);
        ++dcp;
    });
    d.on(
        frame_id::k_rt_class1_first,
        frame_id::k_rt_class1_last,
        [&](const FrameView &) { ++cyclic; });
    const std::uint8_t payload[] = {1, 2, 3};

    EXPECT_TRUE(d.feed(build(k_dst, k_src, frame_id::k_dcp_identify_req, payload)));

    EXPECT_EQ(dcp, 1);
    EXPECT_EQ(cyclic, 0);
}

TEST(Dispatcher, OutOfBandVlanOverridesInBandTag)
{
    Dispatcher d;
    std::optional<Vlan> seen;
    d.on_any([&](const FrameView &f) { seen = f.vlan; });
    const auto tagged = build(k_dst, k_src, 0xC000, {}, Vlan{6, 10});

    d.feed(tagged);
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->vid, 10);

    d.feed(tagged, Vlan{3, 42});
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->pcp, 3);
    EXPECT_EQ(seen->vid, 42);
}

TEST(Dispatcher, RejectsRuntsAndTruncatedProfinetButFeedsOtherEthertypes)
{
    Dispatcher d;
    int profinet_hits = 0;
    int lldp_hits = 0;
    d.on_any([&](const FrameView &) { ++profinet_hits; });
    d.on_ethertype(0x88CC, [&](const FrameView &) { ++lldp_hits; });
    const std::uint8_t runt[10] = {};
    auto truncated = build(k_dst, k_src, 0xFEFE, {});
    truncated.resize(15); // EtherType 0x8892 present, FrameID cut off
    const auto lldp = build_ethernet(k_dst, k_src, 0x88CC, {});

    EXPECT_FALSE(d.feed(runt));
    EXPECT_FALSE(d.feed(truncated));
    EXPECT_TRUE(d.feed(lldp));

    EXPECT_EQ(profinet_hits, 0);
    EXPECT_EQ(lldp_hits, 1);
}
