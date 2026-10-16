/**
 * @file test_lldp.cpp
 * @brief Unit tests for LLDP parsing/building.
 */
#include "profinet/lldp.h"

#include <gtest/gtest.h>

using namespace profinet;

namespace
{
const Mac k_src{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
} // namespace

TEST(Lldp, BuildParseRoundTripWithOrgTlv)
{
    // Arrange: PROFINET chassis-MAC TLV (OUI 00:0E:CF, subtype 5)
    std::vector<std::uint8_t> extra;
    append_lldp_org_tlv(extra, k_oui_profinet, 5, k_src);

    // Act
    const auto bytes = build_lldp(k_src, "plc-1", "port-001", 20, extra);
    const auto f = parse_ethernet(bytes);
    ASSERT_TRUE(f);
    const auto l = parse_lldp(*f);

    // Assert
    EXPECT_EQ(f->dst, k_multicast_lldp);
    EXPECT_EQ(f->ethertype, k_ethertype_lldp);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->chassis_id_subtype, 7);
    EXPECT_EQ(l->chassis_id, "plc-1");
    EXPECT_EQ(l->port_id, "port-001");
    EXPECT_EQ(l->ttl, 20);
    ASSERT_EQ(l->tlvs.size(), 4u);
    const LldpTlv &org = l->tlvs[3];
    EXPECT_EQ(org.type, lldp_tlv::k_org_specific);
    ASSERT_EQ(org.value.size(), 10u);
    EXPECT_EQ(org.value[2], 0xCF);
    EXPECT_EQ(org.value[3], 5);
    EXPECT_EQ(org.value[9], 0x01);
}

TEST(Lldp, LongTlvUsesNinthLengthBit)
{
    std::vector<std::uint8_t> out;
    const std::vector<std::uint8_t> value(300, 0x41);

    append_lldp_tlv(out, lldp_tlv::k_system_description, value);

    ASSERT_EQ(out.size(), 302u);
    EXPECT_EQ(out[0], (6 << 1) | 1);
    EXPECT_EQ(out[1], 300 - 256);
}

TEST(Lldp, RejectsOverrunAndMissingMandatoryTlvs)
{
    auto bytes = build_lldp(k_src, "a", "b");
    auto f = parse_ethernet(bytes);
    ASSERT_TRUE(f);
    ASSERT_TRUE(parse_lldp(*f));

    bytes[15] = 0xFF; // chassis TLV length overruns frame
    f = parse_ethernet(bytes);
    EXPECT_FALSE(parse_lldp(*f));

    std::vector<std::uint8_t> ttl_only;
    append_lldp_tlv(ttl_only, lldp_tlv::k_ttl, std::vector<std::uint8_t>{0, 20});
    const auto no_ids = build_ethernet(k_multicast_lldp, k_src, k_ethertype_lldp, ttl_only);
    EXPECT_FALSE(parse_lldp(*parse_ethernet(no_ids)));

    const auto pn = build(k_multicast_lldp, k_src, 0xC001, ttl_only);
    EXPECT_FALSE(parse_lldp(*parse_ethernet(pn)));
}
