/**
 * @file lldp.cpp
 * @brief LLDP frame parsing and building.
 */
#include "profinet/lldp.h"

namespace profinet
{

std::optional<Lldp> parse_lldp(const FrameView &frame)
{
    if (frame.ethertype != k_ethertype_lldp)
    {
        return std::nullopt;
    }
    Lldp out;
    bool have_chassis = false;
    bool have_port = false;
    bool have_ttl = false;
    Bytes p = frame.payload;
    while (p.size() >= 2)
    {
        const std::uint8_t type = p[0] >> 1;
        const std::size_t len = ((p[0] & 0x01) << 8) | p[1];
        if (p.size() < 2 + len)
        {
            return std::nullopt;
        }
        const Bytes value = p.subspan(2, len);
        p = p.subspan(2 + len);
        if (type == lldp_tlv::k_end)
        {
            break;
        }
        out.tlvs.push_back({type, value});
        if (type == lldp_tlv::k_chassis_id && len >= 1)
        {
            out.chassis_id_subtype = value[0];
            out.chassis_id.assign(value.begin() + 1, value.end());
            have_chassis = true;
        }
        else if (type == lldp_tlv::k_port_id && len >= 1)
        {
            out.port_id_subtype = value[0];
            out.port_id.assign(value.begin() + 1, value.end());
            have_port = true;
        }
        else if (type == lldp_tlv::k_ttl && len >= 2)
        {
            out.ttl = static_cast<std::uint16_t>((value[0] << 8) | value[1]);
            have_ttl = true;
        }
    }
    if (!have_chassis || !have_port || !have_ttl)
    {
        return std::nullopt;
    }
    return out;
}

void append_lldp_tlv(
    std::vector<std::uint8_t> &out,
    const std::uint8_t type,
    const Bytes value)
{
    out.push_back(static_cast<std::uint8_t>((type << 1) | ((value.size() >> 8) & 0x01)));
    out.push_back(static_cast<std::uint8_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void append_lldp_org_tlv(
    std::vector<std::uint8_t> &out,
    const Oui &oui,
    const std::uint8_t subtype,
    const Bytes info)
{
    std::vector<std::uint8_t> value(oui.begin(), oui.end());
    value.push_back(subtype);
    value.insert(value.end(), info.begin(), info.end());
    append_lldp_tlv(out, lldp_tlv::k_org_specific, value);
}

std::vector<std::uint8_t> build_lldp(
    const Mac &src,
    const std::string_view chassis_id,
    const std::string_view port_id,
    const std::uint16_t ttl_s,
    const Bytes extra_tlvs)
{
    constexpr std::uint8_t k_locally_assigned = 7;
    auto with_subtype = [k_locally_assigned](const std::string_view id) {
        std::vector<std::uint8_t> v{k_locally_assigned};
        v.insert(v.end(), id.begin(), id.end());
        return v;
    };
    const std::uint8_t ttl[] = {
        static_cast<std::uint8_t>(ttl_s >> 8),
        static_cast<std::uint8_t>(ttl_s)};

    std::vector<std::uint8_t> pdu;
    append_lldp_tlv(pdu, lldp_tlv::k_chassis_id, with_subtype(chassis_id));
    append_lldp_tlv(pdu, lldp_tlv::k_port_id, with_subtype(port_id));
    append_lldp_tlv(pdu, lldp_tlv::k_ttl, ttl);
    pdu.insert(pdu.end(), extra_tlvs.begin(), extra_tlvs.end());
    append_lldp_tlv(pdu, lldp_tlv::k_end, {});
    return build_ethernet(k_multicast_lldp, src, k_ethertype_lldp, pdu);
}

} // namespace profinet
