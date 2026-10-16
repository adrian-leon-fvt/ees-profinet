# Getting started

## Requirements

- Linux (uses `AF_PACKET`, `PACKET_AUXDATA`).
- C++17 compiler (GCC 8+, Clang 7+). Matches embedded toolchains such as
  IAR 9. The CMake target sets this for you (`cxx_std_17` is a public
  compile feature). `profinet::Bytes` is a minimal C++17 stand-in for
  `std::span<const std::uint8_t>`.
- CMake 3.16+.
- GoogleTest only when building the tests (`libgtest-dev`).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # optional
```

Or with presets (CMake 3.21+):

| Preset | Builds | Use for |
|---|---|---|
| `linux` | everything: codecs, `Dispatcher`, `Stack`, `RpcEndpoint`, examples | Linux hosts |
| `portable` | codecs + `Dispatcher` only (`PROFINET_POSIX=OFF`) | MCU / RTOS targets (e.g. IAR + lwIP); you supply frame RX/TX and UDP |

```sh
cmake --preset portable && cmake --build --preset portable && ctest --preset portable
```

Presets export `compile_commands.json`; `.clangd` points clangd at
`build/linux` (run `cmake --preset linux` once). Edit `.clangd` to
`build/portable` to see what an MCU build compiles.

For a cross toolchain add `-DCMAKE_TOOLCHAIN_FILE=...` (tests need GoogleTest,
so also pass `-DPROFINET_BUILD_TESTS=OFF` there).

### CMake options

| Option | Default (top-level) | Default (`add_subdirectory`) | Effect |
|---|---|---|---|
| `PROFINET_POSIX` | `ON` | `ON` | `OFF` drops `stack.cpp`, `rpc_endpoint.cpp`, POSIX tests and examples, and doesn't install `stack.h`. Library then has no OS dependency. |
| `PROFINET_BUILD_TESTS` | `ON` | `OFF` | GoogleTest unit tests, `profinet_loopback`, ctest entries |
| `PROFINET_E2E_PYTHON` | `python3` | `python3` | Interpreter with profinet-py + pytest for the `e2e` ctest ([testing.md](testing.md)) |
| `PROFINET_BUILD_EXAMPLES` | `ON` | `OFF` | `pn_sniff`, `pn_borrowed_fd`, `pn_dispatcher` examples |
| `PROFINET_BUILD_DOCS` | `ON` | `OFF` | `docs` target (Doxygen HTML in `build/html/`; needs Doxygen, skipped if missing). Undocumented public API fails it. |
| `BUILD_SHARED_LIBS` | `OFF` | inherited | Build `libprofinet.so` instead of `.a` |

## Using it in your application

### Option A: `add_subdirectory` (git submodule / vendored copy)

```cmake
add_subdirectory(third_party/ees-profinet)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE profinet::profinet)
```

### Option B: install + `find_package`

```sh
cmake -S . -B build -DPROFINET_BUILD_TESTS=OFF -DPROFINET_BUILD_EXAMPLES=OFF
cmake --build build
sudo cmake --install build                # or --prefix /opt/profinet
```

```cmake
find_package(profinet REQUIRED)           # add -DCMAKE_PREFIX_PATH=/opt/profinet if needed
target_link_libraries(my_app PRIVATE profinet::profinet)
```

### Option C: without CMake

```sh
g++ -std=c++17 -I/usr/local/include main.cpp -L/usr/local/lib -lprofinet -o my_app
```

## Permissions

Raw sockets need `CAP_NET_RAW`. Pick one:

```sh
sudo ./my_app eth0                                  # simplest
sudo setcap cap_net_raw+ep ./my_app && ./my_app eth0  # no root at runtime
```

`setcap` has to be re-applied after each rebuild. Without the capability,
the `Stack` constructor throws `std::system_error` with `socket: Operation
not permitted`.

## First program

```cpp
#include "profinet/stack.h"

#include <cstdio>

int main(int argc, char **argv)
{
    profinet::Stack stack(argv[1]);

    stack.on_any([](const profinet::FrameView &f) {
        std::printf("FrameID %04X, %zu payload bytes\n", f.frame_id, f.payload.size());
    });

    stack.run(); // blocks until stack.stop()
}
```

A complete version that also sends a DCP Identify-All request is in
[`examples/pn_sniff.cpp`](../examples/pn_sniff.cpp):

```sh
sudo build/pn_sniff eth0
```

Same Identify-All, for the other socket-ownership modes
([concepts.md](concepts.md#who-owns-the-socket)):

| Example | Mode | Builds with |
|---|---|---|
| [`pn_borrowed_fd.cpp`](../examples/pn_borrowed_fd.cpp) | Your fd, library loop: `Stack(fd, mac, ifindex, BORROW)` | `linux` |
| [`pn_dispatcher.cpp`](../examples/pn_dispatcher.cpp) | Fully yours: `Dispatcher::feed()` + `build()`; I/O in `eth_open/eth_rx/eth_tx`, swap those for an MCU driver | `linux` and `portable` |

## Next

- How frames flow through the library: [concepts.md](concepts.md)
- Every function and type: the headers in `include/profinet/`, or the
  generated HTML (`cmake --build build --target docs`, open `build/html/index.html`)
- Ready-made patterns: [cookbook.md](cookbook.md)
