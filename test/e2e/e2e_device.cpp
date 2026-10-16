/**
 * @file e2e_device.cpp
 * @brief Minimal PROFINET IO-Device built on the library, driven by
 *        profinet-py in test_e2e.py.
 *
 * Implements DCP Identify (all / by name), Get, Set (name, IP, signal) and
 * RPC Read / Read Implicit / Write with an in-memory record store, and a
 * single-AR connection manager: Connect, PrmEnd, ApplicationReady, Release
 * and cyclic IO that echoes the output IOCR into the input IOCR. Not a
 * conformant device: one fixed submodule (4 bytes in, 4 bytes out, symmetric
 * IOCR layouts), no alarms.
 *
 * Usage: profinet_e2e_device <interface> <ip> <name>
 * Prints "ready" on stdout once both sockets are open.
 */
#include "profinet/rpc.h"
#include "profinet/stack.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pn = profinet;

namespace
{
constexpr std::uint8_t k_dcp_get = 3;
constexpr std::uint8_t k_dcp_set = 4;
constexpr std::uint8_t k_dcp_identify = 5;
constexpr std::uint16_t k_vendor_id = 0x002A;
constexpr std::uint16_t k_device_id = 0x0203;
constexpr std::uint16_t k_block_write_req = 0x0008;
constexpr std::uint16_t k_block_write_res = 0x8008;
constexpr std::uint16_t k_block_read_res = 0x8009;
constexpr std::uint16_t k_control_done = 0x0008;
constexpr std::uint16_t k_control_app_ready = 0x0002;
constexpr std::uint16_t k_output_frame_id = 0xC100; ///< Assigned to the output IOCR.
constexpr std::size_t k_csdu_length = 40;         ///< Minimum RT C_SDU.
/** @brief DataStatus: primary, valid, run, station ok. */
constexpr std::uint8_t k_data_status_ok = 0x01 | 0x04 | 0x10 | 0x20;
/** @brief IODRead/WriteReq content bytes up to and incl. RecordDataLength. */
constexpr std::size_t k_iod_fixed = 34;
/** @brief IODRead/WriteReq content size (BlockLength 60 - 2 version bytes). */
constexpr std::size_t k_iod_content = 58;
/** @brief PNIO status "PNIORW: access, invalid index". */
constexpr pn::PnioStatus k_invalid_index{0xDE, 0x80, 0xB0, 0x00};

using ByteVec = std::vector<std::uint8_t>;

void put16(ByteVec &out, const std::size_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put32(ByteVec &out, const std::size_t v)
{
    put16(out, v >> 16);
    put16(out, v & 0xFFFF);
}

std::uint16_t get16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

/** @brief Append DCP block: option, suboption, length, @p data, pad to even. */
void dcp_block(ByteVec &out, const int option, const int suboption, const ByteVec &data)
{
    out.push_back(static_cast<std::uint8_t>(option));
    out.push_back(static_cast<std::uint8_t>(suboption));
    put16(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
    if (out.size() & 1)
    {
        out.push_back(0);
    }
}

/** @brief Control/Response block acknowledging (option, suboption). */
void dcp_ack(ByteVec &out, const int option, const int suboption, const std::uint8_t error)
{
    dcp_block(out, 5, 4, {static_cast<std::uint8_t>(option), static_cast<std::uint8_t>(suboption), error});
}

/** @brief Calls @p fn(option, suboption, value) per DCP block; false if truncated. */
template <typename Fn> bool for_each_dcp_block(pn::Bytes in, Fn fn)
{
    while (in.size() >= 4)
    {
        const std::size_t len = get16(&in[2]);
        if (in.size() < 4 + len)
        {
            return false;
        }
        fn(in[0], in[1], in.subspan(4, len));
        in = in.subspan(std::min(in.size(), 4 + len + (len & 1)));
    }
    return true;
}

/** @brief Device state mutated by DCP Set and RPC Write. */
struct Device
{
    std::string name;
    ByteVec ip_suite; ///< IP, mask, gateway (12 bytes).
    std::map<std::uint16_t, ByteVec> records{{0xAFF0, {'h', 'e', 'l', 'l', 'o'}}};

    /** @brief Append the Get/Identify response block; false if unknown. */
    bool get_block(ByteVec &out, const int option, const int suboption) const
    {
        ByteVec d{0, 0}; // BlockInfo
        if (option == 1 && suboption == 2)
        {
            d[1] = 1; // IP set
            d.insert(d.end(), ip_suite.begin(), ip_suite.end());
        }
        else if (option == 2 && suboption == 1)
        {
            const std::string type = "ees-e2e";
            d.insert(d.end(), type.begin(), type.end());
        }
        else if (option == 2 && suboption == 2)
        {
            d.insert(d.end(), name.begin(), name.end());
        }
        else if (option == 2 && suboption == 3)
        {
            put16(d, k_vendor_id);
            put16(d, k_device_id);
        }
        else if (option == 2 && suboption == 4)
        {
            d.insert(d.end(), {0x01, 0x00}); // IO-Device
        }
        else
        {
            return false;
        }
        dcp_block(out, option, suboption, d);
        return true;
    }

    /** @brief Apply one Set block (@p v after BlockQualifier); DCP error code. */
    std::uint8_t set(const int option, const int suboption, const pn::Bytes v)
    {
        if (option == 2 && suboption == 2)
        {
            name.assign(v.begin(), v.end());
            return 0;
        }
        if (option == 1 && suboption == 2 && v.size() == 12)
        {
            ip_suite.assign(v.begin(), v.end());
            return 0;
        }
        if (option == 5 && suboption == 3)
        {
            std::fprintf(stderr, "e2e_device: signal\n");
            return 0;
        }
        return 2; // suboption not supported
    }
};

using Clock = std::chrono::steady_clock;

/** @brief The single Application Relationship and its cyclic IO state. */
struct Ar
{
    bool open = false;
    bool cyclic = false;
    ByteVec id; ///< ARUUID (16) + SessionKey (2), echoed in Control/Release.
    pn::Mac controller{};
    std::uint16_t in_id = 0;   ///< Input IOCR FrameID (device -> controller).
    std::uint16_t counter = 0; ///< Cycle counter, 31.25 us units.
    std::uint16_t step = 0;    ///< SendClockFactor * ReductionRatio.
    Clock::duration period{};
    Clock::time_point next;
    /** @brief Last output C_SDU; IOPS/IOCS bytes start GOOD (0x80). */
    ByteVec io = ByteVec(k_csdu_length, 0);
};

/** @brief IODControl/Release content: ARUUID, SessionKey, command @p cmd. */
ByteVec control_content(const ByteVec &ar_id, const std::uint16_t cmd)
{
    ByteVec c{0, 0};
    c.insert(c.end(), ar_id.begin(), ar_id.end());
    put16(c, 0); // AlarmSequenceNumber
    put16(c, cmd);
    put16(c, 0); // ControlBlockProperties
    return c;
}

/** @brief DCP blocks, excluding Ethernet padding; empty if truncated. */
pn::Bytes dcp_blocks(const pn::FrameView &f, const pn::DcpHeader &h)
{
    return f.payload.size() < 10u + h.data_length ? pn::Bytes{} : f.payload.subspan(10, h.data_length);
}

ByteVec dcp_response(const pn::DcpHeader &req, const ByteVec &blocks)
{
    ByteVec rsp{req.service_id, 1};
    put32(rsp, req.xid);
    put16(rsp, 0);
    put16(rsp, blocks.size());
    rsp.insert(rsp.end(), blocks.begin(), blocks.end());
    return rsp;
}
} // namespace

int main(const int argc, char **argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: %s <interface> <ip> <name>\n", argv[0]);
        return 2;
    }
    Device dev;
    dev.name = argv[3];
    dev.ip_suite = {0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 0, 0};
    if (::inet_pton(AF_INET, argv[2], dev.ip_suite.data()) != 1)
    {
        std::fprintf(stderr, "bad ip %s\n", argv[2]);
        return 2;
    }

    pn::Stack stack(argv[1]);
    pn::RpcEndpoint rpc(pn::k_rpc_port, argv[2]);

    stack.on(pn::frame_id::k_dcp_identify_req, [&](const pn::FrameView &f) {
        const auto req = pn::parse_dcp(f);
        if (!req || req->service_id != k_dcp_identify || req->service_type != 0)
        {
            return;
        }
        bool match = true;
        const bool complete = for_each_dcp_block(dcp_blocks(f, *req), [&](int opt, int sub, pn::Bytes v) {
            if (opt == 2 && sub == 2 && std::string(v.begin(), v.end()) != dev.name)
            {
                match = false;
            }
        });
        if (!complete || !match)
        {
            return;
        }
        ByteVec blocks;
        for (const auto &[opt, sub] : {std::pair{2, 1}, {2, 2}, {2, 3}, {2, 4}, {1, 2}})
        {
            dev.get_block(blocks, opt, sub);
        }
        stack.send(f.src, pn::frame_id::k_dcp_identify_rsp, dcp_response(*req, blocks));
    });

    stack.on(pn::frame_id::k_dcp_get_set, [&](const pn::FrameView &f) {
        const auto req = pn::parse_dcp(f);
        if (!req || req->service_type != 0 || f.dst != stack.mac())
        {
            return;
        }
        const pn::Bytes in = dcp_blocks(f, *req);
        ByteVec blocks;
        if (req->service_id == k_dcp_get)
        {
            // Get request: bare (option, suboption) pairs, no length field.
            for (std::size_t i = 0; i + 1 < in.size(); i += 2)
            {
                if (!dev.get_block(blocks, in[i], in[i + 1]))
                {
                    dcp_ack(blocks, in[i], in[i + 1], 2);
                }
            }
        }
        else if (req->service_id == k_dcp_set)
        {
            for_each_dcp_block(in, [&](int opt, int sub, pn::Bytes v) {
                dcp_ack(blocks, opt, sub, v.size() >= 2 ? dev.set(opt, sub, v.subspan(2)) : 2);
            });
        }
        else
        {
            return;
        }
        stack.send(f.src, pn::frame_id::k_dcp_get_set, dcp_response(*req, blocks));
    });

    auto on_record = [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &from) {
        const auto req = pn::parse_ndr_request(p);
        // Only the first block: Write appends raw record data after it.
        const auto header_len = req && req->args.size() >= 4 ? 4u + get16(&req->args[2]) : 0u;
        const auto blocks = header_len && header_len <= req->args.size()
                                ? pn::parse_blocks(req->args.first(header_len))
                                : std::nullopt;
        if (!blocks || blocks->empty() || (*blocks)[0].content.size() < k_iod_fixed)
        {
            return;
        }
        const pn::PnioBlock &hdr = (*blocks)[0];
        const std::uint16_t index = get16(&hdr.content[28]);
        const bool write = hdr.type == k_block_write_req;

        pn::PnioStatus status = pn::k_pnio_ok;
        ByteVec record;
        std::size_t record_length = 0;
        if (write)
        {
            ByteVec &r = dev.records[index];
            r.assign(req->args.begin() + header_len, req->args.end());
            record_length = r.size();
        }
        else if (const auto it = dev.records.find(index); it != dev.records.end())
        {
            record = it->second;
            record_length = record.size();
        }
        else
        {
            status = k_invalid_index;
        }

        ByteVec content(hdr.content.begin(), hdr.content.begin() + k_iod_fixed - 4);
        put32(content, record_length);
        put32(content, 0); // AdditionalValue1/2
        if (write)
        {
            content.insert(content.end(), status.begin(), status.end());
        }
        content.resize(k_iod_content);

        ByteVec args;
        pn::append_block(args, write ? k_block_write_res : k_block_read_res, content);
        args.insert(args.end(), record.begin(), record.end());
        rpc.reply(p, from, pn::build_ndr_response(status, req->args_maximum, args));
    };
    rpc.on(pn::pnio_opnum::k_read, on_record);
    rpc.on(pn::pnio_opnum::k_read_implicit, on_record);
    rpc.on(pn::pnio_opnum::k_write, on_record);

    Ar ar;
    std::uint32_t seq = 0;

    stack.on(pn::frame_id::k_rt_class1_first, pn::frame_id::k_rt_class1_last, [&](const pn::FrameView &f) {
        if (ar.open && f.frame_id == k_output_frame_id)
        {
            const pn::Bytes d = pn::cyclic_data(f);
            std::copy_n(d.begin(), std::min(d.size(), ar.io.size()), ar.io.begin());
        }
    });

    rpc.on(pn::pnio_opnum::k_connect, [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &from) {
        const auto req = p.header.ptype == pn::rpc_ptype::k_request ? pn::parse_ndr_request(p) : std::nullopt;
        const auto blocks = req ? pn::parse_blocks(req->args) : std::nullopt;
        if (!blocks)
        {
            return;
        }
        Ar next;
        ByteVec args;
        for (const pn::PnioBlock &b : *blocks)
        {
            const std::uint8_t *c = b.content.data();
            ByteVec res;
            if (b.type == pn::pnio_block::k_ar_block_req && b.content.size() >= 26)
            {
                next.id.assign(c + 2, c + 20);
                std::copy_n(c + 20, 6, next.controller.begin());
                res.assign(c, c + 2); // ARType
                res.insert(res.end(), next.id.begin(), next.id.end());
                res.insert(res.end(), stack.mac().begin(), stack.mac().end());
                put16(res, pn::k_rpc_port);
            }
            else if (b.type == pn::pnio_block::k_iocr_block_req && b.content.size() >= 18)
            {
                const bool input = get16(c) == 1;
                const std::uint16_t frame_id = input ? get16(c + 12) : k_output_frame_id;
                res.assign(c, c + 4); // IOCRType, IOCRReference
                put16(res, frame_id);
                if (input)
                {
                    next.in_id = frame_id;
                    next.step = static_cast<std::uint16_t>(get16(c + 14) * get16(c + 16));
                }
            }
            else if (b.type == pn::pnio_block::k_alarm_cr_block_req && b.content.size() >= 2)
            {
                res.assign(c, c + 2); // AlarmCRType
                put16(res, 1);        // LocalAlarmReference
                put16(res, 200);      // MaxAlarmDataLength
            }
            else
            {
                continue;
            }
            pn::append_block(args, b.type | pn::pnio_block::k_response_bit, res);
        }
        if (next.id.empty())
        {
            return;
        }
        next.open = true;
        next.period = std::chrono::microseconds(next.step * 125 / 4);
        next.io[4] = next.io[5] = 0x80;
        ar = next;
        rpc.reply(p, from, pn::build_ndr_response(pn::k_pnio_ok, req->args_maximum, args));
    });

    rpc.on(pn::pnio_opnum::k_control, [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &from) {
        const auto req = p.header.ptype == pn::rpc_ptype::k_request ? pn::parse_ndr_request(p) : std::nullopt;
        const auto blocks = req ? pn::parse_blocks(req->args) : std::nullopt;
        if (!ar.open || !blocks || blocks->empty() || (*blocks)[0].type != pn::pnio_block::k_iod_control_req)
        {
            return;
        }
        ByteVec args;
        pn::append_block(
            args, pn::pnio_block::k_iod_control_req | pn::pnio_block::k_response_bit,
            control_content(ar.id, k_control_done));
        rpc.reply(p, from, pn::build_ndr_response(pn::k_pnio_ok, req->args_maximum, args));

        // PrmEnd done: report ApplicationReady and start the input provider.
        pn::RpcHeader h;
        h.object = p.header.object;
        h.interface = pn::rpc_interface::k_controller;
        h.activity.fill(0x42);
        h.opnum = pn::pnio_opnum::k_control;
        h.sequence = ++seq;
        ByteVec ready;
        pn::append_block(
            ready, pn::pnio_block::k_iox_control_req, control_content(ar.id, k_control_app_ready));
        rpc.send({from.ip, pn::k_rpc_port}, h, pn::build_ndr_request(4096, ready));
        ar.cyclic = true;
        ar.next = Clock::now() + ar.period;
    });

    rpc.on(pn::pnio_opnum::k_release, [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &from) {
        const auto req = p.header.ptype == pn::rpc_ptype::k_request ? pn::parse_ndr_request(p) : std::nullopt;
        if (!ar.open || !req)
        {
            return;
        }
        ByteVec args;
        pn::append_block(
            args, pn::pnio_block::k_release_block_req | pn::pnio_block::k_response_bit,
            control_content(ar.id, k_control_done));
        ar = Ar{};
        rpc.reply(p, from, pn::build_ndr_response(pn::k_pnio_ok, req->args_maximum, args));
    });

    std::printf("ready\n");
    std::fflush(stdout);
    for (;;)
    {
        // ponytail: poll the two fds in turn with short timeouts instead of
        // one poll() over both; adds up to ~2 ms latency, fine for a test.
        if (ar.cyclic && Clock::now() >= ar.next)
        {
            ar.counter = static_cast<std::uint16_t>(ar.counter + ar.step);
            ByteVec frame = ar.io;
            put16(frame, ar.counter);
            frame.push_back(k_data_status_ok);
            frame.push_back(0); // TransferStatus
            stack.send(ar.controller, ar.in_id, frame, pn::Vlan{});
            ar.next += ar.period;
        }
        stack.poll(2);
        rpc.poll(0);
    }
}
