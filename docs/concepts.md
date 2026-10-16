# Concepts

## Architecture

```
            your application
   ┌───────────────────────────────────┐
   │ hooks (std::function)   send()    │
   └───────▲─────────────────────┬─────┘
           │ FrameView           │ Bytes payload
   ┌───────┴─────────────────────▼─────┐
   │ profinet::Stack   (stack.h)       │  optional: socket, receive loop, send
   ├───────────────────────────────────┤
   │ Dispatcher        (dispatcher.h)  │  hooks, feed(raw) → dispatch, no I/O
   ├───────────────────────────────────┤
   │ parse / build     (frame.h)       │  pure functions, no I/O
   └───────▲─────────────────────┬─────┘
           │ AF_PACKET raw socket │
   ════════╧═════════ NIC ═══════╧═════
```

Three layers:

- **`frame.h`**: pure functions. No sockets, no state. Use it alone to
  decode frames from pcap files or other sources, or to build frames for
  tests.
- **`dispatcher.h`**: `profinet::Dispatcher` holds your hooks. `feed(raw)`
  parses one frame and calls the matching hooks. No socket.
- **`stack.h`**: `profinet::Stack` is a `Dispatcher` plus a raw socket on one
  interface: receive loop, VLAN recovery, multicast joins, `send()`.

### Who owns the socket

Socket management is optional. Pick one:

| Mode | Code | Library does | You do |
|---|---|---|---|
| Library-managed | `Stack stack("eth0");` | Open, configure, bind, join DCP/LLDP multicast, receive, send, close | Register hooks, call `run()`/`poll()` |
| Your fd, library loop | `Stack stack(fd, mac, ifindex, fd_ownership_t::BORROW);` | Receive loop, send; closes the fd only with `OWN` | Open/configure the socket (PACKET_AUXDATA, multicast) |
| Fully yours | `Dispatcher d; ... d.feed(raw, vlan);` | Parse + hooks | All I/O: socket / pcap / XDP / replay, sending (`build()`) |

With `feed()`, pass the out-of-band VLAN tag if your NIC strips it
(PACKET_AUXDATA, pcap metadata); otherwise `FrameView::vlan` is only set for
in-band tags.

