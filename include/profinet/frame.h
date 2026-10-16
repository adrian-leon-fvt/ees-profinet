/**
 * @file frame.h
 * @brief PROFINET layer-2 frame parsing and building (no I/O).
 */
#ifndef PROFINET_FRAME_H
#define PROFINET_FRAME_H

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace profinet
{

using Mac = std::array<std::uint8_t, 6>;
using Bytes = std::span<const std::uint8_t>;

constexpr std::uint16_t k_ethertype = 0x8892;
constexpr std::uint16_t k_ethertype_vlan = 0x8100;

/** @brief DCP Identify / Hello multicast address. */
constexpr Mac k_multicast_dcp{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};

/** @brief Well-known FrameID values and ranges (IEC 61158-6-10). */
namespace frame_id
{
constexpr std::uint16_t k_ptcp_first = 0x0000;
constexpr std::uint16_t k_ptcp_last = 0x00FF;
constexpr std::uint16_t k_rt_class3_first = 0x0100;
constexpr std::uint16_t k_rt_class3_last = 0x7FFF;
constexpr std::uint16_t k_rt_class2_first = 0x8000;
constexpr std::uint16_t k_rt_class2_last = 0xBFFF;
constexpr std::uint16_t k_rt_class1_first = 0xC000;
constexpr std::uint16_t k_rt_class1_last = 0xF7FF;
constexpr std::uint16_t k_alarm_high = 0xFC01;
constexpr std::uint16_t k_alarm_low = 0xFE01;
constexpr std::uint16_t k_dcp_hello = 0xFEFC;
constexpr std::uint16_t k_dcp_get_set = 0xFEFD;
constexpr std::uint16_t k_dcp_identify_req = 0xFEFE;
constexpr std::uint16_t k_dcp_identify_rsp = 0xFEFF;
} // namespace frame_id

/** @brief 802.1Q tag fields. */
struct Vlan
{
    std::uint8_t pcp = 6; ///< Priority; PROFINET RT uses 6.
    std::uint16_t vid = 0;
};

/** @brief Trailer of cyclic RT frames (last 4 bytes). */
struct ApduStatus
{
    std::uint16_t cycle_counter = 0;
    std::uint8_t data_status = 0;
    std::uint8_t transfer_status = 0;
};

/** @brief DCP PDU header (follows FrameID for 0xFEFC..0xFEFF). */
struct DcpHeader
{
    std::uint8_t service_id = 0;
    std::uint8_t service_type = 0;
    std::uint32_t xid = 0;
    std::uint16_t response_delay = 0;
    std::uint16_t data_length = 0;
};

/**
 * @brief Parsed, non-owning view of one Ethernet frame.
 * @note Spans point into the receive buffer; valid only inside the hook.
 */
struct FrameView
{
    Mac dst{};
    Mac src{};
    std::optional<Vlan> vlan;
    std::uint16_t ethertype = 0;
    std::uint16_t frame_id = 0; ///< PROFINET only; 0 for other EtherTypes.
    Bytes payload; ///< After FrameID (PROFINET) or after EtherType (others).
    Bytes raw;     ///< Whole Ethernet frame.
};

/** @return true if @p id is a cyclic RT data frame (class 1/2/3). */
bool is_cyclic(std::uint16_t id);

/** @return true if @p id is a DCP frame. */
bool is_dcp(std::uint16_t id);

/**
 * @brief Parse an Ethernet header of any EtherType.
 * @param raw Frame bytes starting at destination MAC, without FCS.
 * @return View with payload = bytes after EtherType, or nullopt if too short.
 */
std::optional<FrameView> parse_ethernet(Bytes raw);

/**
 * @brief Parse a PROFINET frame.
 * @param raw Frame bytes starting at destination MAC, without FCS.
 * @return View, or nullopt if not a PROFINET frame / too short.
 */
std::optional<FrameView> parse(Bytes raw);

/** @brief APDU status from the last 4 payload bytes of a cyclic frame. */
std::optional<ApduStatus> parse_apdu_status(const FrameView &frame);

/** @brief Cyclic IO data: payload without the APDU status trailer. */
Bytes cyclic_data(const FrameView &frame);

/** @brief DCP header from the payload of a DCP frame. */
std::optional<DcpHeader> parse_dcp(const FrameView &frame);

/**
 * @brief Build an Ethernet frame of any EtherType, padded to 60 bytes.
 * @param payload Bytes placed after the EtherType.
 */
std::vector<std::uint8_t> build_ethernet(
    const Mac &dst,
    const Mac &src,
    std::uint16_t ethertype,
    Bytes payload,
    const std::optional<Vlan> &vlan = std::nullopt);

/**
 * @brief Build a PROFINET frame, padded to the 60-byte minimum.
 * @param payload Bytes placed after FrameID.
 */
std::vector<std::uint8_t> build(
    const Mac &dst,
    const Mac &src,
    std::uint16_t id,
    Bytes payload,
    const std::optional<Vlan> &vlan = std::nullopt);

} // namespace profinet

#endif // PROFINET_FRAME_H
