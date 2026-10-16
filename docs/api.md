# API reference

Namespace: `profinet`. Headers: `#include "profinet/frame.h"` (parsing only)
or `#include "profinet/stack.h"` (includes `frame.h`).

## Types (`frame.h`)

### `Mac`
```cpp
using Mac = std::array<std::uint8_t, 6>;
```
Ethernet address. Supports `==`, brace-initialisation:
`profinet::Mac m{0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};`

### `Bytes`
```cpp
using Bytes = std::span<const std::uint8_t>;
```
Read-only, non-owning byte view. Converts implicitly from
`std::vector<std::uint8_t>`, `std::array<std::uint8_t, N>` and
`std::uint8_t[N]`. For a pointer + length use `Bytes(ptr, len)`.

### `Vlan`
| Field | Type | Default | Meaning |
|---|---|---|---|
| `pcp` | `uint8_t` | `6` | 802.1Q priority (0–7). PROFINET RT uses 6. |
| `vid` | `uint16_t` | `0` | VLAN ID (0–4095). `0` = priority tag only. |

### `ApduStatus`
Trailer of cyclic RT frames.

| Field | Type | Meaning |
|---|---|---|
| `cycle_counter` | `uint16_t` | Sender cycle counter (units of 31.25 µs) |
| `data_status` | `uint8_t` | Bit field: State, Redundancy, DataValid, ProviderState, StationProblemIndicator, Ignore |
| `transfer_status` | `uint8_t` | `0` = OK |

### `DcpHeader`
| Field | Type | Meaning |
|---|---|---|
| `service_id` | `uint8_t` | 3 Get, 4 Set, 5 Identify, 6 Hello |
| `service_type` | `uint8_t` | 0 request, 1 response success, 5 response unsupported |
| `xid` | `uint32_t` | Transaction ID; echo it in the response |
| `response_delay` | `uint16_t` | Identify request: response-delay factor. Others: reserved |
| `data_length` | `uint16_t` | Byte count of the blocks following the header |

