/**
 * @file frame.h
 * @brief PROFINET layer-2 frame parsing and building (no I/O).
 *
 * All functions are pure: no I/O, no exceptions, no allocation except the
 * build functions. Usable alone for pcap / test-vector decoding.
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

/** @brief Ethernet address, e.g. `Mac{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00}`. */
using Mac = std::array<std::uint8_t, 6>;

/**
 * @brief Read-only, non-owning byte view.
 *
 * Converts implicitly from `std::vector<std::uint8_t>`,
 * `std::array<std::uint8_t, N>` and `std::uint8_t[N]`; use
 * `Bytes(ptr, len)` for a pointer + length.
 */
using Bytes = std::span<const std::uint8_t>;

/** @brief PROFINET EtherType. */
constexpr std::uint16_t k_ethertype = 0x8892;
/** @brief 802.1Q VLAN tag protocol identifier. */
constexpr std::uint16_t k_ethertype_vlan = 0x8100;

/** @brief DCP Identify / Hello multicast address. */
constexpr Mac k_multicast_dcp{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};

/**
 * @brief Well-known FrameID values and ranges (IEC 61158-6-10).
 * @see docs/concepts.md "FrameID ranges".
 */
namespace frame_id
{
/** @name Inclusive FrameID ranges */
///@{
constexpr std::uint16_t k_ptcp_first = 0x0000;      ///< PTCP (time sync)
constexpr std::uint16_t k_ptcp_last = 0x00FF;       ///< PTCP (time sync)
constexpr std::uint16_t k_rt_class3_first = 0x0100; ///< IRT
constexpr std::uint16_t k_rt_class3_last = 0x7FFF;  ///< IRT
constexpr std::uint16_t k_rt_class2_first = 0x8000; ///< RT class 2
constexpr std::uint16_t k_rt_class2_last = 0xBFFF;  ///< RT class 2
constexpr std::uint16_t k_rt_class1_first = 0xC000; ///< RT class 1 (cyclic IO)
constexpr std::uint16_t k_rt_class1_last = 0xF7FF;  ///< RT class 1 (cyclic IO)
///@}
/** @name Single FrameIDs */
///@{
constexpr std::uint16_t k_alarm_high = 0xFC01;       ///< Alarm, high priority
constexpr std::uint16_t k_alarm_low = 0xFE01;        ///< Alarm, low priority
constexpr std::uint16_t k_dcp_hello = 0xFEFC;        ///< DCP Hello
constexpr std::uint16_t k_dcp_get_set = 0xFEFD;      ///< DCP Get / Set
constexpr std::uint16_t k_dcp_identify_req = 0xFEFE; ///< DCP Identify request
constexpr std::uint16_t k_dcp_identify_rsp = 0xFEFF; ///< DCP Identify response
///@}
} // namespace frame_id

/** @brief 802.1Q tag fields. */
struct Vlan
{
    std::uint8_t pcp = 6;  ///< Priority 0–7; PROFINET RT uses 6.
    std::uint16_t vid = 0; ///< VLAN ID 0–4095; 0 = priority tag only.
};

/** @brief Trailer of cyclic RT frames (last 4 bytes). */
struct ApduStatus
{
    std::uint16_t cycle_counter = 0; ///< Sender cycle counter (31.25 µs units).
    /** @brief Bits: State, Redundancy, DataValid, ProviderState,
     *         StationProblemIndicator, Ignore. */
    std::uint8_t data_status = 0;
    std::uint8_t transfer_status = 0; ///< 0 = OK.
};

/** @brief DCP PDU header (follows FrameID for 0xFEFC..0xFEFF). */
struct DcpHeader
{
    std::uint8_t service_id = 0;   ///< 3 Get, 4 Set, 5 Identify, 6 Hello.
    std::uint8_t service_type = 0; ///< 0 request, 1 success, 5 unsupported.
    std::uint32_t xid = 0;         ///< Transaction ID; echo it in the response.
    std::uint16_t response_delay = 0; ///< Identify request: delay factor.
    std::uint16_t data_length = 0; ///< Bytes of blocks after the header.
};

