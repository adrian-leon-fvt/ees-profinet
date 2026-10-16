/**
 * @file test_rpc.cpp
 * @brief Unit tests for DCE/RPC, NDR, PNIO blocks and RpcEndpoint.
 */
#include "profinet/rpc.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace profinet;

namespace
{
RpcHeader sample_header()
{
    RpcHeader h;
    h.object = make_pnio_object_uuid(1, 0x0203, 0x002A);
    h.interface = rpc_interface::k_device;
    h.activity = Uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    h.sequence = 7;
    h.opnum = pnio_opnum::k_connect;
    return h;
}

void put(std::vector<std::uint8_t> &out, std::uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
    {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
} // namespace

TEST(Rpc, PnioObjectUuidLayout)
{
    const Uuid u = make_pnio_object_uuid(0x0001, 0x0203, 0x002A);

    const Uuid expected{0xDE, 0xA0, 0x00, 0x00, 0x6C, 0x97, 0x11, 0xD1,
                        0x82, 0x71, 0x00, 0x01, 0x02, 0x03, 0x00, 0x2A};
    EXPECT_EQ(u, expected);
}

TEST(Rpc, BuildIsLittleEndianAndRoundTrips)
{
    const RpcHeader h = sample_header();
    const std::vector<std::uint8_t> body{0xAA, 0xBB, 0xCC};

    const auto bytes = build_rpc(h, body);
    const auto p = parse_rpc(bytes);

    ASSERT_EQ(bytes.size(), 83u);
    EXPECT_EQ(bytes[4], 0x10); // drep: little-endian
    // Interface UUID DEA00001-6C97-11D1-...: first three fields byte-swapped
    EXPECT_EQ(bytes[24], 0x01);
    EXPECT_EQ(bytes[27], 0xDE);
    EXPECT_EQ(bytes[28], 0x97);
    EXPECT_EQ(bytes[30], 0xD1);
    EXPECT_EQ(bytes[32], 0x82);
    ASSERT_TRUE(p);
    EXPECT_TRUE(p->little_endian);
    EXPECT_EQ(p->header.object, h.object);
    EXPECT_EQ(p->header.interface, rpc_interface::k_device);
    EXPECT_EQ(p->header.activity, h.activity);
    EXPECT_EQ(p->header.sequence, 7u);
    EXPECT_EQ(p->header.opnum, pnio_opnum::k_connect);
    EXPECT_EQ(p->header.flags1, rpc_flags1::k_idempotent);
    EXPECT_EQ(std::vector<std::uint8_t>(p->body.begin(), p->body.end()), body);
}

TEST(Rpc, ParsesHandEncodedBigEndianPacket)
{
    // Arrange: header encoded independently of build_rpc, drep = big-endian
    std::vector<std::uint8_t> d{4, rpc_ptype::k_response, 0x0A, 0, 0x00, 0, 0, 0};
    const Uuid obj = make_pnio_object_uuid(1, 2, 3);
    d.insert(d.end(), obj.begin(), obj.end());
    d.insert(d.end(), rpc_interface::k_controller.begin(), rpc_interface::k_controller.end());
    d.insert(d.end(), 16, 0x55);
    put(d, 0x11223344, 4); // server_boot
    put(d, 1, 4);          // interface version
    put(d, 0x01020304, 4); // sequence
    put(d, pnio_opnum::k_write, 2);
    put(d, 0xFFFF, 2);
    put(d, 0xFFFF, 2);
    put(d, 2, 2); // body length
    put(d, 0, 2); // fragment
    d.push_back(0);
    d.push_back(0);
    d.push_back(0xEE);
    d.push_back(0xFF);
    d.push_back(0x99); // trailing junk beyond body length is ignored

    // Act
    const auto p = parse_rpc(d);

    // Assert
    ASSERT_TRUE(p);
    EXPECT_FALSE(p->little_endian);
    EXPECT_EQ(p->header.ptype, rpc_ptype::k_response);
    EXPECT_EQ(p->header.object, obj);
    EXPECT_EQ(p->header.interface, rpc_interface::k_controller);
    EXPECT_EQ(p->header.server_boot, 0x11223344u);
    EXPECT_EQ(p->header.sequence, 0x01020304u);
    EXPECT_EQ(p->header.opnum, pnio_opnum::k_write);
    ASSERT_EQ(p->body.size(), 2u);
    EXPECT_EQ(p->body[1], 0xFF);
}

TEST(Rpc, RejectsShortWrongVersionAndOverrun)
{
    auto bytes = build_rpc(sample_header(), std::vector<std::uint8_t>(4, 0));
    EXPECT_FALSE(parse_rpc(Bytes(bytes.data(), 79)));
    EXPECT_FALSE(parse_rpc(Bytes(bytes.data(), 83))); // body length 4, only 3 present
    bytes[0] = 5;
    EXPECT_FALSE(parse_rpc(bytes));
}

TEST(Rpc, MakeResponseKeepsIdentityAndSetsFlags)
{
    const RpcHeader req = sample_header();

    const RpcHeader rsp = make_response(req);

    EXPECT_EQ(rsp.ptype, rpc_ptype::k_response);
    EXPECT_EQ(rsp.flags1, rpc_flags1::k_last_fragment | rpc_flags1::k_no_fack);
    EXPECT_EQ(rsp.activity, req.activity);
    EXPECT_EQ(rsp.object, req.object);
    EXPECT_EQ(rsp.interface, req.interface);
    EXPECT_EQ(rsp.sequence, req.sequence);
    EXPECT_EQ(rsp.opnum, req.opnum);
}

TEST(Rpc, NdrAndBlocksRoundTrip)
{
    std::vector<std::uint8_t> blocks;
    append_block(blocks, pnio_block::k_ar_block_req, std::vector<std::uint8_t>{1, 2, 3});
    append_block(blocks, pnio_block::k_iocr_block_req, {});

    const auto req_body = build_ndr_request(1024, blocks);
    const auto req = parse_rpc(build_rpc(sample_header(), req_body));
    ASSERT_TRUE(req);
    const auto ndr = parse_ndr_request(*req);
    ASSERT_TRUE(ndr);
    EXPECT_EQ(ndr->args_maximum, 1024u);
    const auto parsed = parse_blocks(ndr->args);

    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 2u);
    EXPECT_EQ((*parsed)[0].type, pnio_block::k_ar_block_req);
    EXPECT_EQ((*parsed)[0].version_high, 1);
    ASSERT_EQ((*parsed)[0].content.size(), 3u);
    EXPECT_EQ((*parsed)[0].content[2], 3);
    EXPECT_EQ((*parsed)[1].type, pnio_block::k_iocr_block_req);
    EXPECT_TRUE((*parsed)[1].content.empty());

    const PnioStatus err{0xDB, 0x81, 0x01, 0x02};
    const auto rsp = parse_rpc(build_rpc(make_response(sample_header()),
                                         build_ndr_response(err, 512, blocks)));
    ASSERT_TRUE(rsp);
    const auto ndr_rsp = parse_ndr_response(*rsp);
    ASSERT_TRUE(ndr_rsp);
    EXPECT_EQ(ndr_rsp->status, err);
    EXPECT_EQ(ndr_rsp->args_maximum, 512u);
    EXPECT_EQ(ndr_rsp->args.size(), blocks.size());
}

TEST(Rpc, RejectsTruncatedNdrAndBlocks)
{
    auto body = build_ndr_request(64, std::vector<std::uint8_t>(8, 0));
    body.resize(body.size() - 1); // ActualCount now exceeds remaining bytes
    const auto p = parse_rpc(build_rpc(sample_header(), body));
    ASSERT_TRUE(p);
    EXPECT_FALSE(parse_ndr_request(*p));

    std::vector<std::uint8_t> blocks;
    append_block(blocks, 0x0101, std::vector<std::uint8_t>(10, 0));
    blocks.pop_back();
    EXPECT_FALSE(parse_blocks(blocks));
}

TEST(RpcEndpoint, RequestReplyOverLoopback)
{
    // Arrange: server on an ephemeral port, client on another
    RpcEndpoint server(0, "127.0.0.1");
    RpcEndpoint client(0, "127.0.0.1");
    EXPECT_GE(server.port(), k_ephemeral_port_first);
    EXPECT_NE(server.port(), client.port());

    server.on(pnio_opnum::k_connect, [&](const RpcPacket &req, const Ipv4Endpoint &from) {
        const auto ndr = parse_ndr_request(req);
        ASSERT_TRUE(ndr);
        server.reply(req, from, build_ndr_response(k_pnio_ok, ndr->args_maximum, ndr->args));
    });
    int wrong_opnum = 0;
    server.on(pnio_opnum::k_read, [&](const auto &, const auto &) { ++wrong_opnum; });

    std::optional<RpcHeader> got;
    std::vector<std::uint8_t> echoed;
    client.on_any([&](const RpcPacket &rsp, const Ipv4Endpoint &from) {
        EXPECT_EQ(from.port, server.port());
        got = rsp.header;
        const auto ndr = parse_ndr_response(rsp);
        ASSERT_TRUE(ndr);
        echoed.assign(ndr->args.begin(), ndr->args.end());
    });

    std::vector<std::uint8_t> blocks;
    append_block(blocks, pnio_block::k_ar_block_req, std::vector<std::uint8_t>{9, 9});

    // Act
    client.send({{127, 0, 0, 1}, server.port()}, sample_header(), build_ndr_request(256, blocks));
    EXPECT_EQ(server.poll(1000), 1);
    EXPECT_EQ(client.poll(1000), 1);

    // Assert
    EXPECT_EQ(wrong_opnum, 0);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->ptype, rpc_ptype::k_response);
    EXPECT_EQ(got->activity, sample_header().activity);
    EXPECT_EQ(got->server_boot, server.server_boot());
    EXPECT_EQ(echoed, blocks);
}

TEST(RpcEndpoint, IgnoresNonRpcDatagramsAndRejectsBadAddress)
{
    RpcEndpoint server(0, "127.0.0.1");
    RpcEndpoint client(0, "127.0.0.1");
    int calls = 0;
    server.on_any([&](const auto &, const auto &) { ++calls; });

    client.send({{127, 0, 0, 1}, server.port()}, sample_header(), {});
    // Corrupt datagram via a raw send of garbage through a second socket
    RpcEndpoint junk(0, "127.0.0.1");
    const std::vector<std::uint8_t> garbage(10, 0x42);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(server.port());
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_GT(::sendto(junk.fd(), garbage.data(), garbage.size(), 0,
                       reinterpret_cast<sockaddr *>(&to), sizeof(to)), 0);

    int total = 0;
    for (int i = 0; i < 5 && total < 1; ++i)
    {
        total += server.poll(200);
    }
    server.poll(50);

    EXPECT_EQ(calls, 1);
    EXPECT_THROW(RpcEndpoint(0, "not-an-ip"), std::invalid_argument);
}
