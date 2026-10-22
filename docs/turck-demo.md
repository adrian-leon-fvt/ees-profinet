# Turck TBPN-L5-4FDI-4FDX bench demo

This demo discovers device identity and can request read-only I/O snapshots.
It does not set station names/IP addresses, connect an IO controller, write
parameters, reset/unlock the safe unit, or energize outputs. Use an isolated
bench network with hazardous loads disconnected. DCP identification is not
authentication or proof that a safety function works.

## Source and GSDML

Prepared against the Turck
[TBPN-L5-4FDI-4FDX product page](https://www.turck.us/en/product/100001826)
(Ident. no. 100001826), accessed **2026-10-19**. This records documentation
provenance, not a successful hardware commissioning test.

Download **Configuration File -> GSDML files** from that page when configuring
a controller or inspecting the device model. Download the user manual and
safety manual there too. No GSDML is needed to run read-only discovery.
Vendor files are not redistributed here; obtain them from Turck and follow
their license terms. Keep the downloaded ZIP/version with your bench records.

The supplied `TBPN-L_PROFINET.ZIP` contains:

```text
GSDML-V2.35-Turck-TBPN_L5_4FDI_4FDX-20211210-010735.xml
VendorID = 0x013D; DeviceID = 0xB002
```

Select the L5 model, not the L1/LL descriptions also present in the ZIP.
Check compatibility with the actual hardware/firmware before engineering it.

## Discover

Connect a dedicated Linux Ethernet interface to the powered module using
the correct M12 Ethernet adapter. DCP uses Ethernet layer 2; no matching
IPv4 subnet is necessary. In WSL, use an interface that actually exposes
the physical layer-2 network; default NAT networking is not sufficient.
Use native Linux if the NIC cannot be exposed.

```sh
cmake --preset linux
cmake --build --preset linux --target pn_turck_demo
build/linux/pn_turck_demo --self-test
sudo build/linux/pn_turck_demo eth0
```

Replace `eth0` with the dedicated interface. `CAP_NET_RAW` is needed for the
hardware run, not the self-test. Expected reply shape (MAC is illustrative):

```text
Sending read-only DCP Identify-All; waiting 2 seconds.
02:00:00:00:00:01 vendor=013D device=B002 TBPN-L5-4FDI-4FDX identity matched
```

Exit codes: `0` matching identity received, `1` usage/socket/self-test error,
`2` no matching reply within two seconds. Other devices are printed but do
not count as a match. A missing reply usually means wrong interface, missing
power, multicast filtering, or a router/NAT between host and device.

Implementation: [pn_turck_demo.cpp](../examples/pn_turck_demo.cpp).
`Stack::send()` transmits the DCP request and a FrameID hook consumes replies.
The self-test checks identity decoding, transaction filtering, block padding
and malformed input without sending anything.

## Read I/O status

With the device's existing numeric IPv4 address reachable from Linux:

```sh
build/linux/pn_turck_demo --status 192.168.1.100
# Optional repeated display, using the system's watch command:
watch -n 1 build/linux/pn_turck_demo --status 192.168.1.100
```

Replace the example IP. This mode uses UDP RPC, so it does not need root or
raw-socket access. It sends only `ReadImplicit` requests (opnum 5), API 0,
subslot 1, record index `0x8028` (acyclic input object) for slots 1 through 4.
No AR, F-Host, parameterization or output write is attempted.

It prints:

- Slot 1: configured F-input bytes and F-status, **unvalidated raw data**.
- Slot 2: safe-unit mode flags and C0..C7 diagnostic flags: RGG, WAIT, TEST,
  ERRFIN, TCCH0/1, OVL. These are connector diagnostics, not individual pin states.
- Slot 3: configured FBO0..15 fieldbus bits, not a fixed physical pin map.
- Slot 4: raw module-status bytes.

Each snapshot includes IOCS/IOPS and raw input bytes. BAD/missing IOPS suppresses
state interpretation. A GOOD IOPS is only PROFINET provider quality, not
PROFIsafe validation. The six-byte F-input telegram is not checked for F-CRC,
consecutive numbers or watchdog state. Do not use any demo value for protection.
Snapshots are sequential, not atomic or real-time.

**Hardware support is not yet verified.** The standard record may be rejected
or have no live input data until an engineering tool/controller configures the
device and establishes its IOCRs. Do not expect read-only discovery to activate
the safety application. Unavailable reads report the PNIO error or a two-second
timeout per slot; exit `2` means at least one slot failed, not an all-zero
successful read. The demo does not bypass that limitation or fabricate data.
Use a supported controller/F-Host setup for live safety application data.

The Python-standard-library loopback fixture checks request bytes and
successful, denied, malformed and BAD-quality responses without Turck hardware:

```sh
ctest --test-dir build/linux -R 'turck_' --output-on-failure
```

## Where PROFIsafe data would go

This is the next integration boundary, **not implemented by the discovery demo**:

```text
Application <-> validated PROFIsafe F-Host
            <-> PROFINET IO-Controller state machines / IOCR mapping
            <-> ees-profinet RpcEndpoint + Stack
            <-> Turck F-Device
```

The selected GSDML describes these application data lengths. Input/output
directions are from the controller's perspective:

| Slot | Module | Module/submodule ID | Input bytes | Output bytes |
|---|---|---|---:|---:|
| 1 | PROFIsafe | `0x00186002` | 6 | 6 |
| 2 | Safety status | `0x012C2009` | 14 | 2 |
| 3 | Safety Fieldbus Bits | `0x04082000` | 2 | 2 |
| 4 | Module status | `0x00080000` | 2 | 0 |

Slot 1's six bytes are two configured safety-data bytes, one F-status/control
byte and three F-CRC bytes. Signal assignments come from Turck Safety
Configurator, not a fixed pin-to-bit assumption. F-parameters use record
`0x0200`; the inspected GSDML has `F_WD_Time` default 150 ms and allowed range
80..65535 ms. Select timing using the required response-time analysis.
Match `F_Dest_Add` to the configured hardware F-address and `F_iPar_CRC` to
the actual safety-configuration report; do not copy example CRCs.

The separate "use for non-safe PLC" DAP omits the PROFIsafe module. Its
diagnostics/fieldbus bits are not safety-validated inputs.

A complete controller must establish AR/IOCR connections, write parameter
records, complete PrmEnd/ApplicationReady, handle alarms, and supervise
cyclic communication. `RpcEndpoint` provides transport, not those state
machines. Once established, the F-Host produces the six-byte F-telegram;
the controller places it at the negotiated output offset with IOPS/IOCS
and assembles the RT payload:

```cpp
// Integration sketch only; variables supplied by the missing controller.
// rt_payload = mapped C_SDU + required padding + four-byte APDU status.
stack.send(device_mac, negotiated_output_frame_id,
           rt_payload, profinet::Vlan{6, 0});
```

On receipt, `parse_apdu_status(frame)` and `cyclic_data(frame)` expose the
transport data. Validate negotiated lengths, identity and IO status before
extracting the F-telegram. The F-Host must validate it before application use.
Its watchdog must run even when no frames arrive. Frame spans expire after
the hook returns.

Do not send fabricated/replayed F-telegrams or force "valid/RUN" status.
`Stack::send()` does not compute F-CRC or schedule cyclic IO. See
[concepts](concepts.md#rpc-connection-management) and
[cyclic transmission recipe](cookbook.md#produce-cyclic-io-data) for transport.
The separate private `fvt-research/ees-profisafe` project covers device-side
engineering and pump GSDML authoring; neither project claims safety certification.
