/**
 * @file rpc.h
 * @brief PROFINET IO connection-management transport: DCE/RPC (connectionless,
 *        over UDP), the NDR argument wrapper and PNIO block framing, plus a
 *        UDP endpoint with opnum-based hooks.
 *
 * This is the transport for Connect / Release / Read / Write / Control.
 * The meaning of the blocks (AR, IOCR, expected submodules, ...) and the CM
 * state machine are left to the application.
 */
#ifndef PROFINET_RPC_H
#define PROFINET_RPC_H

#include "profinet/frame.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace profinet
{

/** @brief UUID in canonical (as-written, big-endian) byte order. */
using Uuid = std::array<std::uint8_t, 16>;

constexpr std::uint16_t k_rpc_port = 34964;
/** @brief Range used for PROFINET's ephemeral RPC ports. */
constexpr std::uint16_t k_ephemeral_port_first = 49152;
constexpr std::uint16_t k_ephemeral_port_last = 65535;

/** @brief PNIO RPC interface UUIDs (DEA0000x-6C97-11D1-8271-00A02442DF7D). */
namespace rpc_interface
{
constexpr Uuid k_device{0xDE, 0xA0, 0x00, 0x01, 0x6C, 0x97, 0x11, 0xD1,
                        0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
constexpr Uuid k_controller{0xDE, 0xA0, 0x00, 0x02, 0x6C, 0x97, 0x11, 0xD1,
                            0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
constexpr Uuid k_supervisor{0xDE, 0xA0, 0x00, 0x03, 0x6C, 0x97, 0x11, 0xD1,
                            0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
constexpr Uuid k_parameter_server{0xDE, 0xA0, 0x00, 0x04, 0x6C, 0x97, 0x11, 0xD1,
                                  0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
} // namespace rpc_interface

/** @brief DCE/RPC connectionless packet types. */
namespace rpc_ptype
{
constexpr std::uint8_t k_request = 0;
constexpr std::uint8_t k_ping = 1;
constexpr std::uint8_t k_response = 2;
constexpr std::uint8_t k_fault = 3;
constexpr std::uint8_t k_working = 4;
constexpr std::uint8_t k_nocall = 5;
constexpr std::uint8_t k_reject = 6;
constexpr std::uint8_t k_ack = 7;
constexpr std::uint8_t k_fack = 9;
} // namespace rpc_ptype

/** @brief DCE/RPC flags1 bits. */
namespace rpc_flags1
{
constexpr std::uint8_t k_last_fragment = 0x02;
constexpr std::uint8_t k_fragment = 0x04;
constexpr std::uint8_t k_no_fack = 0x08;
constexpr std::uint8_t k_maybe = 0x10;
constexpr std::uint8_t k_idempotent = 0x20;
constexpr std::uint8_t k_broadcast = 0x40;
} // namespace rpc_flags1

/** @brief PNIO operation numbers. */
namespace pnio_opnum
{
constexpr std::uint16_t k_connect = 0;
constexpr std::uint16_t k_release = 1;
constexpr std::uint16_t k_read = 2;
constexpr std::uint16_t k_write = 3;
constexpr std::uint16_t k_control = 4;
constexpr std::uint16_t k_read_implicit = 5;
} // namespace pnio_opnum

/** @brief Common PNIO block types (responses = request | 0x8000). */
namespace pnio_block
{
constexpr std::uint16_t k_iod_write_req_header = 0x0008;
constexpr std::uint16_t k_iod_read_req_header = 0x0009;
constexpr std::uint16_t k_ar_block_req = 0x0101;
constexpr std::uint16_t k_iocr_block_req = 0x0102;
constexpr std::uint16_t k_alarm_cr_block_req = 0x0103;
constexpr std::uint16_t k_expected_submodule_block_req = 0x0104;
constexpr std::uint16_t k_iod_control_req = 0x0110;     ///< PrmEnd
constexpr std::uint16_t k_iox_control_req = 0x0112;     ///< ApplicationReady
constexpr std::uint16_t k_release_block_req = 0x0114;
constexpr std::uint16_t k_response_bit = 0x8000;
} // namespace pnio_block

/** @brief PNIOStatus: ErrorCode, ErrorDecode, ErrorCode1, ErrorCode2. */
using PnioStatus = std::array<std::uint8_t, 4>;
constexpr PnioStatus k_pnio_ok{0, 0, 0, 0};

/** @brief DCE/RPC connectionless header (80 bytes on the wire). */
struct RpcHeader
{
    std::uint8_t ptype = rpc_ptype::k_request;
    std::uint8_t flags1 = rpc_flags1::k_idempotent;
    std::uint8_t flags2 = 0;
    Uuid object{};
    Uuid interface{};
    Uuid activity{};
    std::uint32_t server_boot = 0;
    std::uint32_t interface_version = 1;
    std::uint32_t sequence = 0;
    std::uint16_t opnum = 0;
    std::uint16_t interface_hint = 0xFFFF;
    std::uint16_t activity_hint = 0xFFFF;
    std::uint16_t fragment_number = 0;
};

/** @brief Parsed RPC datagram; @p body spans the receive buffer. */
struct RpcPacket
{
    RpcHeader header;
    bool little_endian = true; ///< Sender's integer representation (drep).
    Bytes body;
};

/** @brief NDR-wrapped request arguments (Connect/Read/Write/Control). */
struct NdrRequest
{
    std::uint32_t args_maximum = 0;
    Bytes args; ///< PNIO blocks.
};

/** @brief NDR-wrapped response arguments. */
struct NdrResponse
{
    PnioStatus status{};
    std::uint32_t args_maximum = 0;
    Bytes args; ///< PNIO blocks.
};

/** @brief One PNIO block; @p content spans the input buffer. */
struct PnioBlock
{
    std::uint16_t type = 0;
    std::uint8_t version_high = 1;
    std::uint8_t version_low = 0;
    Bytes content; ///< After BlockVersionLow.
};

/** @brief IPv4 address + UDP port. */
struct Ipv4Endpoint
{
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = k_rpc_port;
    bool operator==(const Ipv4Endpoint &) const = default;
};

/** @brief PNIO object UUID DEA00000-6C97-11D1-8271-IIII-DDDD-VVVV. */
Uuid make_pnio_object_uuid(
    std::uint16_t instance,
    std::uint16_t device_id,
    std::uint16_t vendor_id);

/**
 * @brief Parse an RPC datagram (UDP payload). Accepts both byte orders.
 * @return nullopt if shorter than 80 bytes, not RPC version 4, unknown drep
 *         or the body length overruns the datagram.
 */
std::optional<RpcPacket> parse_rpc(Bytes datagram);

/** @brief Encode header + body (always little-endian drep). */
std::vector<std::uint8_t> build_rpc(const RpcHeader &header, Bytes body);

/**
 * @brief Response header for @p request: same UUIDs, sequence and opnum;
 *        ptype response; flags1 last-fragment | no-fack.
 */
RpcHeader make_response(const RpcHeader &request);

/** @brief Decode NDR request wrapper; nullopt if truncated. */
std::optional<NdrRequest> parse_ndr_request(const RpcPacket &packet);

/** @brief Decode NDR response wrapper; nullopt if truncated. */
std::optional<NdrResponse> parse_ndr_response(const RpcPacket &packet);

/** @brief Encode NDR request wrapper (little-endian) around @p args. */
std::vector<std::uint8_t> build_ndr_request(std::uint32_t args_maximum, Bytes args);

/** @brief Encode NDR response wrapper (little-endian) around @p args. */
std::vector<std::uint8_t> build_ndr_response(
    const PnioStatus &status,
    std::uint32_t args_maximum,
    Bytes args);

/** @brief Split PNIO blocks (big-endian); nullopt if a block overruns. */
std::optional<std::vector<PnioBlock>> parse_blocks(Bytes args);

/** @brief Append one PNIO block header + @p content to @p out. */
void append_block(
    std::vector<std::uint8_t> &out,
    std::uint16_t type,
    Bytes content,
    std::uint8_t version_high = 1,
    std::uint8_t version_low = 0);

/** @brief Callback for each received RPC datagram. */
using RpcHook = std::function<void(const RpcPacket &, const Ipv4Endpoint &from)>;

/**
 * @brief UDP socket speaking PNIO RPC, with opnum-based hook dispatch.
 * @note Register hooks before poll()/run(); registration is not thread-safe.
 *       No privileges needed.
 */
class RpcEndpoint
{
  public:
    /**
     * @param port UDP port; k_rpc_port for a device / server, or 0 for the
     *             first free port in 49152–65535 (controller side, or a
     *             device's own calls such as ApplicationReady).
     * @param bind_ip Local IPv4 address to bind ("0.0.0.0" = all).
     * @throws std::system_error on socket/bind failure,
     *         std::invalid_argument on a malformed @p bind_ip.
     */
    explicit RpcEndpoint(
        std::uint16_t port = k_rpc_port,
        const std::string &bind_ip = "0.0.0.0");
    virtual ~RpcEndpoint();
    RpcEndpoint(const RpcEndpoint &) = delete;
    RpcEndpoint &operator=(const RpcEndpoint &) = delete;

    /** @brief Call @p hook for datagrams with this opnum (any ptype). */
    void on(std::uint16_t opnum, RpcHook hook);

    /** @brief Call @p hook for every valid RPC datagram. */
    void on_any(RpcHook hook);

    /**
     * @brief Wait up to @p timeout_ms, then dispatch all queued datagrams.
     * @return Number of valid RPC datagrams dispatched.
     */
    int poll(int timeout_ms);

    /** @brief Loop poll() until stop(). */
    void run();

    /** @brief Make run() return (any thread / signal handler). */
    void stop();

    /** @brief Send @p header + @p body to @p to. */
    void send(const Ipv4Endpoint &to, const RpcHeader &header, Bytes body);

    /**
     * @brief Answer @p request: make_response() + this endpoint's
     *        server_boot, sent to @p to.
     */
    void reply(const RpcPacket &request, const Ipv4Endpoint &to, Bytes body);

    /** @return Bound UDP port. */
    std::uint16_t port() const
    {
        return port_;
    }

    /** @return Boot time stamp placed in responses. */
    std::uint32_t server_boot() const
    {
        return server_boot_;
    }

    /** @return Socket fd for an external event loop + poll(0). */
    int fd() const
    {
        return fd_;
    }

  private:
    struct Entry
    {
        int opnum; ///< -1 = any.
        RpcHook hook;
    };

    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::uint32_t server_boot_ = 0;
    std::vector<Entry> hooks_;
    std::vector<std::uint8_t> buf_;
    std::atomic<bool> running_{false};
};

} // namespace profinet

#endif // PROFINET_RPC_H
