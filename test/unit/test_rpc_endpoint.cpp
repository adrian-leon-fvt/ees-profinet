/**
 * @file test_rpc_endpoint.cpp
 * @brief Unit tests for the POSIX RpcEndpoint over 127.0.0.1.
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
} // namespace

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
