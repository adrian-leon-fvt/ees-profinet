/**
 * @file rpc.cpp
 * @brief DCE/RPC (connectionless), NDR wrapper and PNIO blocks (no I/O).
 */
#include "profinet/rpc.h"

#include <algorithm>

namespace profinet
{

namespace
{
constexpr std::size_t k_header_size = 80;
constexpr std::size_t k_ndr_size = 20;
constexpr std::uint8_t k_drep_little_endian = 0x10;

std::uint16_t rd16(const std::uint8_t *p, const bool le)
{
    return static_cast<std::uint16_t>(le ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]);
}

std::uint32_t rd32(const std::uint8_t *p, const bool le)
{
    return le ? p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t{p[3]} << 24)
              : (std::uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

std::uint16_t be16(const std::uint8_t *p)
{
    return rd16(p, false);
}

void wr16le(std::vector<std::uint8_t> &out, const std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void wr32le(std::vector<std::uint8_t> &out, const std::uint32_t v)
{
    wr16le(out, static_cast<std::uint16_t>(v));
    wr16le(out, static_cast<std::uint16_t>(v >> 16));
}

void wr16be(std::vector<std::uint8_t> &out, const std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/** @brief Swap time_low / time_mid / time_hi between canonical and LE. */
Uuid swap_uuid_fields(Uuid u)
{
    std::reverse(u.begin(), u.begin() + 4);
    std::reverse(u.begin() + 4, u.begin() + 6);
    std::reverse(u.begin() + 6, u.begin() + 8);
    return u;
}

Uuid rd_uuid(const std::uint8_t *p, const bool le)
{
    Uuid u;
    std::copy_n(p, u.size(), u.begin());
    return le ? swap_uuid_fields(u) : u;
}

void wr_uuid_le(std::vector<std::uint8_t> &out, const Uuid &u)
{
    const Uuid w = swap_uuid_fields(u);
    out.insert(out.end(), w.begin(), w.end());
}

/** @brief Common NDR layout: word0, ArgsLength, MaxCount, Offset, ActualCount. */
std::optional<Bytes> ndr_args(const RpcPacket &packet)
{
    const Bytes b = packet.body;
    if (b.size() < k_ndr_size)
    {
        return std::nullopt;
    }
    const std::uint32_t actual = rd32(&b[16], packet.little_endian);
    if (actual > b.size() - k_ndr_size)
    {
        return std::nullopt;
    }
    return b.subspan(k_ndr_size, actual);
}

std::vector<std::uint8_t> ndr_wrap(
    const std::uint8_t *word0,
    const std::uint32_t args_maximum,
    const Bytes args)
{
    std::vector<std::uint8_t> out(word0, word0 + 4);
    const auto len = static_cast<std::uint32_t>(args.size());
    wr32le(out, len);          // ArgsLength
    wr32le(out, args_maximum); // MaximumCount
    wr32le(out, 0);            // Offset
    wr32le(out, len);          // ActualCount
    out.insert(out.end(), args.begin(), args.end());
    return out;
}
} // namespace

Uuid make_pnio_object_uuid(
    const std::uint16_t instance,
    const std::uint16_t device_id,
    const std::uint16_t vendor_id)
{
    Uuid u{0xDE, 0xA0, 0x00, 0x00, 0x6C, 0x97, 0x11, 0xD1, 0x82, 0x71};
    const std::uint16_t tail[] = {instance, device_id, vendor_id};
    for (int i = 0; i < 3; ++i)
    {
        u[10 + 2 * i] = static_cast<std::uint8_t>(tail[i] >> 8);
        u[11 + 2 * i] = static_cast<std::uint8_t>(tail[i]);
    }
    return u;
}

std::optional<RpcPacket> parse_rpc(const Bytes datagram)
{
    if (datagram.size() < k_header_size || datagram[0] != 4)
    {
        return std::nullopt;
    }
    const std::uint8_t *p = datagram.data();
    const std::uint8_t int_rep = p[4] & 0xF0;
    if (int_rep != 0x00 && int_rep != k_drep_little_endian)
    {
        return std::nullopt;
    }
    RpcPacket out;
    const bool le = out.little_endian = int_rep == k_drep_little_endian;
    RpcHeader &h = out.header;
    h.ptype = p[1];
    h.flags1 = p[2];
    h.flags2 = p[3];
    h.object = rd_uuid(p + 8, le);
    h.interface = rd_uuid(p + 24, le);
    h.activity = rd_uuid(p + 40, le);
    h.server_boot = rd32(p + 56, le);
    h.interface_version = rd32(p + 60, le);
    h.sequence = rd32(p + 64, le);
    h.opnum = rd16(p + 68, le);
    h.interface_hint = rd16(p + 70, le);
    h.activity_hint = rd16(p + 72, le);
    const std::uint16_t len = rd16(p + 74, le);
    h.fragment_number = rd16(p + 76, le);
    if (datagram.size() < k_header_size + len)
    {
        return std::nullopt;
    }
    out.body = datagram.subspan(k_header_size, len);
    return out;
}

std::vector<std::uint8_t> build_rpc(const RpcHeader &h, const Bytes body)
{
    std::vector<std::uint8_t> out{
        4, h.ptype, h.flags1, h.flags2, k_drep_little_endian, 0, 0, 0};
    out.reserve(k_header_size + body.size());
    wr_uuid_le(out, h.object);
    wr_uuid_le(out, h.interface);
    wr_uuid_le(out, h.activity);
    wr32le(out, h.server_boot);
    wr32le(out, h.interface_version);
    wr32le(out, h.sequence);
    wr16le(out, h.opnum);
    wr16le(out, h.interface_hint);
    wr16le(out, h.activity_hint);
    wr16le(out, static_cast<std::uint16_t>(body.size()));
    wr16le(out, h.fragment_number);
    out.push_back(0); // auth_proto: none
    out.push_back(0); // serial_lo
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

RpcHeader make_response(const RpcHeader &request)
{
    RpcHeader r = request;
    r.ptype = rpc_ptype::k_response;
    r.flags1 = rpc_flags1::k_last_fragment | rpc_flags1::k_no_fack;
    r.flags2 = 0;
    r.fragment_number = 0;
    r.interface_hint = 0xFFFF;
    r.activity_hint = 0xFFFF;
    return r;
}

std::optional<NdrRequest> parse_ndr_request(const RpcPacket &packet)
{
    const auto args = ndr_args(packet);
    if (!args)
    {
        return std::nullopt;
    }
    return NdrRequest{rd32(packet.body.data(), packet.little_endian), *args};
}

std::optional<NdrResponse> parse_ndr_response(const RpcPacket &packet)
{
    const auto args = ndr_args(packet);
    if (!args)
    {
        return std::nullopt;
    }
    NdrResponse r;
    std::copy_n(packet.body.begin(), 4, r.status.begin());
    r.args_maximum = rd32(&packet.body[8], packet.little_endian);
    r.args = *args;
    return r;
}

std::vector<std::uint8_t> build_ndr_request(
    const std::uint32_t args_maximum,
    const Bytes args)
{
    std::vector<std::uint8_t> word0;
    wr32le(word0, args_maximum);
    return ndr_wrap(word0.data(), args_maximum, args);
}

std::vector<std::uint8_t> build_ndr_response(
    const PnioStatus &status,
    const std::uint32_t args_maximum,
    const Bytes args)
{
    return ndr_wrap(status.data(), args_maximum, args);
}

std::optional<std::vector<PnioBlock>> parse_blocks(Bytes args)
{
    std::vector<PnioBlock> out;
    while (!args.empty())
    {
        if (args.size() < 6)
        {
            return std::nullopt;
        }
        const std::uint16_t len = be16(&args[2]); // excludes type + length
        if (len < 2 || args.size() < 4u + len)
        {
            return std::nullopt;
        }
        out.push_back({be16(&args[0]), args[4], args[5], args.subspan(6, len - 2)});
        args = args.subspan(4u + len);
    }
    return out;
}

void append_block(
    std::vector<std::uint8_t> &out,
    const std::uint16_t type,
    const Bytes content,
    const std::uint8_t version_high,
    const std::uint8_t version_low)
{
    wr16be(out, type);
    wr16be(out, static_cast<std::uint16_t>(content.size() + 2));
    out.push_back(version_high);
    out.push_back(version_low);
    out.insert(out.end(), content.begin(), content.end());
}

} // namespace profinet
