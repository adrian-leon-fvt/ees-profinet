# An EES implementation of profinet

Linux PROFINET layer-2 library: raw `AF_PACKET` socket, frame parsing
(Ethernet/802.1Q/FrameID, RT APDU status, DCP header) and FrameID-based hooks.
CM/RPC, alarms and the IO-device state machine are left to the application.

## Build

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
sudo cmake --install build   # optional
```

Options (default ON when top-level, OFF when added via `add_subdirectory`):
`PROFINET_BUILD_TESTS`, `PROFINET_BUILD_EXAMPLES`.

## Test

- `ctest --test-dir build -L unit` — GoogleTest, no privileges needed.
- `ctest --test-dir build -L integration` — runs `profinet_loopback` over a
  veth pair in an unprivileged netns; skipped if user namespaces are off.
- Manual, on real hardware: `sudo build/profinet_loopback <tx_if> <rx_if>`
  with the two interfaces linked. Exit 0 = pass.

## Use

```cmake
add_subdirectory(ees-profinet)          # or: find_package(profinet)
target_link_libraries(app PRIVATE profinet::profinet)
```

```cpp
#include "profinet/stack.h"

profinet::Stack stack("eth0");                     // needs CAP_NET_RAW
stack.on(profinet::frame_id::k_rt_class1_first,
         profinet::frame_id::k_rt_class1_last,
         [](const profinet::FrameView &f) {
             auto status = profinet::parse_apdu_status(f);
             auto io = profinet::cyclic_data(f);
         });
stack.on(profinet::frame_id::k_dcp_identify_req, [&](const auto &f) {
    auto dcp = profinet::parse_dcp(f);
    // stack.send(f.src, profinet::frame_id::k_dcp_identify_rsp, reply);
});
stack.run();   // or integrate stack.fd() in your epoll loop and call poll(0)
```

`FrameView` spans are valid only inside the hook. Example: `examples/pn_sniff.cpp`.

## Documentation

Full usage docs in [`docs/`](docs/README.md): getting started, concepts,
API reference, cookbook, testing, troubleshooting.
