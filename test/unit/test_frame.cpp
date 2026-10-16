/**
 * @file test_frame.cpp
 * @brief Unit tests for PROFINET frame parsing/building.
 */
#include "profinet/frame.h"

#include <gtest/gtest.h>

using namespace profinet;

namespace
{
const Mac k_dst{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
const Mac k_src{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
} // namespace

TEST(Frame, BuildParseRoundTripWithVlanAndApduStatus)
{
    // Arrange: 40 bytes IO data + APDU status (cc=0x1234, ds=0x35, ts=0x00)
    std::vector<std::uint8_t> payload(40, 0xAB);
    payload.insert(payload.end(), {0x12, 0x34, 0x35, 0x00});

    // Act
    const auto bytes = build(k_dst, k_src, 0xC001, payload, Vlan{6, 7});
    const auto f = parse(bytes);

    // Assert
    ASSERT_TRUE(f);
    EXPECT_EQ(f->dst, k_dst);
    EXPECT_EQ(f->src, k_src);
    ASSERT_TRUE(f->vlan);
    EXPECT_EQ(f->vlan->pcp, 6);
    EXPECT_EQ(f->vlan->vid, 7);
    EXPECT_EQ(f->frame_id, 0xC001);
    EXPECT_EQ(cyclic_data(*f).size(), 40u);
    const auto st = parse_apdu_status(*f);
    ASSERT_TRUE(st);
    EXPECT_EQ(st->cycle_counter, 0x1234);
    EXPECT_EQ(st->data_status, 0x35);
    EXPECT_FALSE(parse_dcp(*f));
}

TEST(Frame, ParsesDcpHeaderAndPadsShortFrames)
{
    const std::vector<std::uint8_t> dcp{
        0x05, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x00, 0x04,
        0xFF, 0xFF, 0x00, 0x00};

    const auto bytes = build(k_dst, k_src, frame_id::k_dcp_identify_req, dcp);
    const auto f = parse(bytes);

    EXPECT_EQ(bytes.size(), 60u);
    ASSERT_TRUE(f);
    EXPECT_FALSE(f->vlan);
    const auto h = parse_dcp(*f);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->service_id, 5);
    EXPECT_EQ(h->xid, 0xDEADBEEFu);
    EXPECT_EQ(h->response_delay, 1);
    EXPECT_EQ(h->data_length, 4);
    EXPECT_FALSE(parse_apdu_status(*f));
}

TEST(Frame, RejectsNonProfinetAndTruncated)
{
    std::vector<std::uint8_t> ipv4(60, 0);
    ipv4[12] = 0x08;

    EXPECT_FALSE(parse(ipv4));
    EXPECT_FALSE(parse(Bytes(ipv4.data(), 10)));
}
