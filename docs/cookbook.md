# Cookbook

Copy-paste patterns. All snippets assume:

```cpp
#include "profinet/stack.h"
namespace pn = profinet;
```

- [Clean shutdown on Ctrl-C](cookbook.md#clean-shutdown-on-ctrl-c)
- [Consume cyclic IO data](cookbook.md#consume-cyclic-io-data)
- [Produce cyclic IO data](cookbook.md#produce-cyclic-io-data)
- [Walk DCP blocks](cookbook.md#walk-dcp-blocks)
- [Answer DCP Identify](cookbook.md#answer-dcp-identify)
- [Discover devices (DCP Identify-All)](cookbook.md#discover-devices-dcp-identify-all)
- [Integrate with your own epoll loop](cookbook.md#integrate-with-your-own-epoll-loop)
- [Hand frames to a worker thread](cookbook.md#hand-frames-to-a-worker-thread)
- [Send and receive LLDP](cookbook.md#send-and-receive-lldp)
- [Answer RPC Connect (device side)](cookbook.md#answer-rpc-connect-device-side)
- [Send an RPC request (controller side)](cookbook.md#send-an-rpc-request-controller-side)
- [VLAN tagging](cookbook.md#vlan-tagging)
- [Several interfaces](cookbook.md#several-interfaces)
- [Decode frames without a socket](cookbook.md#decode-frames-without-a-socket)
- [Feed hooks from your own socket](cookbook.md#feed-hooks-from-your-own-socket)

## Clean shutdown on Ctrl-C

`stop()` is an atomic store, so it is safe in a signal handler.

```cpp
#include <csignal>

pn::Stack *g_stack = nullptr;

int main()
{
    pn::Stack stack("eth0");
    g_stack = &stack;
    std::signal(SIGINT, [](int) { g_stack->stop(); });
    std::signal(SIGTERM, [](int) { g_stack->stop(); });
    stack.run();
}
```

## Consume cyclic IO data

Check `DataValid` (bit 2) and `ProviderState` (bit 4 = RUN) before using the
data. Watch the cycle counter to detect a stalled provider.

```cpp
constexpr std::uint8_t k_data_valid = 1u << 2;
constexpr std::uint8_t k_provider_run = 1u << 4;

stack.on(0xC001, [&](const pn::FrameView &f) {
    const auto status = pn::parse_apdu_status(f);
    if (!status || !(status->data_status & k_data_valid) ||
        !(status->data_status & k_provider_run))
    {
        return;
    }
    const pn::Bytes io = pn::cyclic_data(f);
    // The layout of `io` (IOPS/IOCS bytes, submodule data) comes from the
    // connection setup (CM). Example: first input byte + its IOPS.
    const std::uint8_t input0 = io[0];
    const std::uint8_t iops0 = io[1];
    (void)input0;
    (void)iops0;
});
```

## Produce cyclic IO data

The library does not time anything; you drive the cycle. `cycle_counter`
counts in 31.25 µs units, so add `send_clock_factor * reduction_ratio` each
cycle (32 for a 1 ms cycle).

```cpp
#include <chrono>
#include <thread>

void produce(pn::Stack &stack, const pn::Mac &controller, std::uint16_t frame_id)
{
    std::vector<std::uint8_t> payload(40, 0x00); // C_SDU, minimum 40 bytes
    payload.resize(44);                          // + 4 bytes APDU status
    std::uint16_t cycle = 0;
    auto next = std::chrono::steady_clock::now();

    for (;;)
    {
        // ... write your IO data + IOPS into payload[0..39] ...
        cycle += 32;
        payload[40] = static_cast<std::uint8_t>(cycle >> 8);
        payload[41] = static_cast<std::uint8_t>(cycle);
        payload[42] = 0x35; // State=primary, DataValid, ProviderState=RUN, no problem
        payload[43] = 0x00; // TransferStatus OK
        stack.send(controller, frame_id, payload, pn::Vlan{6, 0});

        next += std::chrono::milliseconds(1);
        std::this_thread::sleep_until(next);
    }
}
```

`sleep_until` on a stock kernel jitters by tens to hundreds of µs. For tight
cycles use `SCHED_FIFO`, `isolcpus`, or a PREEMPT_RT kernel.

## Walk DCP blocks

```cpp
#include <cstdio>

void for_each_dcp_block(const pn::FrameView &f)
{
    const auto h = pn::parse_dcp(f);
    if (!h)
    {
        return;
    }
    pn::Bytes blocks = f.payload.subspan(10);
    if (blocks.size() > h->data_length)
    {
        blocks = blocks.first(h->data_length); // drop Ethernet padding
    }
    while (blocks.size() >= 4)
    {
        const std::uint8_t option = blocks[0];
        const std::uint8_t suboption = blocks[1];
        const std::size_t len = (blocks[2] << 8) | blocks[3];
        if (blocks.size() < 4 + len)
        {
            break; // malformed
        }
        const pn::Bytes data = blocks.subspan(4, len);
        std::printf("option %u/%u, %zu bytes\n", option, suboption, data.size());

        const std::size_t step = 4 + len + (len & 1); // blocks are padded to even length
        blocks = blocks.subspan(std::min(step, blocks.size()));
    }
}
```

Common options: `1/2` IP parameter, `2/1` vendor name, `2/2` NameOfStation,
`2/3` DeviceID, `2/4` DeviceRole, `5/x` control, `0xFF/0xFF` all.

## Answer DCP Identify

Minimal device-side responder announcing a NameOfStation.
Real devices must also honour the `response_delay` factor and filter on the
requested NameOfStation; this sketch answers every request.

```cpp
void answer_identify(pn::Stack &stack, const std::string &name_of_station)
{
    stack.on(pn::frame_id::k_dcp_identify_req, [&stack, name_of_station](const pn::FrameView &f) {
        const auto req = pn::parse_dcp(f);
        if (!req || req->service_id != 5 || req->service_type != 0)
        {
            return;
        }

        std::vector<std::uint8_t> blocks;
        auto put16 = [&](std::size_t v) {
            blocks.push_back(static_cast<std::uint8_t>(v >> 8));
            blocks.push_back(static_cast<std::uint8_t>(v));
        };
        // NameOfStation block: option 2, suboption 2, BlockInfo(2) + name
        blocks.push_back(2);
        blocks.push_back(2);
        put16(2 + name_of_station.size());
        put16(0); // BlockInfo
        blocks.insert(blocks.end(), name_of_station.begin(), name_of_station.end());
        if (blocks.size() & 1)
        {
            blocks.push_back(0);
        }

        std::vector<std::uint8_t> rsp{
            5, 1, // Identify, response success
            static_cast<std::uint8_t>(req->xid >> 24),
            static_cast<std::uint8_t>(req->xid >> 16),
            static_cast<std::uint8_t>(req->xid >> 8),
            static_cast<std::uint8_t>(req->xid),
            0, 0, // reserved
            static_cast<std::uint8_t>(blocks.size() >> 8),
            static_cast<std::uint8_t>(blocks.size())};
        rsp.insert(rsp.end(), blocks.begin(), blocks.end());

        stack.send(f.src, pn::frame_id::k_dcp_identify_rsp, rsp);
    });
}
```

## Discover devices (DCP Identify-All)

```cpp
#include <map>

std::map<pn::Mac, std::vector<std::uint8_t>> discover(pn::Stack &stack)
{
    std::map<pn::Mac, std::vector<std::uint8_t>> found;
    stack.on(pn::frame_id::k_dcp_identify_rsp, [&](const pn::FrameView &f) {
        found[f.src].assign(f.payload.begin(), f.payload.end()); // copy: span dies after hook
    });

    const std::uint8_t identify_all[] = {
        5, 0,                   // Identify request
        0x00, 0x00, 0x10, 0x01, // Xid
        0x00, 0x80,             // ResponseDelay factor
        0x00, 0x04,             // DCPDataLength
        0xFF, 0xFF, 0x00, 0x00  // All selector
    };
    stack.send({0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00}, pn::frame_id::k_dcp_identify_req, identify_all);

    for (int i = 0; i < 20; ++i) // ~2 s
    {
        stack.poll(100);
    }
    return found;
}
```

The hook captures `found` by reference, and hooks can't be removed, so the
`Stack` must not be polled after `discover()` returns. Use a dedicated `Stack`
for discovery, or have the hook write into storage that outlives the stack.

## Integrate with your own epoll loop

Don't call `run()`. Watch `fd()` and call `poll(0)` when it is readable.

```cpp
#include <sys/epoll.h>

void event_loop(pn::Stack &stack)
{
    const int ep = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = stack.fd();
    epoll_ctl(ep, EPOLL_CTL_ADD, stack.fd(), &ev);
    // ... add your timers, sockets, etc. ...

    epoll_event events[16];
    for (;;)
    {
        const int n = epoll_wait(ep, events, 16, -1);
        for (int i = 0; i < n; ++i)
        {
            if (events[i].data.fd == stack.fd())
            {
                stack.poll(0); // drains all queued frames
            }
        }
    }
}
```

The same works with `select`, `poll`, Boost.Asio (`posix::stream_descriptor`
+ `async_wait`), libuv (`uv_poll_t`) or Qt (`QSocketNotifier`).

## Hand frames to a worker thread

Hooks run in the receive loop. For slow processing, copy and queue.

```cpp
#include <condition_variable>
#include <deque>
#include <mutex>

struct FrameQueue
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<std::uint8_t>> q;

    void push(pn::Bytes raw)
    {
        {
            std::lock_guard lock(m);
            q.emplace_back(raw.begin(), raw.end());
        }
        cv.notify_one();
    }

    std::vector<std::uint8_t> pop()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return !q.empty(); });
        auto frame = std::move(q.front());
        q.pop_front();
        return frame;
    }
};

// Receive thread:   stack.on_any([&](const pn::FrameView &f) { queue.push(f.raw); });
// Worker thread:    auto raw = queue.pop(); auto f = pn::parse(raw); ...
```

With hardware VLAN offload, `raw` has no tag; copy `f.vlan` too if you need it.

## Send and receive LLDP

PROFINET devices announce themselves on every port with LLDP: chassis ID =
NameOfStation, port ID = `port-NNN`, plus PROFINET org-specific TLVs.

```cpp
#include <chrono>
#include <map>
#include <thread>

void lldp_neighbours(pn::Stack &stack, std::map<std::string, std::string> &neighbours)
{
    stack.on_ethertype(pn::k_ethertype_lldp, [&](const pn::FrameView &f) {
        const auto l = pn::parse_lldp(f);
        if (l)
        {
            neighbours[l->chassis_id] = l->port_id; // strings are owned copies
        }
    });
}

void lldp_announce(pn::Stack &stack, const std::string &name_of_station)
{
    std::vector<std::uint8_t> extra;
    // PROFINET chassis MAC TLV (OUI 00:0E:CF, subtype 5)
    pn::append_lldp_org_tlv(extra, pn::k_oui_profinet, 5, stack.mac());
    const auto frame = pn::build_lldp(stack.mac(), name_of_station, "port-001", 20, extra);
    for (;;)
    {
        stack.send_raw(frame); // send() is thread-safe; run this in its own thread
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
```

Other EtherTypes work the same way with `on_ethertype(0x88E3, ...)` (MRP) etc.

## Answer RPC Connect (device side)

`RpcEndpoint` handles the UDP socket and DCE/RPC framing; your hook decides
what the blocks mean. Block content layouts are in IEC 61158-6-10.

```cpp
#include "profinet/rpc.h"

void serve_connect(pn::RpcEndpoint &ep) // pn::RpcEndpoint ep(pn::k_rpc_port);
{
    ep.on(pn::pnio_opnum::k_connect, [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &from) {
        if (p.header.ptype != pn::rpc_ptype::k_request)
        {
            return;
        }
        const auto req = pn::parse_ndr_request(p);
        const auto blocks = req ? pn::parse_blocks(req->args) : std::nullopt;
        if (!blocks)
        {
            return;
        }
        std::vector<std::uint8_t> out;
        for (const auto &b : *blocks)
        {
            if (b.type == pn::pnio_block::k_ar_block_req)
            {
                std::vector<std::uint8_t> ar_res; // decode b.content, fill ARBlockRes
                pn::append_block(out, b.type | pn::pnio_block::k_response_bit, ar_res);
            }
        }
        ep.reply(p, from, pn::build_ndr_response(pn::k_pnio_ok, req->args_maximum, out));
    });
    ep.run();
}
```

## Send an RPC request (controller side)

Requests go out from an ephemeral port; match the response on `activity`.

```cpp
#include "profinet/rpc.h"

#include <arpa/inet.h>
#include <random>

std::optional<std::vector<std::uint8_t>> rpc_read(
    const std::string &device_ip, std::uint16_t vendor_id, std::uint16_t device_id,
    pn::Bytes read_blocks)
{
    pn::RpcEndpoint ep(0); // first free port in 49152-65535
    pn::Ipv4Endpoint to;   // port defaults to 34964
    if (inet_pton(AF_INET, device_ip.c_str(), to.ip.data()) != 1)
    {
        return std::nullopt;
    }
    pn::RpcHeader h;
    h.object = pn::make_pnio_object_uuid(1, device_id, vendor_id);
    h.interface = pn::rpc_interface::k_device;
    h.opnum = pn::pnio_opnum::k_read;
    std::random_device rd;
    for (auto &b : h.activity)
    {
        b = static_cast<std::uint8_t>(rd());
    }

    std::optional<std::vector<std::uint8_t>> result;
    ep.on(pn::pnio_opnum::k_read, [&](const pn::RpcPacket &p, const pn::Ipv4Endpoint &) {
        const auto rsp = pn::parse_ndr_response(p);
        if (p.header.ptype == pn::rpc_ptype::k_response && p.header.activity == h.activity &&
            rsp && rsp->status == pn::k_pnio_ok)
        {
            result.emplace(rsp->args.begin(), rsp->args.end()); // copy: span dies after hook
        }
    });
    ep.send(to, h, pn::build_ndr_request(4096, read_blocks));
    for (int i = 0; i < 10 && !result; ++i) // ~1 s timeout, no retransmission
    {
        ep.poll(100);
    }
    return result;
}
```

## VLAN tagging

Receiving: `f.vlan` is set for tagged frames, whether the tag was in the
frame or stripped by the NIC.

Sending: pass a `Vlan`. PROFINET RT normally uses priority 6, VLAN 0
("priority tagged"):

```cpp
stack.send(dst, 0xC001, payload, pn::Vlan{6, 0});
```

## Several interfaces

One `Stack` per port. Run each in its own thread, or put all `fd()`s in one
epoll set.

```cpp
#include <thread>

void two_ports()
{
    pn::Stack port1("eth0");
    pn::Stack port2("eth1");
    // ... register hooks on each ...
    std::thread t1([&] { port1.run(); });
    std::thread t2([&] { port2.run(); });
    t1.join();
    t2.join();
}
```

## Decode frames without a socket

`frame.h` has no I/O. Decode captured bytes (pcap, logs, test vectors)
directly:

```cpp
#include "profinet/frame.h"

bool is_dcp_identify(const std::vector<std::uint8_t> &captured)
{
    const auto f = profinet::parse(captured);
    return f && f->frame_id == profinet::frame_id::k_dcp_identify_req;
}
```

## Feed hooks from your own socket

Keep the socket (or pcap handle, XDP ring, ...) in your code and use only
the hook dispatch. `Dispatcher` has no I/O and needs no privileges.

```cpp
#include "profinet/dispatcher.h"

#include <sys/socket.h>

void pump(int my_fd, const pn::Dispatcher &hooks)
{
    std::uint8_t buf[2048];
    for (;;)
    {
        const ssize_t n = ::recv(my_fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        hooks.feed(pn::Bytes(buf, static_cast<std::size_t>(n)));
        // NIC strips VLAN tags? read PACKET_AUXDATA via recvmsg() and pass
        // feed(raw, pn::Vlan{pcp, vid}).
    }
}
```

To keep the library's receive loop but your own socket, hand the fd to
`Stack` without giving it away:

```cpp
void run_on(int my_fd, const pn::Mac &mac, int ifindex)
{
    pn::Stack stack(my_fd, mac, ifindex, pn::fd_ownership_t::BORROW);
    stack.on_any([](const pn::FrameView &) { /* ... */ });
    stack.run(); // my_fd stays open after stack is destroyed
}
```

Complete programs: [`examples/pn_dispatcher.cpp`](../examples/pn_dispatcher.cpp)
(feed) and [`examples/pn_borrowed_fd.cpp`](../examples/pn_borrowed_fd.cpp)
(borrowed fd).
