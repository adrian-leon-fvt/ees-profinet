# Testing

## Layout

```
test/
├── unit/                     GoogleTest, no privileges, no network
│   ├── test_frame.cpp        parse/build/APDU/DCP
│   ├── test_lldp.cpp         LLDP TLV parse/build
│   ├── test_rpc.cpp          DCE/RPC header (LE + hand-encoded BE), NDR, blocks, UDP request/reply on 127.0.0.1
│   └── test_stack.cpp        hook dispatch, poll/run/stop (socketpair)
└── integration/
    ├── profinet_loopback.cpp manual test app: TX on one interface, RX on another
    └── loopback_netns.sh     runs it over a veth pair in a user netns (ctest)
```

All of it is built only when `PROFINET_BUILD_TESTS=ON` (default when this
repo is the top-level project).

## Running

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                    # everything
ctest --test-dir build -L unit            # unit tests only
ctest --test-dir build -L integration     # veth loopback only
build/profinet_test --gtest_filter='StackTest.*'   # run gtest directly
```

The integration test needs unprivileged user namespaces
(`unshare -rn`). Where they are disabled (some containers, hardened
kernels), ctest reports it as **skipped**, not failed.

## Manual / hardware test: `profinet_loopback`

```sh
sudo build/profinet_loopback <tx_if> <rx_if>
```

Sends a class-1 RT frame (unicast to `rx_if`'s MAC) and a DCP Identify
(multicast) and an LLDP frame out of `tx_if`, and checks that hooks on `rx_if` receive all three
with intact headers, APDU status and payload length. It also checks
`/proc/net/dev_mcast` to confirm `rx_if` joined the DCP and LLDP multicast groups
(a veth pair delivers multicast even without joining, so receipt alone
doesn't prove it).

```
RT  : PASS
DCP : PASS
LLDP: PASS
MCAST: PASS
```

| Exit code | Meaning |
|---|---|
| 0 | Both frames received correctly |
| 1 | A frame was missing or corrupted, or an interface/socket error (message on stderr) |
| 2 | Wrong arguments |

Setups that work:

- Two NICs joined by a cable.
- Two NICs on the same unmanaged switch. Managed switches may drop or
  re-prioritise frames depending on VLAN/QoS settings.
- A veth pair (what ctest does):
  ```sh
  sudo ip link add pn_tx type veth peer name pn_rx
  sudo ip link set pn_tx up && sudo ip link set pn_rx up
  sudo build/profinet_loopback pn_tx pn_rx
  sudo ip link del pn_tx
  ```

Use `tcpdump -i <if> -e ether proto 0x8892` or Wireshark (`pn_rt`, `pn_dcp`
filters) next to it to inspect the frames.

## Testing your own hooks

Don't open a real interface in unit tests. Subclass `Stack` to reach the
protected fd-adopting constructor and feed frames through a `socketpair`.
The whole receive path runs as in production: `poll()`, parsing, dispatch.

```cpp
#include "profinet/stack.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
class FakeStack : public profinet::Stack
{
  public:
    explicit FakeStack(int fd) : Stack(fd, {0x02, 0, 0, 0, 0, 1}, 1) {}
};
} // namespace

TEST(MyApp, ReactsToDcpIdentify)
{
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);
    FakeStack stack(fds[0]); // owns fds[0]
    bool seen = false;
    stack.on(profinet::frame_id::k_dcp_identify_req, [&](const auto &) { seen = true; });

    const std::uint8_t dcp[] = {5, 0, 0, 0, 0, 1, 0, 1, 0, 4, 0xFF, 0xFF, 0, 0};
    const auto frame = profinet::build({0x01, 0x0E, 0xCF, 0, 0, 0}, {0x02, 0, 0, 0, 0, 2},
                                       profinet::frame_id::k_dcp_identify_req, dcp);
    ::send(fds[1], frame.data(), frame.size(), 0);

    stack.poll(100);

    EXPECT_TRUE(seen);
    ::close(fds[1]);
}
```

What you send from the fake stack (`send()`) arrives on `fds[1]`; read it
with `recv()` to check your replies.

For pure parsing logic, skip the socket and call `profinet::parse()` on
bytes from `profinet::build()` or from a capture.

See `test/unit/test_stack.cpp` for more examples.