Beside them, **`rpc.h`** handles connection management (CM) over UDP; see
[RPC](concepts.md#rpc-connection-management). It shares only `Bytes` with the layer-2 code.

## Frame layout

```
 0      6      12            (16)       +2         +2
 ┌──────┬──────┬─────────────┬──────────┬──────────┬────────────────────────┐
 │ dst  │ src  │[0x8100 TCI] │ 0x8892   │ FrameID  │ payload ...            │
 └──────┴──────┴─────────────┴──────────┴──────────┴────────────────────────┘
                optional VLAN  EtherType              ▲
                                                      FrameView::payload
```

All multi-byte fields are big-endian. FCS is not included (Linux strips it).

### Cyclic RT payload

```
 ┌──────────────── C_SDU (IO data, ≥ 40 bytes) ───────────┬──────────────┐
 │ cyclic_data(f)                                          │ APDU status  │
 └─────────────────────────────────────────────────────────┴──────────────┘
                              CycleCounter(2) DataStatus(1) TransferStatus(1)
```

### DCP payload

```
 ServiceID(1) ServiceType(1) Xid(4) ResponseDelay/Reserved(2) DCPDataLength(2) │ blocks...
 └──────────────────────────── parse_dcp(f) ────────────────────────────────┘
 blocks = { Option(1) Suboption(1) Length(2) Data(Length) [pad to even] } ...
```

`parse_dcp` decodes the 10-byte header only. Blocks start at
`f.payload.subspan(10)`; parse them yourself (see the
[cookbook](cookbook.md#walk-dcp-blocks)).

## FrameID ranges

Constants are in `profinet::frame_id`.

| Range | Meaning | Helper |
|---|---|---|
| `0x0000`–`0x00FF` | PTCP (time sync) | |
| `0x0100`–`0x7FFF` | RT class 3 (IRT) | `is_cyclic` |
| `0x8000`–`0xBFFF` | RT class 2 | `is_cyclic` |
| `0xC000`–`0xF7FF` | RT class 1 (typical cyclic IO) | `is_cyclic` |
| `0xFC01` | Alarm high | |
| `0xFE01` | Alarm low | |
| `0xFEFC` | DCP Hello | `is_dcp` |
| `0xFEFD` | DCP Get/Set | `is_dcp` |
| `0xFEFE` | DCP Identify request (multicast `01:0E:CF:00:00:00`) | `is_dcp` |
| `0xFEFF` | DCP Identify response | `is_dcp` |

## Other EtherTypes (LLDP, MRP, ...)

`on_ethertype(type, hook)` receives frames of any EtherType. For those,
`FrameView::payload` starts right after the EtherType and `frame_id` is 0.
`profinet/lldp.h` decodes and builds LLDP (`0x88CC`), which PROFINET devices
must send for topology detection:

```
 dst=01:80:C2:00:00:0E │ src │ 0x88CC │ TLV │ TLV │ ... │ End(0,0)
 TLV = type(7 bit) | length(9 bit) | value(length)
 mandatory: Chassis ID(1), Port ID(2), TTL(3); PROFINET uses subtype 7
 (locally assigned): chassis = NameOfStation, port = "port-001"
```

The library does not send LLDP periodically by itself; call
`send_raw(build_lldp(...))` from a timer (every 5 s is typical, TTL 20 s).

## Multicast

Network cards drop multicast frames for groups nobody has joined. `Stack`
joins `01:0E:CF:00:00:00` (DCP Identify / Hello) and `01:80:C2:00:00:0E`
(LLDP) when it is constructed, so a device sees Identify-All requests from
engineering tools and neighbours' LLDP frames. Join other groups
(e.g. multicast cyclic data) with `join_multicast()`. To receive *all*
multicast while debugging: `sudo ip link set <if> allmulticast on`.

## Receive path

`Stack::poll(timeout_ms)`:

1. Waits up to `timeout_ms` for the socket to become readable
   (`0` = don't wait, `-1` = wait forever).
2. Reads **every** pending frame without blocking.
3. For each frame:
   - drops frames this host sent (`PACKET_OUTGOING`);
   - parses PROFINET frames with `parse()`, everything else with
     `parse_ethernet()`; drops runts and truncated PROFINET frames;
   - fills in `FrameView::vlan` from the kernel if the NIC stripped the VLAN
     tag (hardware VLAN offload);
   - PROFINET: calls **every** `on()` hook whose `[first, last]` range
     contains the FrameID, in registration order;
   - then calls every `on_ethertype()` hook registered for the frame's
     EtherType, in registration order.
4. Returns how many frames were PROFINET or matched an EtherType hook.

`Stack::run()` calls `poll(100)` in a loop until `stop()`.

### Hook semantics

- A frame can match several hooks. All of them run.
- Hooks run on the thread that called `poll()`/`run()`, one at a time.
- An exception thrown by a hook propagates out of `poll()`/`run()`. Frames
  still in the socket queue are delivered on the next `poll()`.
- Hooks run inline in the receive loop. A slow hook delays every following
  frame. Keep hooks short or hand the work to another thread (copy first).

## Lifetimes

`FrameView` does not own its memory. `payload`, `raw` and the spans returned
by `cyclic_data()` point into the stack's receive buffer, which gets
overwritten by the next frame. **They are valid only until the hook returns.**
To keep data, copy it:

```cpp
std::vector<std::uint8_t> keep(f.raw.begin(), f.raw.end());
```

`dst`, `src`, `vlan` and `frame_id` are values and are safe to copy.

## Threading

| Operation | Thread-safety |
|---|---|
| `on()`, `on_any()` | **Not** thread-safe. Register all hooks before the first `poll()`/`run()`. |
| `poll()`, `run()` | Call from one thread only. |
| `stop()` | Any thread, including from inside a hook. |
| `send()`, `send_raw()` | Any thread, including from inside a hook (each call is a single `sendto`). |
| `mac()`, `fd()` | Any thread. |

`stop()` takes effect within ~100 ms (one `poll(100)` period). Calling
`stop()` *before* `run()` has no effect, because `run()` resets the flag
when it starts.

## RPC (connection management)

Connect, Release, Read, Write and Control (PrmEnd, ApplicationReady) travel
as DCE/RPC connectionless datagrams over UDP/IP, not as raw frames:

```
UDP payload:
| RPC header (80) | NDR wrapper (20) | PNIO block | PNIO block | ...
  drep byte order   drep byte order    big-endian: type(2) length(2) ver(1) ver(1) content
```

- **Ports.** Devices listen on `34964`. The side that *initiates* a call
  uses an ephemeral port (49152–65535): the controller for Connect/Write/
  PrmEnd, the device for ApplicationReady. `RpcEndpoint(0)` picks one.
- **drep.** The 4th header byte says whether integers in the header and NDR
  wrapper are little- or big-endian. `parse_rpc` accepts both; `build_rpc`
  always sends little-endian. UUIDs are stored canonically in `Uuid`.
- **Matching.** A response copies `activity`, `sequence`, `opnum`, `object`
  and `interface` from the request (`make_response`). Use `activity` +
  `sequence` to pair responses with requests on the client side.
- **What the library does not do.** No AR/CR state machine, no block
  content decoding beyond the block header, no DCE/RPC fragmentation, no
  retransmission, ping or ack handling. Your hooks decide what a Connect
  means and build the response blocks. See the open-source p-net stack for
  a full reference implementation of the CM state machines.

## Limits

- One interface per `Stack`. Create several `Stack` objects for several ports.
- Receive buffer is 2048 bytes. Longer (jumbo) frames are truncated.
- The socket captures all EtherTypes (needed to see in-band VLAN-tagged
  frames) and filters in user space. On very busy links, add a BPF filter on
  `fd()` if CPU load matters.
- No send rate control or cycle timing. Your application decides when to send.
- RPC: one datagram per call (no DCE/RPC fragmentation); bodies above
  ~1400 bytes rely on IP fragmentation.
