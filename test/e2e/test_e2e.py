"""End-to-end tests: profinet-py (IO-Controller) against profinet_e2e_device.

Run via run_e2e.sh, which creates the veth pair and sets PN_DEVICE.
"""

import os
import subprocess
import time

import pytest
from profinet import dcp
from profinet.exceptions import PNIOError
from profinet.cyclic import CyclicController
from profinet.rpc import IOCRSetup, IOSlot, RPCCon
from profinet.rt import build_iocr_configs
from profinet.util import ethernet_socket, get_mac

CTL_IF = "pn_ctl"
DEV_IF = "pn_dev"
DEV_IP = "10.0.0.2"
DEV_NAME = "ees-e2e-dev"
TIMEOUT = 1


def sh(cmd):
    subprocess.run(cmd, shell=True, check=True)


@pytest.fixture
def device():
    """Fresh device per test, in its own netns on a fresh veth pair.

    Own netns: device and controller both need UDP port 34964 (the device
    sends ApplicationReady there), and one netns would also short-circuit
    RPC over loopback. Killing the device drops the netns and the veth.
    """
    sh(f"ip link add name {DEV_IF} type veth peer name {CTL_IF}")
    sh(f"ip addr add 10.0.0.1/24 dev {CTL_IF} && ip link set {CTL_IF} up")
    script = (
        f"while ! ip link show {DEV_IF} >/dev/null 2>&1; do sleep 0.02; done;"
        f"ip link set lo up; ip addr add {DEV_IP}/24 dev {DEV_IF}; ip link set {DEV_IF} up;"
        f'exec "$0" {DEV_IF} {DEV_IP} {DEV_NAME}'
    )
    proc = subprocess.Popen(
        ["unshare", "-n", "sh", "-c", script, os.environ["PN_DEVICE"]],
        stdout=subprocess.PIPE,
        text=True,
    )
    sh(f"ip link set {DEV_IF} netns {proc.pid}")
    assert proc.stdout.readline().strip() == "ready"
    yield proc
    proc.terminate()
    proc.wait(timeout=5)
    sh(f"ip link del {CTL_IF} 2>/dev/null || true")


@pytest.fixture
def ctl(device):
    """Raw controller socket and its MAC."""
    sock = ethernet_socket(CTL_IF)
    yield sock, get_mac(CTL_IF)
    sock.close()


def identify(ctl, name=None):
    sock, mac = ctl
    if name is None:
        xid = dcp.send_discover(sock, mac)
    else:
        xid = dcp.send_request(sock, mac, dcp.PNDCPBlock.NAME_OF_STATION, name.encode())
    rsp = dcp.read_response(sock, mac, timeout_sec=TIMEOUT, once=True, expected_xid=xid, strict_xid=True)
    return [dcp.DCPDeviceDescription(m, b) for m, b in rsp.items()]


def only_device(ctl):
    found = identify(ctl)
    assert len(found) == 1
    return found[0]


def test_identify_all_reports_device_properties(ctl):
    d = only_device(ctl)
    assert d.name == DEV_NAME
    assert d.device_type == "ees-e2e"
    assert (d.ip, d.netmask, d.gateway) == (DEV_IP, "255.255.255.0", "0.0.0.0")
    assert (d.vendor_high, d.vendor_low, d.device_high, d.device_low) == (0x00, 0x2A, 0x02, 0x03)


def test_identify_by_name_filters(ctl):
    assert [d.name for d in identify(ctl, DEV_NAME)] == [DEV_NAME]
    assert identify(ctl, "someone-else") == []


def test_get_name_and_ip(ctl):
    sock, mac = ctl
    target = only_device(ctl).mac
    assert dcp.get_param(sock, mac, target, "name", timeout_sec=TIMEOUT, strict_xid=True) == DEV_NAME.encode()
    ip = dcp.get_param(sock, mac, target, "ip", timeout_sec=TIMEOUT, strict_xid=True)
    assert ip[-12:] == bytes([10, 0, 0, 2, 255, 255, 255, 0, 0, 0, 0, 0])