/**
 * @brief Parsed, non-owning view of one Ethernet frame.
 * @note Spans point into the receive buffer; valid only inside the hook
 *       (see docs/concepts.md "Lifetimes"). Copy what you keep.
 */
struct FrameView
{
    Mac dst{}; ///< Destination MAC.
    Mac src{}; ///< Source MAC.
    /** @brief Set if tagged (in-band or NIC-offloaded; with offload @ref raw
     *         has no tag). */
    std::optional<Vlan> vlan;
    std::uint16_t ethertype = 0; ///< EtherType after any VLAN tag.
    std::uint16_t frame_id = 0;  ///< PROFINET only; 0 for other EtherTypes.
    /** @brief PROFINET: after FrameID, incl. APDU status and Ethernet
     *         padding. Others: after EtherType. */
    Bytes payload;
    Bytes raw; ///< Whole frame from destination MAC, as received.
};

/** @return true if @p id is a cyclic RT data frame (0x0100–0xF7FF). */
bool is_cyclic(std::uint16_t id);

/** @return true if @p id is a DCP frame (0xFEFC–0xFEFF). */
bool is_dcp(std::uint16_t id);

/**
 * @brief Parse an Ethernet header of any EtherType (one optional 802.1Q tag).
 * @param raw Frame bytes starting at destination MAC, without FCS.
 * @return View with payload = bytes after EtherType and frame_id = 0, or
 *         nullopt if shorter than 14 bytes (18 with a VLAN tag).
 */
std::optional<FrameView> parse_ethernet(Bytes raw);

/**
 * @brief Parse a PROFINET frame: parse_ethernet() plus FrameID.
 * @param raw Frame bytes starting at destination MAC, without FCS.
 * @return View with payload = bytes after FrameID, or nullopt if the
 *         EtherType is not 0x8892 or the frame ends before the FrameID.
 */
std::optional<FrameView> parse(Bytes raw);

/**
 * @brief APDU status from the last 4 payload bytes of a cyclic frame.
 * @param frame Parsed PROFINET frame.
 * @return nullopt if the FrameID is not cyclic or payload < 4 bytes.
 * @note Assumes no Ethernet padding; holds for valid RT frames (C_SDU ≥ 40).
 */
std::optional<ApduStatus> parse_apdu_status(const FrameView &frame);

/**
 * @brief Cyclic IO data (C_SDU): payload without the 4-byte APDU status.
 * @param frame Parsed PROFINET frame; FrameID is not checked.
 * @return Empty if payload < 4 bytes.
 */
Bytes cyclic_data(const FrameView &frame);

/**
 * @brief Decode the 10-byte DCP header; blocks are `payload.subspan(10)`.
 * @param frame Parsed PROFINET frame.
 * @return nullopt if the FrameID is not DCP or payload < 10 bytes.
 */
std::optional<DcpHeader> parse_dcp(const FrameView &frame);

/**
 * @brief Build `dst | src | [0x8100 TCI] | ethertype | payload`, zero-padded
 *        to 60 bytes.
 * @param dst Destination MAC.
 * @param src Source MAC (usually Stack::mac()).
 * @param ethertype EtherType after the optional tag.
 * @param payload Bytes placed after the EtherType.
 * @param vlan Optional 802.1Q tag.
 */
std::vector<std::uint8_t> build_ethernet(
    const Mac &dst,
    const Mac &src,
    std::uint16_t ethertype,
    Bytes payload,
    const std::optional<Vlan> &vlan = std::nullopt);

/**
 * @brief Build `dst | src | [0x8100 TCI] | 0x8892 | id | payload`,
 *        zero-padded to 60 bytes.
 * @param dst Destination MAC.
 * @param src Source MAC (usually Stack::mac()).
 * @param id FrameID.
 * @param payload Bytes placed after FrameID; keep ≤ 1496 (1492 with VLAN)
 *        for a 1500-byte MTU, not checked.
 * @param vlan Optional 802.1Q tag.
 */
std::vector<std::uint8_t> build(
    const Mac &dst,
    const Mac &src,
    std::uint16_t id,
    Bytes payload,
    const std::optional<Vlan> &vlan = std::nullopt);

} // namespace profinet

#endif // PROFINET_FRAME_H
