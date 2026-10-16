# Concepts

## Architecture

```
            your application
   ┌───────────────────────────────────┐
   │ hooks (std::function)   send()    │
   └───────▲─────────────────────┬─────┘
           │ FrameView           │ Bytes payload
   ┌───────┴─────────────────────▼─────┐
   │ profinet::Stack   (stack.h)       │  socket, receive loop, dispatch
   ├───────────────────────────────────┤
   │ parse / build     (frame.h)       │  pure functions, no I/O
   └───────▲─────────────────────┬─────┘
           │ AF_PACKET raw socket │
   ════════╧═════════ NIC ═══════╧═════
```

Two layers:

- **`frame.h`**: pure functions. No sockets, no state. Use it alone to
  decode frames from pcap files or other sources, or to build frames for
  tests.
- **`stack.h`**: `profinet::Stack` owns one raw socket bound to one
  interface. Receives frames, parses them with `frame.h`, and calls your hooks.

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

## Multicast

Network cards drop multicast frames for groups nobody has joined. `Stack`
joins `01:0E:CF:00:00:00` (DCP Identify / Hello) when it is constructed, so a
device sees Identify-All requests from engineering tools. Join other groups
(e.g. multicast cyclic data) with `join_multicast()`. To receive *all*
multicast while debugging: `sudo ip link set <if> allmulticast on`.

## Receive path

`Stack::poll(timeout_ms)`:

1. Waits up to `timeout_ms` for the socket to become readable
   (`0` = don't wait, `-1` = wait forever).
2. Reads **every** pending frame without blocking.
3. For each frame:
   - drops frames this host sent (`PACKET_OUTGOING`);
   - drops non-PROFINET frames (`parse()` returns `nullopt`);
   - fills in `FrameView::vlan` from the kernel if the NIC stripped the VLAN
     tag (hardware VLAN offload);
   - calls **every** hook whose `[first, last]` range contains the FrameID,
     in registration order.
4. Returns how many PROFINET frames were dispatched.

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

## Limits

- One interface per `Stack`. Create several `Stack` objects for several ports.
- Receive buffer is 2048 bytes. Longer (jumbo) frames are truncated.
- The socket captures all EtherTypes (needed to see in-band VLAN-tagged
  frames) and filters in user space. On very busy links, add a BPF filter on
  `fd()` if CPU load matters.
- No send rate control or cycle timing. Your application decides when to send.