def test_set_name(ctl):
    sock, mac = ctl
    assert dcp.set_param(sock, mac, only_device(ctl).mac, "name", "renamed", timeout_sec=TIMEOUT)
    assert only_device(ctl).name == "renamed"


def test_set_ip(ctl):
    sock, mac = ctl
    target = only_device(ctl).mac
    assert dcp.set_ip(sock, mac, target, "10.0.0.9", "255.255.0.0", "10.0.0.254", timeout_sec=TIMEOUT)
    d = only_device(ctl)
    assert (d.ip, d.netmask, d.gateway) == ("10.0.0.9", "255.255.0.0", "10.0.0.254")


def test_signal(ctl):
    sock, mac = ctl
    assert dcp.signal_device(sock, mac, only_device(ctl).mac, timeout_sec=TIMEOUT)


def test_read_implicit(ctl):
    with RPCCon(only_device(ctl), timeout=TIMEOUT) as con:
        assert con.read_implicit(api=0, slot=0, subslot=1, idx=0xAFF0).payload == b"hello"


def test_read_unknown_index_returns_pnio_error(ctl):
    with RPCCon(only_device(ctl), timeout=TIMEOUT) as con, pytest.raises(PNIOError):
        con.read_implicit(api=0, slot=0, subslot=1, idx=0x1234)


def test_write_then_read(ctl):
    with RPCCon(only_device(ctl), timeout=TIMEOUT) as con:
        con.write(api=0, slot=0, subslot=1, idx=0x1234, data=b"\x01\x02\x03")
        assert con.read_implicit(api=0, slot=0, subslot=1, idx=0x1234).payload == b"\x01\x02\x03"


SLOT = IOSlot(slot=1, subslot=1, input_length=4, output_length=4)
SETUP = IOCRSetup(slots=[SLOT], reduction_ratio=8, watchdog_factor=20)  # 8 ms cycle


@pytest.fixture
def ar(ctl):
    """Connected AR through PrmEnd and ApplicationReady."""
    _, mac = ctl
    with RPCCon(only_device(ctl), timeout=TIMEOUT) as con:
        result = con.connect(src_mac=mac, iocr_setup=SETUP)
        con.prm_end()
        con.application_ready(timeout=3)
        yield con, result, ctl


def test_connect_assigns_frame_ids_and_alarm_ref(ctl):
    with RPCCon(only_device(ctl), timeout=TIMEOUT) as con:
        r = con.connect(src_mac=ctl[1], iocr_setup=SETUP)
        assert (r.input_frame_id, r.output_frame_id, r.device_alarm_ref) == (0xC001, 0xC100, 1)


def test_connect_prm_end_application_ready(ar):
    pass  # the fixture is the test: each step raises on timeout or error


def test_cyclic_echoes_outputs_into_inputs(ar):
    con, r, (_, mac) = ar
    dev_mac = bytes.fromhex(only_device(ar[2]).mac.replace(":", ""))
    inp, out = build_iocr_configs([SLOT], r.input_frame_id, r.output_frame_id, reduction_ratio=8, watchdog_factor=20)
    with CyclicController(CTL_IF, mac, dev_mac, inp, out) as cyc:
        cyc.set_output_data(1, 1, b"\xde\xad\xbe\xef")
        deadline = time.time() + 3
        while time.time() < deadline and (
            cyc.get_input_data(1, 1) != b"\xde\xad\xbe\xef" or cyc.stats.frames_received < 10
        ):
            time.sleep(0.01)
        assert cyc.get_input_data(1, 1) == b"\xde\xad\xbe\xef"
        assert cyc.stats.frames_received >= 10
        assert cyc.stats.frames_invalid == 0
        assert cyc.is_input_good(1, 1)
