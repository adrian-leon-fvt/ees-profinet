/**
 * @file pn_windows_udp.cpp
 * @brief Windows example: carry complete Ethernet frames over UDP.
 *
 * This is a small transport adapter for testing Dispatcher on Windows.
 * Replace the UDP send/receive calls with the chosen Ethernet capture driver
 * when connecting to real PROFINET hardware.
 *
 * Usage: pn_windows_udp [port]
 */
#include "profinet/dispatcher.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void fail(const char *what)
{
    std::fprintf(stderr, "%s failed: %d\n", what, WSAGetLastError());
    std::exit(1);
}
} // namespace

int main(const int argc, char **argv)
{
    const auto requested_port = static_cast<unsigned short>(
        argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 0);

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    const SOCKET socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET)
    {
        fail("socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(requested_port);
    if (::bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
        SOCKET_ERROR)
    {
        fail("bind");
    }

    int address_size = sizeof(address);
    if (::getsockname(
            socket_fd,
            reinterpret_cast<sockaddr *>(&address),
            &address_size) == SOCKET_ERROR)
    {
        fail("getsockname");
    }
    std::printf("listening on UDP 127.0.0.1:%u\n", ntohs(address.sin_port));

    profinet::Dispatcher dispatcher;
    dispatcher.on(profinet::frame_id::k_dcp_identify_req, [](const profinet::FrameView &frame) {
        std::printf(
            "received DCP Identify request from %02X:%02X:%02X:%02X:%02X:%02X\n",
            frame.src[0],
            frame.src[1],
            frame.src[2],
            frame.src[3],
            frame.src[4],
            frame.src[5]);
    });

    const profinet::Mac source{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    const std::uint8_t identify_all[] = {
        0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x04, 0xFF, 0xFF, 0x00, 0x00};
    const auto frame = profinet::build(
        profinet::k_multicast_dcp,
        source,
        profinet::frame_id::k_dcp_identify_req,
        identify_all);

    if (::sendto(
            socket_fd,
            reinterpret_cast<const char *>(frame.data()),
            static_cast<int>(frame.size()),
            0,
            reinterpret_cast<const sockaddr *>(&address),
            sizeof(address)) == SOCKET_ERROR)
    {
        fail("sendto");
    }

    DWORD timeout_ms = 1000;
    if (::setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char *>(&timeout_ms),
            sizeof(timeout_ms)) == SOCKET_ERROR)
    {
        fail("setsockopt");
    }

    std::vector<std::uint8_t> received(2048);
    const int size = ::recv(socket_fd, reinterpret_cast<char *>(received.data()),
                            static_cast<int>(received.size()), 0);
    if (size == SOCKET_ERROR)
    {
        fail("recv");
    }
    if (!dispatcher.feed(profinet::Bytes(received.data(), static_cast<std::size_t>(size))))
    {
        std::fprintf(stderr, "dispatcher rejected received frame\n");
        return 1;
    }

    ::closesocket(socket_fd);
    WSACleanup();
    return 0;
}
