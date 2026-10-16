/**
 * @file lldp.h
 * @brief LLDP (IEEE 802.1AB) frame parsing and building, as used by PROFINET
 *        for neighbourhood / topology detection.
 */
#ifndef PROFINET_LLDP_H
#define PROFINET_LLDP_H

#include "profinet/frame.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace profinet
{

/** @brief LLDP EtherType. */
constexpr std::uint16_t k_ethertype_lldp = 0x88CC;
/** @brief Nearest-bridge LLDP multicast address. */
constexpr Mac k_multicast_lldp{0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

/** @brief Organisationally unique identifier (3 bytes). */
using Oui = std::array<std::uint8_t, 3>;
/** @brief PROFIBUS & PROFINET International OUI for org-specific TLVs. */
constexpr Oui k_oui_profinet{0x00, 0x0E, 0xCF};
/** @brief IEEE 802.3 OUI (MAC/PHY configuration TLV, subtype 1). */
constexpr Oui k_oui_ieee_802_3{0x00, 0x12, 0x0F};

/** @brief TLV type numbers. */
namespace lldp_tlv
{
/** @name IEEE 802.1AB TLV types */
///@{
constexpr std::uint8_t k_end = 0;
constexpr std::uint8_t k_chassis_id = 1;
constexpr std::uint8_t k_port_id = 2;
constexpr std::uint8_t k_ttl = 3;
constexpr std::uint8_t k_port_description = 4;
constexpr std::uint8_t k_system_name = 5;
constexpr std::uint8_t k_system_description = 6;
constexpr std::uint8_t k_system_capabilities = 7;
constexpr std::uint8_t k_management_address = 8;
/** @brief Value layout: `OUI(3) | subtype(1) | info`. */
constexpr std::uint8_t k_org_specific = 127;
///@}
} // namespace lldp_tlv

/** @brief One TLV; @p value spans the receive buffer. */
struct LldpTlv
{
    std::uint8_t type = 0; ///< lldp_tlv::k_* value.
    Bytes value;           ///< TLV value (hook lifetime).
};

/** @brief Decoded LLDPDU. */
struct Lldp
{
    std::uint8_t chassis_id_subtype = 0; ///< 7 = locally assigned (PROFINET).
    std::string chassis_id; ///< Raw bytes; text for subtype 7 (PROFINET).
    std::uint8_t port_id_subtype = 0; ///< 7 = locally assigned (PROFINET).
    std::string port_id; ///< Raw bytes; text for subtype 7 (PROFINET).
    std::uint16_t ttl = 0; ///< Seconds.
    /** @brief All TLVs except End, in frame order (spans: hook lifetime). */
    std::vector<LldpTlv> tlvs;
};

/**
 * @brief Decode an LLDPDU; stops at the End TLV.
 * @param frame From parse_ethernet() or an on_ethertype(k_ethertype_lldp)
 *              hook.
 * @return nullopt if not EtherType 0x88CC, a TLV overruns the frame, or
 *         Chassis ID / Port ID / TTL is missing.
 */
std::optional<Lldp> parse_lldp(const FrameView &frame);

/**
 * @brief Append `type(7 bit) | length(9 bit) | value` to @p out.
 * @param out Buffer to extend.
 * @param type lldp_tlv::k_* value.
 * @param value At most 511 bytes.
 */
void append_lldp_tlv(
    std::vector<std::uint8_t> &out,
    std::uint8_t type,
    Bytes value);

/**
 * @brief Append a type-127 TLV: `OUI | subtype | info`.
 * @param out Buffer to extend.
 * @param oui e.g. k_oui_profinet.
 * @param subtype OUI-defined subtype.
 * @param info Subtype payload.
 */
void append_lldp_org_tlv(
    std::vector<std::uint8_t> &out,
    const Oui &oui,
    std::uint8_t subtype,
    Bytes info);

/**
 * @brief Build an LLDP frame to k_multicast_lldp.
 *
 * Chassis ID and Port ID use subtype 7 (locally assigned), as PROFINET
 * requires. Send it with Stack::send_raw().
 * @param src Source MAC (usually Stack::mac()).
 * @param chassis_id NameOfStation.
 * @param port_id e.g. "port-001".
 * @param ttl_s Time to live in seconds.
 * @param extra_tlvs Pre-encoded TLVs inserted before End (build them with
 *                   append_lldp_tlv / append_lldp_org_tlv).
 */
std::vector<std::uint8_t> build_lldp(
    const Mac &src,
    std::string_view chassis_id,
    std::string_view port_id,
    std::uint16_t ttl_s = 20,
    Bytes extra_tlvs = {});

} // namespace profinet

#endif // PROFINET_LLDP_H
