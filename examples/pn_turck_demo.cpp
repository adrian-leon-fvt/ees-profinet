/**
 * @file pn_turck_demo.cpp
 * @brief Read-only Turck discovery and acyclic input-status demo.
 */
#include "profinet/rpc.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef PROFINET_NPCAP
#include "npcap_stack.h"
#endif
#else
#include "profinet/stack.h"
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>

namespace pn = profinet;

namespace
{
constexpr std::uint32_t k_xid = 1;
constexpr std::uint16_t k_turck_vendor = 0x013D;
constexpr std::uint16_t k_tbpn_device = 0xB002;
constexpr std::uint16_t k_input_record = 0x8028;

struct DeviceIdentity
{
    std::uint16_t vendor;
    std::uint16_t device;
};

std::uint16_t be16(const pn::Bytes bytes)
{
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::optional<DeviceIdentity> identify(const pn::FrameView &frame)
{
    const auto header = pn::parse_dcp(frame);
    if (frame.frame_id != pn::frame_id::k_dcp_identify_rsp || !header ||
        header->service_id != 5 || header->service_type != 1 ||
        header->xid != k_xid)
    {
        return std::nullopt;
    }
    auto blocks = frame.payload.subspan(10);
    if (header->data_length > blocks.size())
    {
        throw std::invalid_argument("truncated DCP block data");
    }
    blocks = blocks.first(header->data_length);
    std::optional<DeviceIdentity> result;
    while (!blocks.empty())
    {
        if (blocks.size() < 4)
        {
            throw std::invalid_argument("truncated DCP block header");
        }
        const std::size_t length = be16(blocks.subspan(2, 2));
        const std::size_t step = 4 + length + (length & 1);
        if (step > blocks.size())
        {
            throw std::invalid_argument("truncated DCP block or padding");
        }
        if (blocks[0] == 2 && blocks[1] == 3)
        {
            if (length != 6 || result)
            {
                throw std::invalid_argument("invalid/duplicate DeviceID block");
            }
            result = DeviceIdentity{
                be16(blocks.subspan(6, 2)), be16(blocks.subspan(8, 2))};
        }
        blocks = blocks.subspan(step);
    }
    return result;
}

struct InputSnapshot
{
    std::vector<std::uint8_t> iocs;
    std::vector<std::uint8_t> iops;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> read_request(const std::uint16_t slot)
{
    // IODReadReqHeader: zero ARUUID and TargetARUUID for ReadImplicit.
    std::vector<std::uint8_t> content(58, 0);
    content[1] = static_cast<std::uint8_t>(slot); // SequenceNumber
    content[23] = static_cast<std::uint8_t>(slot);
    content[25] = 1; // Subslot 1, API 0.
    content[28] = static_cast<std::uint8_t>(k_input_record >> 8);
    content[29] = static_cast<std::uint8_t>(k_input_record);
    content[32] = 2; // RecordDataLength = 512.
    std::vector<std::uint8_t> args;
    pn::append_block(args, pn::pnio_block::k_iod_read_req_header, content);
    return pn::build_ndr_request(1024, args);
}

InputSnapshot parse_snapshot(
    const pn::RpcPacket &packet, const std::uint16_t slot)
{
    if (packet.header.ptype != pn::rpc_ptype::k_response ||
        (packet.header.flags1 & pn::rpc_flags1::k_fragment) ||
        packet.header.fragment_number != 0)
    {
        throw std::runtime_error("RPC fault/reject or fragmented response");
    }
    const auto response = pn::parse_ndr_response(packet);
    if (!response)
    {
        throw std::runtime_error("invalid NDR response");
    }
    if (response->status != pn::k_pnio_ok)
    {
        char message[96];
        std::snprintf(
            message,
            sizeof(message),
            "PNIO status %02X %02X %02X %02X (record unavailable or denied)",
            response->status[0],
            response->status[1],
            response->status[2],
            response->status[3]);
        throw std::runtime_error(message);
    }
    const auto args = response->args;
    if (args.size() < 64)
    {
        throw std::runtime_error("truncated IODReadResHeader");
    }
    const auto headers = pn::parse_blocks(args.first(64));
    if (!headers || headers->size() != 1 || headers->front().type != 0x8009 ||
        headers->front().version_high != 1 || headers->front().version_low != 0)
    {
        throw std::runtime_error("invalid IODReadResHeader");
    }
    const auto header = headers->front().content;
    const bool zero_context = std::all_of(
        header.begin() + 2, header.begin() + 22, [](const std::uint8_t byte) {
            return byte == 0;
        });
    if (!zero_context || be16(header) != slot ||
        be16(header.subspan(22, 2)) != slot ||
        be16(header.subspan(24, 2)) != 1 ||
        be16(header.subspan(28, 2)) != k_input_record ||
        be16(header.subspan(30, 2)) != 0 ||
        be16(header.subspan(32, 2)) != args.size() - 64)
    {
        throw std::runtime_error(
            "record context/length does not match request");
    }
    const auto records = pn::parse_blocks(args.subspan(64));
    if (!records || records->size() != 1 || records->front().type != 0x0015 ||
        records->front().version_high != 1 || records->front().version_low != 0)
    {
        throw std::runtime_error("missing InputDataObject record");
    }
    auto bytes = records->front().content;
    auto take = [&](const std::size_t length) {
        if (length > bytes.size())
        {
            throw std::runtime_error("truncated InputDataObject");
        }
        std::vector<std::uint8_t> result(bytes.begin(), bytes.begin() + length);
        bytes = bytes.subspan(length);
        return result;
    };
    auto status = [&]() {
        const auto length = take(1).front();
        return take(length);
    };
    InputSnapshot snapshot;
    snapshot.iocs = status();
    snapshot.iops = status();
    if (bytes.size() < 2)
    {
        throw std::runtime_error("missing InputDataObject data length");
    }
    const auto length = be16(bytes);
    (void)take(2);
    snapshot.data = take(length);
    if (!bytes.empty())
    {
        throw std::runtime_error("unexpected InputDataObject trailing bytes");
    }
    return snapshot;
}

InputSnapshot read_inputs(
    const pn::Ipv4Endpoint &device, const std::uint16_t slot)
{
    pn::RpcEndpoint endpoint(0);
    pn::RpcHeader request;
    request.object =
        pn::make_pnio_object_uuid(1, k_tbpn_device, k_turck_vendor);
    request.interface = pn::rpc_interface::k_device;
    request.opnum = pn::pnio_opnum::k_read_implicit;
    request.sequence = slot;
    std::random_device random;
    for (auto &byte : request.activity)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    std::optional<InputSnapshot> result;
    endpoint.on(
        request.opnum,
        [&](const pn::RpcPacket &packet, const pn::Ipv4Endpoint &from) {
            if (from.ip != device.ip ||
                packet.header.activity != request.activity ||
                packet.header.sequence != request.sequence ||
                packet.header.object != request.object ||
                packet.header.interface != request.interface)
            {
                return;
            }
            result = parse_snapshot(packet, slot);
        });
    endpoint.send(device, request, read_request(slot));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!result && std::chrono::steady_clock::now() < deadline)
    {
        endpoint.poll(100);
    }
    if (!result)
    {
        throw std::runtime_error("ReadImplicit timed out after 2 seconds");
    }
    return *result;
}

void print_bytes(const char *label, const pn::Bytes bytes)
{
    std::printf("%s", label);
    for (const auto byte : bytes)
    {
        std::printf(" %02X", byte);
    }
    std::putchar('\n');
}

void print_snapshot(const std::uint16_t slot, const InputSnapshot &snapshot)
{
    constexpr std::size_t k_lengths[] = {0, 6, 14, 2, 2};
    if (snapshot.data.size() != k_lengths[slot])
    {
        throw std::runtime_error("input size differs from selected L5 GSDML");
    }
    print_bytes("  IOCS:", snapshot.iocs);
    print_bytes("  IOPS:", snapshot.iops);
    const bool good =
        !snapshot.iops.empty() &&
        std::all_of(
            snapshot.iops.begin(),
            snapshot.iops.end(),
            [](const std::uint8_t byte) { return (byte & 0x80) != 0; });
    print_bytes("  Raw input:", snapshot.data);
    if (!good)
    {
        std::puts(
            "  Provider status absent/BAD; no live-state interpretation.");
        return;
    }
    const auto &data = snapshot.data;
    if (slot == 1)
    {
        std::printf(
            "  Configured F-input bytes=%02X %02X F-status=%02X\n"
            "  RAW ONLY: no F-CRC, sequence or watchdog validation.\n",
            data[0],
            data[1],
            data[2]);
    }
    else if (slot == 2)
    {
        std::printf(
            "  Unit: protective=%u configuration=%u unknown=%u\n",
            data[0] & 1,
            (data[0] >> 1) & 1,
            (data[0] >> 2) & 1);
        for (std::size_t connector = 0; connector < 8; ++connector)
        {
            const auto flags = data[6 + connector];
            std::printf(
                "  C%zu: RGG=%u WAIT=%u TEST=%u ERRFIN=%u "
                "TCCH0=%u TCCH1=%u OVL=%u\n",
                connector,
                flags & 1,
                (flags >> 1) & 1,
                (flags >> 2) & 1,
                (flags >> 3) & 1,
                (flags >> 4) & 1,
                (flags >> 5) & 1,
                (flags >> 7) & 1);
        }
    }
    else if (slot == 3)
    {
        std::printf("  Configured fieldbus bits FBO0..15:");
        for (std::size_t bit = 0; bit < 16; ++bit)
        {
            std::printf(" %u", (data[bit / 8] >> (bit % 8)) & 1);
        }
        std::putchar('\n');
    }
}

int show_status(const char *ip)
{
    pn::Ipv4Endpoint device;
    if (::inet_pton(AF_INET, ip, device.ip.data()) != 1)
    {
        throw std::invalid_argument("--status requires a numeric IPv4 address");
    }
    std::puts("Read-only input snapshots; NOT safety-validated or atomic.");
    bool failed = false;
    for (std::uint16_t slot = 1; slot <= 4; ++slot)
    {
        std::printf("Slot %u / subslot 1 (index 0x8028):\n", slot);
        try
        {
            print_snapshot(slot, read_inputs(device, slot));
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "Slot %u: %s\n", slot, error.what());
            failed = true;
        }
    }
    if (failed)
    {
        std::fputs(
            "Some inputs unavailable. Firmware may require a configured "
            "controller/active IOCR or reject acyclic input reads. "
            "No connection, parameter or output writes were attempted.\n",
            stderr);
    }
    return failed ? 2 : 0;
}

void require(const bool condition)
{
    if (!condition)
    {
        throw std::runtime_error("discovery self-test failed");
    }
}

void self_test()
{
    const std::vector<std::uint8_t> response{5, 1, 0,    0,    0,    1,   0,
                                             0, 0, 10,   2,    3,    0,   6,
                                             0, 0, 0x01, 0x3D, 0xB0, 0x02};
    auto decode = [](const std::vector<std::uint8_t> &payload) {
        const auto raw = pn::build(
            pn::Mac{}, pn::Mac{}, pn::frame_id::k_dcp_identify_rsp, payload);
        return identify(*pn::parse(raw));
    };
    const auto identity = decode(response);
    require(
        identity && identity->vendor == k_turck_vendor &&
        identity->device == k_tbpn_device);
    auto unrelated = response;
    unrelated[5] = 2;
    require(!decode(unrelated));
    auto padded = response;
    padded[9] = 16;
    padded.insert(padded.end(), {2, 2, 0, 1, 'x', 0});
    require(decode(padded).has_value());
    auto duplicate = response;
    duplicate[9] = 20;
    duplicate.insert(duplicate.end(), response.begin() + 10, response.end());
    auto short_header = response;
    short_header[9] = 11;
    auto short_block = response;
    short_block[13] = 40;
    auto short_payload = response;
    short_payload[9] = 100;
    auto short_padding = response;
    short_padding[9] = 15;
    short_padding.insert(short_padding.end(), {2, 2, 0, 1, 'x'});
    for (const auto &bad :
         {duplicate, short_header, short_block, short_payload, short_padding})
    {
        bool rejected = false;
        try
        {
            (void)decode(bad);
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        require(rejected);
    }
    const auto wire = pn::build_rpc(pn::RpcHeader{}, read_request(2));
    const auto packet = pn::parse_rpc(wire);
    require(packet.has_value());
    const auto ndr = pn::parse_ndr_request(*packet);
    require(ndr.has_value());
    const auto request = pn::parse_blocks(ndr->args);
    require(request && request->size() == 1);
    require(request->front().type == pn::pnio_block::k_iod_read_req_header);
    require(request->front().content.size() == 58);
    require(be16(request->front().content.subspan(22, 2)) == 2);
    require(be16(request->front().content.subspan(28, 2)) == k_input_record);

    std::puts("Discovery/request self-test passed (no network access).");
}
} // namespace

int main(const int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--self-test")
        {
            self_test();
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--status")
        {
            return show_status(argv[2]);
        }
#ifdef PROFINET_NPCAP
        if (argc == 2 && std::string(argv[1]) == "--list")
        {
            profinet_example::NpcapStack::list();
            return 0;
        }
#endif
        if (argc != 2)
        {
            std::fprintf(
                stderr,
                "usage: %s <interface> | --status <ip> | --self-test"
#ifdef PROFINET_NPCAP
                " | --list"
#endif
                "\n",
                argv[0]);
            return 1;
        }

#if defined(_WIN32) && !defined(PROFINET_NPCAP)
        throw std::runtime_error(
            "Discovery requires the Npcap SDK at build time and Npcap runtime. "
            "Set NPCAP_SDK, reconfigure and rebuild. --status needs no Npcap.");
#else
#ifdef PROFINET_NPCAP
        profinet_example::NpcapStack stack(argv[1]);
#else
        pn::Stack stack(argv[1]);
#endif
        bool found = false;
        stack.on(
            pn::frame_id::k_dcp_identify_rsp, [&](const pn::FrameView &frame) {
                std::optional<DeviceIdentity> identity;
                try
                {
                    identity = identify(frame);
                }
                catch (const std::invalid_argument &error)
                {
                    std::fprintf(
                        stderr, "Rejected DCP response: %s\n", error.what());
                    return;
                }
                if (!identity)
                {
                    return;
                }
                const bool target = identity->vendor == k_turck_vendor &&
                                    identity->device == k_tbpn_device;
                found = found || target;
                std::printf(
                    "%02X:%02X:%02X:%02X:%02X:%02X vendor=%04X device=%04X%s\n",
                    frame.src[0],
                    frame.src[1],
                    frame.src[2],
                    frame.src[3],
                    frame.src[4],
                    frame.src[5],
                    identity->vendor,
                    identity->device,
                    target ? " TBPN-L5-4FDI-4FDX identity matched" : "");
            });

        const std::uint8_t request[] = {
            5, 0, 0, 0, 0, 1, 0, 128, 0, 4, 0xFF, 0xFF, 0, 0};
        std::puts("Sending read-only DCP Identify-All; waiting 2 seconds.");
        stack.send(
            pn::k_multicast_dcp, pn::frame_id::k_dcp_identify_req, request);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            stack.poll(100);
        }
        if (!found)
        {
            std::fputs(
                "No matching Turck identity received; check interface, power "
                "and layer-2 connectivity.\n",
                stderr);
            return 2;
        }
        return 0;
#endif
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "pn_turck_demo: %s\n", error.what());
        return 1;
    }
}