### `FrameView`
Parsed frame. **Spans are valid only while the hook runs**; see
[concepts.md#lifetimes](concepts.md#lifetimes).

| Field | Type | Meaning |
|---|---|---|
| `dst` | `Mac` | Destination MAC |
| `src` | `Mac` | Source MAC |
| `vlan` | `std::optional<Vlan>` | Set if the frame had a VLAN tag (in-band or offloaded) |
| `ethertype` | `uint16_t` | EtherType after any VLAN tag |
| `frame_id` | `uint16_t` | PROFINET FrameID; `0` for other EtherTypes |
| `payload` | `Bytes` | PROFINET: everything after FrameID, including APDU status and any Ethernet padding. Other EtherTypes: everything after the EtherType |
| `raw` | `Bytes` | Whole frame from destination MAC, as received |

Note: with hardware VLAN offload, `vlan` is set but `raw` contains no tag.

### Constants
| Name | Value |
|---|---|
| `k_ethertype` | `0x8892` |
| `k_ethertype_vlan` | `0x8100` |
| `k_multicast_dcp` | `01:0E:CF:00:00:00` (DCP Identify / Hello) |
| `frame_id::*` | See [concepts.md#frameid-ranges](concepts.md#frameid-ranges) |

## Functions (`frame.h`)

All are pure: no I/O, no allocation except `build()`, no exceptions.

### `bool is_cyclic(uint16_t id)`
True for `0x0100`–`0xF7FF` (RT class 1, 2 and 3).

### `bool is_dcp(uint16_t id)`
True for `0xFEFC`–`0xFEFF`.

### `std::optional<FrameView> parse_ethernet(Bytes raw)`
Parse the Ethernet header of a frame of any EtherType (starting at
destination MAC, no FCS). Handles one optional 802.1Q tag. `payload` = bytes
after the EtherType, `frame_id` = 0. Returns `nullopt` if shorter than 14
bytes (18 with a VLAN tag).

### `std::optional<FrameView> parse(Bytes raw)`
Parse a PROFINET frame: `parse_ethernet()` plus FrameID. `payload` = bytes
after the FrameID.
Returns `nullopt` if the frame is truncated before the FrameID or the
EtherType is not `0x8892`.

### `std::optional<ApduStatus> parse_apdu_status(const FrameView &frame)`
Reads the last 4 bytes of `payload`. Returns `nullopt` if the FrameID is not
cyclic or the payload is shorter than 4 bytes.
Assumes no Ethernet padding, which holds for valid RT frames (C_SDU ≥ 40
bytes keeps the frame ≥ 60 bytes).

### `Bytes cyclic_data(const FrameView &frame)`
`payload` without its last 4 bytes (the IO data / C_SDU). Empty if
`payload` < 4 bytes. Does **not** check the FrameID.

### `std::optional<DcpHeader> parse_dcp(const FrameView &frame)`
Decodes the 10-byte DCP header. Returns `nullopt` if the FrameID is not DCP
or the payload is shorter than 10 bytes. Blocks: `frame.payload.subspan(10)`.

### `std::vector<std::uint8_t> build(dst, src, id, payload, vlan = nullopt)`
```cpp
std::vector<std::uint8_t> build(
    const Mac &dst, const Mac &src, std::uint16_t id, Bytes payload,
    const std::optional<Vlan> &vlan = std::nullopt);
```
Returns `dst | src | [0x8100 TCI] | 0x8892 | id | payload`, zero-padded to
60 bytes. No maximum-length check: the payload should be at most 1496 bytes
(1492 with VLAN) for a standard 1500-byte MTU.

### `std::vector<std::uint8_t> build_ethernet(dst, src, ethertype, payload, vlan = nullopt)`
Same, for any EtherType: `dst | src | [0x8100 TCI] | ethertype | payload`,
zero-padded to 60 bytes.

## LLDP (`lldp.h`)

Included by `stack.h`.

| Constant | Value |
|---|---|
| `k_ethertype_lldp` | `0x88CC` |
| `k_multicast_lldp` | `01:80:C2:00:00:0E` |
| `k_oui_profinet` | `00:0E:CF` (PROFINET org-specific TLVs) |
| `k_oui_ieee_802_3` | `00:12:0F` (MAC/PHY config TLV) |
| `lldp_tlv::k_*` | TLV types: `k_end` 0, `k_chassis_id` 1, `k_port_id` 2, `k_ttl` 3, `k_port_description` 4, `k_system_name` 5, `k_system_description` 6, `k_system_capabilities` 7, `k_management_address` 8, `k_org_specific` 127 |

### `struct LldpTlv { std::uint8_t type; Bytes value; }`
One TLV. `value` points into the frame (hook lifetime).

### `struct Lldp`
| Field | Type | Meaning |
|---|---|---|
| `chassis_id_subtype` | `uint8_t` | 7 = locally assigned (PROFINET NameOfStation) |
| `chassis_id` | `std::string` | Chassis ID bytes (owned copy) |
| `port_id_subtype` | `uint8_t` | 7 = locally assigned |
| `port_id` | `std::string` | Port ID bytes (owned copy) |
| `ttl` | `uint16_t` | Seconds |
| `tlvs` | `std::vector<LldpTlv>` | All TLVs except End, in frame order (spans: hook lifetime) |

### `std::optional<Lldp> parse_lldp(const FrameView &frame)`
Decode an LLDPDU from a frame parsed with `parse_ethernet()` (or received via
`on_ethertype(k_ethertype_lldp, ...)`). Returns `nullopt` if the EtherType is
not `0x88CC`, a TLV runs past the frame, or Chassis ID / Port ID / TTL is
missing. Stops at the End TLV.

Org-specific TLV layout in `value`: `OUI(3) | subtype(1) | info`.

### `void append_lldp_tlv(std::vector<std::uint8_t> &out, std::uint8_t type, Bytes value)`
Appends `type(7 bit) | length(9 bit) | value`. `value` must be ≤ 511 bytes.

### `void append_lldp_org_tlv(std::vector<std::uint8_t> &out, const Oui &oui, std::uint8_t subtype, Bytes info)`
Appends a type-127 TLV: `OUI | subtype | info`.

### `std::vector<std::uint8_t> build_lldp(src, chassis_id, port_id, ttl_s = 20, extra_tlvs = {})`
```cpp
std::vector<std::uint8_t> build_lldp(
    const Mac &src, std::string_view chassis_id, std::string_view port_id,
    std::uint16_t ttl_s = 20, Bytes extra_tlvs = {});
```
Complete LLDP frame to `k_multicast_lldp`: Chassis ID (subtype 7), Port ID
(subtype 7), TTL, `extra_tlvs` (pre-encoded), End. Send with
`stack.send_raw()`.

## `class Stack` (`stack.h`)

Owns one `AF_PACKET` socket bound to one interface. Not copyable or movable.

### `explicit Stack(const std::string &interface_name)`
Opens the socket, looks up the interface index and MAC, enables VLAN
recovery, binds, and joins the DCP and LLDP multicast groups.
**Throws** `std::system_error` (with `errno`) naming the failed step:
`socket`, `SIOCGIFINDEX`, `SIOCGIFHWADDR`, `PACKET_AUXDATA`, `bind`,
`PACKET_ADD_MEMBERSHIP`.
Needs `CAP_NET_RAW`.

### `virtual ~Stack()`
Closes the socket.

### Hook registration
```cpp
using Hook = std::function<void(const FrameView &)>;

void on(std::uint16_t first, std::uint16_t last, Hook hook); // inclusive range
void on(std::uint16_t id, Hook hook);                        // exactly one FrameID
void on_any(Hook hook);                                      // 0x0000–0xFFFF
void on_ethertype(std::uint16_t ethertype, Hook hook);       // any EtherType
```
`on`/`on_any` see PROFINET (`0x8892`) frames only. `on_ethertype` sees every
frame of that EtherType; for `0x8892` it gets the PROFINET view (payload after
FrameID), otherwise `payload` starts after the EtherType and `frame_id` is 0.
Hooks stack up: every matching hook runs, in registration order. There is no
removal; to turn a hook off, check a flag inside it. Not thread-safe:
register before `poll()`/`run()`.

### `int poll(int timeout_ms)`
Waits up to `timeout_ms` (`0` non-blocking, `-1` forever), then reads and
dispatches every queued frame. Returns the number of frames that were
PROFINET or matched an `on_ethertype` hook. Returns `0` if interrupted by a signal (`EINTR`).
**Throws** `std::system_error` on `poll`/`recvmsg` failure, and passes
through anything a hook throws.

### `void run()`
`while (!stopped) poll(100);`. Blocks the calling thread.

### `void stop()`
Makes `run()` return within ~100 ms. Thread- and signal-safe (atomic store).
Has no effect if called before `run()` starts.

### `void send(const Mac &dst, std::uint16_t id, Bytes payload, const std::optional<Vlan> &vlan = std::nullopt)`
`build(dst, mac(), id, payload, vlan)` followed by `send_raw()`.

### `void send_raw(Bytes frame)`
Transmits `frame` unchanged. It must be a complete Ethernet frame from the
destination MAC on (≥ 14 bytes; pad to 60 yourself).
**Throws** `std::system_error` on `sendto` failure (e.g. `ENETDOWN`,
`EMSGSIZE`).

### `const Mac &mac() const`
MAC address of the bound interface, read once at construction.

### `void join_multicast(const Mac &group)`
Adds @p group to the interface's multicast filter so the NIC delivers frames
sent to it. The constructor already joins `k_multicast_dcp` and `k_multicast_lldp`. Membership is
dropped automatically when the `Stack` is destroyed. Joining the same group
twice is harmless.
**Throws** `std::system_error` (`PACKET_ADD_MEMBERSHIP`).

### `int fd() const`
The socket descriptor, for an external `epoll`/`select` loop. Don't read
from it yourself; call `poll(0)` when it is readable.

### Protected: `Stack(int fd, const Mac &mac, int ifindex)`
Adopts an already-open socket that delivers one whole Ethernet frame per
datagram. Takes ownership (closes it in the destructor). The socket can be
an `AF_PACKET` socket (sent with `sendto` to `ifindex`) or any connected
datagram socket such as a `socketpair` end (sent with plain `send`). Use it to:

- unit-test hooks with a `socketpair` (see [testing.md](testing.md#testing-your-own-hooks));
- open the raw socket in a privileged helper and pass the fd to an
  unprivileged process.

Reachable through a subclass:

```cpp
class MyStack : public profinet::Stack
{
  public:
    MyStack(int fd, const profinet::Mac &mac, int ifindex) : Stack(fd, mac, ifindex) {}
};
```

### Protected: `bool dispatch(const FrameView &frame) const`
Runs the matching hooks for one frame. Returns true if the frame is
PROFINET or matched an EtherType hook. Subclasses can call it to inject
frames from other sources (pcap replay, simulation).
