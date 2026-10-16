# Getting started

## Requirements

- Linux (uses `AF_PACKET`, `PACKET_AUXDATA`).
- C++20 compiler (GCC 10+, Clang 12+). Public headers use `std::span`, so
  your application must also compile as C++20. The CMake target sets this
  for you (`cxx_std_20` is a public compile feature).
- CMake 3.16+.
- GoogleTest only when building the tests (`libgtest-dev`).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # optional
```

### CMake options

| Option | Default (top-level) | Default (`add_subdirectory`) | Effect |
|---|---|---|---|
| `PROFINET_BUILD_TESTS` | `ON` | `OFF` | GoogleTest unit tests, `profinet_loopback`, ctest entries |
| `PROFINET_BUILD_EXAMPLES` | `ON` | `OFF` | `pn_sniff` example |
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
g++ -std=c++20 -I/usr/local/include main.cpp -L/usr/local/lib -lprofinet -o my_app
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

## Next

- How frames flow through the library: [concepts.md](concepts.md)
- Every function and type: the headers in `include/profinet/`, or the
  generated HTML (`cmake --build build --target docs`, open `build/html/index.html`)
- Ready-made patterns: [cookbook.md](cookbook.md)
