# ees-profinet documentation

Linux PROFINET layer-2 library. Opens a raw Ethernet socket on one interface,
receives frames with EtherType `0x8892`, parses the common headers and hands
each frame to the application through FrameID-filtered **hooks**. The
application also gets helpers to build and send frames.

| Document | Contents |
|---|---|
| [getting-started.md](getting-started.md) | Build, install, link into your app, permissions, first program |
| [concepts.md](concepts.md) | Architecture, frame layout, FrameID ranges, receive path, lifetimes, threading |
| [api.md](api.md) | Full reference for `profinet/frame.h` and `profinet/stack.h` |
| [cookbook.md](cookbook.md) | Recipes: DCP responder, cyclic IO, epoll/threads, VLAN, copying frames |
| [testing.md](testing.md) | Unit tests, loopback test app, hardware testing, testing your own hooks |
| [troubleshooting.md](troubleshooting.md) | Common errors and what they mean |

## Scope

| Included | Not included (build on top with hooks) |
|---|---|
| Raw `AF_PACKET` socket bound to one interface | Connection management (CM / DCE-RPC over UDP 34964) |
| DCP + LLDP multicast group membership | MRP, SNMP |
| LLDP parse/build, hooks for any EtherType | Periodic LLDP transmission (call `build_lldp` from a timer) |
| Ethernet + 802.1Q VLAN + FrameID parsing | Alarm protocol (acknowledgement, sequence numbers) |
| Cyclic RT APDU status trailer parsing | IO device / controller state machines |
| DCP PDU header parsing | DCP block (option/suboption) parsing |
| Frame building with 60-byte padding | GSDML, record data, PTCP/IRT scheduling |
| FrameID range dispatch to callbacks | Cycle timing / watchdogs |

## 30-second example

```cpp
#include "profinet/stack.h"

int main()
{
    profinet::Stack stack("eth0");
    stack.on(
        profinet::frame_id::k_rt_class1_first,
        profinet::frame_id::k_rt_class1_last,
        [](const profinet::FrameView &f) {
            profinet::Bytes io = profinet::cyclic_data(f);
            // ... use io ...
        });
    stack.run();
}
```
