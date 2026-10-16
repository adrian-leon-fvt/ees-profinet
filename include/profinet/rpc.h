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

/** @brief UDP port a device (RPC server) listens on. */
constexpr std::uint16_t k_rpc_port = 34964;
/** @name Ephemeral port range used by the side initiating a call */
///@{
/** @brief First ephemeral port. */
constexpr std::uint16_t k_ephemeral_port_first = 49152;
constexpr std::uint16_t k_ephemeral_port_last = 65535; ///< Last ephemeral port.
///@}

/** @brief PNIO RPC interface UUIDs (DEA0000x-6C97-11D1-8271-00A02442DF7D). */
namespace rpc_interface
{
/** @name PNIO RPC interface UUIDs */
///@{
/** @brief IO device interface (DEA00001-...). */
constexpr Uuid k_device{0xDE, 0xA0, 0x00, 0x01, 0x6C, 0x97, 0x11, 0xD1,
                        0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
/** @brief IO controller interface (DEA00002-...). */
constexpr Uuid k_controller{0xDE, 0xA0, 0x00, 0x02, 0x6C, 0x97, 0x11, 0xD1,
                            0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
/** @brief IO supervisor interface (DEA00003-...). */
constexpr Uuid k_supervisor{0xDE, 0xA0, 0x00, 0x03, 0x6C, 0x97, 0x11, 0xD1,
                            0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
/** @brief Parameter server interface (DEA00004-...). */
constexpr Uuid k_parameter_server{0xDE, 0xA0, 0x00, 0x04, 0x6C, 0x97, 0x11, 0xD1,
                                  0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};
///@}
} // namespace rpc_interface

/** @brief DCE/RPC connectionless packet types. */
namespace rpc_ptype
{
/** @name Packet types (RpcHeader::ptype) */
///@{
constexpr std::uint8_t k_request = 0; ///< Call.
constexpr std::uint8_t k_ping = 1; ///< Client asks if call is alive.
constexpr std::uint8_t k_response = 2; ///< Reply to a request.
constexpr std::uint8_t k_fault = 3; ///< Server-side call failure.
constexpr std::uint8_t k_working = 4; ///< Reply to ping: still processing.
constexpr std::uint8_t k_nocall = 5; ///< Reply to ping: call unknown.
constexpr std::uint8_t k_reject = 6; ///< Call rejected (protocol error).
constexpr std::uint8_t k_ack = 7; ///< Client acknowledges a response.
constexpr std::uint8_t k_fack = 9; ///< Fragment acknowledgement.
///@}
} // namespace rpc_ptype

/** @brief DCE/RPC flags1 bits. */
namespace rpc_flags1
{
/** @name Flag bits (RpcHeader::flags1) */
///@{
constexpr std::uint8_t k_last_fragment = 0x02; ///< Last fragment of the body.
constexpr std::uint8_t k_fragment = 0x04; ///< Body is fragmented.
/** @brief Sender does not want fragment acks. */
constexpr std::uint8_t k_no_fack = 0x08;
constexpr std::uint8_t k_maybe = 0x10; ///< No response expected.
/** @brief Call may be executed more than once. */
constexpr std::uint8_t k_idempotent = 0x20;
constexpr std::uint8_t k_broadcast = 0x40; ///< Broadcast call.
///@}
} // namespace rpc_flags1

/** @brief PNIO operation numbers. */
namespace pnio_opnum
{
/** @name Operation numbers (RpcHeader::opnum) */
///@{
constexpr std::uint16_t k_connect = 0; ///< Establish an AR.
constexpr std::uint16_t k_release = 1; ///< Release an AR.
constexpr std::uint16_t k_read = 2; ///< Read record data.
constexpr std::uint16_t k_write = 3; ///< Write record data.
constexpr std::uint16_t k_control = 4; ///< PrmEnd, ApplicationReady, ...
constexpr std::uint16_t k_read_implicit = 5; ///< Read without an AR.
///@}
} // namespace pnio_opnum

/** @brief Common PNIO block types (responses = request | 0x8000). */
namespace pnio_block
{
/** @name Block types */
///@{
constexpr std::uint16_t k_iod_write_req_header = 0x0008; ///< IODWriteReqHeader.
constexpr std::uint16_t k_iod_read_req_header = 0x0009; ///< IODReadReqHeader.
constexpr std::uint16_t k_ar_block_req = 0x0101; ///< ARBlockReq.
constexpr std::uint16_t k_iocr_block_req = 0x0102; ///< IOCRBlockReq.
constexpr std::uint16_t k_alarm_cr_block_req = 0x0103; ///< AlarmCRBlockReq.
/** @brief ExpectedSubmoduleBlockReq. */
constexpr std::uint16_t k_expected_submodule_block_req = 0x0104;
constexpr std::uint16_t k_iod_control_req = 0x0110;     ///< PrmEnd
constexpr std::uint16_t k_iox_control_req = 0x0112;     ///< ApplicationReady
constexpr std::uint16_t k_release_block_req = 0x0114; ///< ReleaseBlockReq.
/** @brief OR into a request type to get the response type. */
constexpr std::uint16_t k_response_bit = 0x8000;
///@}
} // namespace pnio_block

/** @brief PNIOStatus: ErrorCode, ErrorDecode, ErrorCode1, ErrorCode2. */
using PnioStatus = std::array<std::uint8_t, 4>;
/** @brief Success status. */
constexpr PnioStatus k_pnio_ok{0, 0, 0, 0};

/** @brief DCE/RPC connectionless header (80 bytes on the wire). */
struct RpcHeader
{
    std::uint8_t ptype = rpc_ptype::k_request;      ///< rpc_ptype::k_*.
    std::uint8_t flags1 = rpc_flags1::k_idempotent; ///< rpc_flags1 bits.
    std::uint8_t flags2 = 0;                        ///< Unused by PNIO.
    Uuid object{};    ///< Target object; see make_pnio_object_uuid().
    Uuid interface{}; ///< rpc_interface::k_*.
    Uuid activity{};  ///< Client call identity; random per client.
    std::uint32_t server_boot = 0; ///< Server boot time; 0 in requests.
    std::uint32_t interface_version = 1; ///< 1 for PNIO.
    std::uint32_t sequence = 0; ///< Call number within the activity.
    std::uint16_t opnum = 0;    ///< pnio_opnum::k_*.
    std::uint16_t interface_hint = 0xFFFF; ///< 0xFFFF = no hint.
    std::uint16_t activity_hint = 0xFFFF;  ///< 0xFFFF = no hint.
    std::uint16_t fragment_number = 0;     ///< Always 0 (no fragmentation).
};

/** @brief Parsed RPC datagram; @p body spans the receive buffer. */
struct RpcPacket
{
    RpcHeader header; ///< Decoded header (host byte order).
    bool little_endian = true; ///< Sender's integer representation (drep).
    Bytes body; ///< After the 80-byte header (hook lifetime).
};

/** @brief NDR-wrapped request arguments (Connect/Read/Write/Control). */
struct NdrRequest
{
    std::uint32_t args_maximum = 0; ///< Max response size the caller accepts.
    Bytes args; ///< PNIO blocks.
};

/** @brief NDR-wrapped response arguments. */
struct NdrResponse
{
    PnioStatus status{};            ///< k_pnio_ok on success.
    std::uint32_t args_maximum = 0; ///< Echo of the request's value.
    Bytes args; ///< PNIO blocks.
};

/** @brief One PNIO block; @p content spans the input buffer. */
struct PnioBlock
{
    std::uint16_t type = 0;        ///< pnio_block::k_* (big-endian on wire).
    std::uint8_t version_high = 1; ///< BlockVersionHigh.
    std::uint8_t version_low = 0;  ///< BlockVersionLow.
    Bytes content; ///< After BlockVersionLow.
};

/** @brief IPv4 address + UDP port. */
struct Ipv4Endpoint
{
    std::array<std::uint8_t, 4> ip{}; ///< Network order, e.g. {192,168,0,10}.
    std::uint16_t port = k_rpc_port;  ///< Host order.
    /** @brief Equal if ip and port match. */
    bool operator==(const Ipv4Endpoint &o) const
    {
        return ip == o.ip && port == o.port;
    }
    /** @brief Negation of operator==. */
    bool operator!=(const Ipv4Endpoint &o) const { return !(*this == o); }
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
 *       No privileges needed. POSIX only: not built with PROFINET_POSIX=OFF;
 *       use the codec functions above with your own UDP stack instead.
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
