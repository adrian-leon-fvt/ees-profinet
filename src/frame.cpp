/**
 * @file frame.cpp
 * @brief PROFINET layer-2 frame parsing and building.
 */
#include "profinet/frame.h"

#include <algorithm>

namespace profinet
{

namespace
{
std::uint16_t be16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void put16(std::vector<std::uint8_t> &out, const std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

constexpr std::size_t k_min_frame = 60;
} // namespace

bool is_cyclic(const std::uint16_t id)
{
    return id >= frame_id::k_rt_class3_first && id <= frame_id::k_rt_class1_last;
}

bool is_dcp(const std::uint16_t id)
{
    return id >= frame_id::k_dcp_hello && id <= frame_id::k_dcp_identify_rsp;
}

std::optional<FrameView> parse_ethernet(const Bytes raw)
{
    if (raw.size() < 14)
    {
        return std::nullopt;
    }
    FrameView f;
    std::copy_n(raw.begin(), 6, f.dst.begin());
    std::copy_n(raw.begin() + 6, 6, f.src.begin());
    f.raw = raw;

    std::size_t off = 12;
    if (be16(&raw[off]) == k_ethertype_vlan)
    {
        if (raw.size() < 18)
        {
            return std::nullopt;
        }
        const std::uint16_t tci = be16(&raw[off + 2]);
        f.vlan = Vlan{static_cast<std::uint8_t>(tci >> 13),
                      static_cast<std::uint16_t>(tci & 0x0FFF)};
        off += 4;
    }
    f.ethertype = be16(&raw[off]);
    f.payload = raw.subspan(off + 2);
    return f;
}

std::optional<FrameView> parse(const Bytes raw)
{
    auto f = parse_ethernet(raw);
    if (!f || f->ethertype != k_ethertype || f->payload.size() < 2)
    {
        return std::nullopt;
    }
    f->frame_id = be16(f->payload.data());
    f->payload = f->payload.subspan(2);
    return f;
}

std::optional<ApduStatus> parse_apdu_status(const FrameView &frame)
{
    // ponytail: assumes no Ethernet padding; true for RT since C_SDU >= 40.
    const Bytes p = frame.payload;
    if (!is_cyclic(frame.frame_id) || p.size() < 4)
    {
        return std::nullopt;
    }
    const std::uint8_t *t = p.data() + p.size() - 4;
    return ApduStatus{be16(t), t[2], t[3]};
}

Bytes cyclic_data(const FrameView &frame)
{
    const Bytes p = frame.payload;
    return p.size() < 4 ? Bytes{} : p.first(p.size() - 4);
}

std::optional<DcpHeader> parse_dcp(const FrameView &frame)
{
    const Bytes p = frame.payload;
    if (!is_dcp(frame.frame_id) || p.size() < 10)
    {
        return std::nullopt;
    }
    return DcpHeader{
        p[0],
        p[1],
        (static_cast<std::uint32_t>(be16(&p[2])) << 16) | be16(&p[4]),
        be16(&p[6]),
        be16(&p[8])};
}

std::vector<std::uint8_t> build_ethernet(
    const Mac &dst,
    const Mac &src,
    const std::uint16_t ethertype,
    const Bytes payload,
    const std::optional<Vlan> &vlan)
{
    std::vector<std::uint8_t> out(dst.begin(), dst.end());
    out.insert(out.end(), src.begin(), src.end());
    if (vlan)
    {
        put16(out, k_ethertype_vlan);
        put16(
            out,
            static_cast<std::uint16_t>((vlan->pcp << 13) | (vlan->vid & 0x0FFF)));
    }
    put16(out, ethertype);
    out.insert(out.end(), payload.begin(), payload.end());
    if (out.size() < k_min_frame)
    {
        out.resize(k_min_frame, 0);
    }
    return out;
}

std::vector<std::uint8_t> build(
    const Mac &dst,
    const Mac &src,
    const std::uint16_t id,
    const Bytes payload,
    const std::optional<Vlan> &vlan)
{
    std::vector<std::uint8_t> pdu;
    pdu.reserve(2 + payload.size());
    put16(pdu, id);
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return build_ethernet(dst, src, k_ethertype, pdu, vlan);
}

} // namespace profinet
