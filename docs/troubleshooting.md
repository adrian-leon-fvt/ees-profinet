# Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `socket: Operation not permitted` | No `CAP_NET_RAW` | Run with `sudo`, or `sudo setcap cap_net_raw+ep ./app` |
| `SIOCGIFINDEX: No such device` | Wrong interface name | Check `ip link` |
| `sendto: Network is down` | Interface is down | `sudo ip link set <if> up` |
| `sendto: Message too long` | Frame larger than MTU | Keep payload ≤ 1496 bytes (1492 with VLAN) |
| Compile error about `std::span` | App not built as C++20 | Link via `profinet::profinet` in CMake (sets C++20), or pass `-std=c++20` |
| LLDP hook never fires | Linux bridge or `lldpd` in the path, or the switch consumes LLDP | Connect directly; LLDP is link-local and not forwarded by switches |
| No hooks fire | Hook range doesn't contain the FrameID | Register `on_any` with a `printf` to see what arrives |
| No hooks fire, `tcpdump` shows frames | `poll()`/`run()` never called, or called on another `Stack` | Make sure the loop runs on the `Stack` the hooks are on |
| DCP Identify-All not received, unicast works | Multicast filtered (group not joined on this interface, or switch IGMP/multicast filtering) | `Stack` joins `01:0E:CF:00:00:00` and `01:80:C2:00:00:0E` automatically; check with `ip maddr show dev <if>`. For other groups call `join_multicast()` |
| No frames at all | Wrong port, switch filtering, or traffic not addressed to this host | Check with `tcpdump -i <if> -e ether proto 0x8892`. For unicast frames to other MACs, enable promiscuous mode: `sudo ip link set <if> promisc on` |
| Own transmitted frames not seen | By design: outgoing frames are dropped | Use a second interface (see [testing.md](testing.md)) |
| `f.vlan` set but no tag in `f.raw` | NIC stripped the tag (VLAN offload); recovered from kernel metadata | Expected. Disable with `ethtool -K <if> rxvlan off` if you need it in-band |
| Garbage / crash after hook returns | Kept a `Bytes` span from `FrameView` | Copy the data inside the hook ([concepts.md#lifetimes](concepts.md#lifetimes)) |
| `run()` doesn't return after `stop()` | `stop()` was called before `run()` started | Call `stop()` only after `run()` is running, or use `poll()` with your own flag |
| Exception escapes `run()` | A hook threw | Catch inside the hook, or around `run()` and call it again |
| `RpcEndpoint` throws `bind: Address already in use` | Another process (or a second endpoint) owns UDP 34964 | One device endpoint per host/IP; pass `bind_ip`, or use port `0` for client endpoints |
| RPC hook never fires, Connect times out | Firewall drops UDP 34964 / 49152–65535, or peer sends to another IP | `sudo tcpdump -i <if> udp port 34964`; open the ports (`nft`/`ufw`) |
| RPC response ignored by peer | `activity`/`sequence` not copied, or wrong `server_boot` | Answer with `reply()` (uses `make_response()`) |
| Large Read/Write responses lost | No DCE/RPC fragmentation; datagram exceeds path MTU and IP fragments are dropped | Keep bodies < ~1400 bytes, or allow IP fragments on the path |
| Cyclic data jitter | Linux scheduling | `SCHED_FIFO`, CPU isolation, PREEMPT_RT kernel; disable NIC interrupt coalescing (`ethtool -C <if> rx-usecs 0`) |
| Integration test skipped | User namespaces disabled | Run `profinet_loopback` manually with `sudo` |
