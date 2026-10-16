"""End-to-end tests: profinet-py (IO-Controller) against profinet_e2e_device.

Run via run_e2e.sh, which creates the veth pair and sets PN_DEVICE.
"""

import os
import subprocess

import pytest
from profinet import dcp
from profinet.exceptions import PNIOError
from profinet.rpc import RPCCon
from profinet.util import ethernet_socket, get_mac

CTL_IF = "pn_ctl"
DEV_IF = "pn_dev"
DEV_IP = "10.0.0.2"
DEV_NAME = "ees-e2e-dev"
TIMEOUT = 1


@pytest.fixture
def device():
    """Fresh device process per test so Set/Write can't leak between tests."""
    proc = subprocess.Popen(
        [os.environ["PN_DEVICE"], DEV_IF, DEV_IP, DEV_NAME],
        stdout=subprocess.PIPE,
        text=True,
    )
    assert proc.stdout.readline().strip() == "ready"
    yield proc
    proc.terminate()
    proc.wait(timeout=5)


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
