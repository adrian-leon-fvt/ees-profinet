"""Exercise demo against a read-only UDP fixture, not physical hardware."""

import socket
import struct
import subprocess
import sys


def check(binary, server, scenario):
    process = subprocess.Popen(
        [binary, "--status", "127.0.0.2"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        for slot in range(1, 5):
            request, peer = server.recvfrom(2048)
            assert request[0:2] == b"\x04\x00"
            assert struct.unpack_from("<H", request, 68)[0] == 5  # ReadImplicit
            assert struct.unpack_from("<I", request, 64)[0] == slot
            assert struct.unpack_from("<5I", request, 80) == (1024, 64, 1024, 0, 64)
            record_header = bytearray(request[100:])
            assert record_header[:6] == b"\x00\x09\x00\x3c\x01\x00"
            assert len(record_header) == 64
            assert struct.unpack_from(">H", record_header, 6)[0] == slot
            assert record_header[8:28] == bytes(20)  # ARUUID, API
            assert struct.unpack_from(">HH", record_header, 28) == (slot, 1)
            assert struct.unpack_from(">HI", record_header, 34) == (0x8028, 512)
            assert record_header[40:] == bytes(24)  # TargetARUUID, padding

            data = {
                1: b"\x05\x00\x10\x00\x00\x00",
                2: b"\x01" + bytes(5) + b"\x01" * 8,
                3: b"\x05\x80",
                4: b"\x00\x00",
            }[slot]
            iops = 0 if scenario == "bad-quality" and slot == 2 else 0x80
            content = bytes([1, 0x80, 1, iops]) + struct.pack(">H", len(data)) + data
            if scenario == "malformed" and slot == 2:
                content = content[:4] + b"\xff\xff" + data
            input_object = struct.pack(">HHBB", 0x0015, len(content) + 2, 1, 0) + content
            record_header[:2] = b"\x80\x09"
            struct.pack_into(">I", record_header, 36, len(input_object))
            args = record_header + input_object
            status = b"\xde\x80\xb0\x00" if scenario == "denied" and slot == 2 else bytes(4)
            body = status + struct.pack("<4I", len(args), 1024, 0, len(args)) + args
            header = bytearray(request[:80])
            header[1:3] = b"\x02\x0a"
            struct.pack_into("<H", header, 74, len(body))
            unrelated = bytearray(header)
            unrelated[40] ^= 1  # Wrong activity must not satisfy this read.
            server.sendto(unrelated + body, peer)
            server.sendto(header + body, peer)

        stdout, stderr = process.communicate(timeout=3)
        expected = 2 if scenario in ("denied", "malformed") else 0
        assert process.returncode == expected, (scenario, stdout, stderr)
        assert "Configured F-input bytes=05 00" in stdout
        assert "FBO0..15: 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 1" in stdout
        if scenario == "denied":
            assert "PNIO status DE 80 B0 00" in stderr
        elif scenario == "malformed":
            assert "truncated InputDataObject" in stderr
        elif scenario == "bad-quality":
            assert "Provider status absent/BAD" in stdout
            assert "C0:" not in stdout
        else:
            assert "C0: RGG=1 WAIT=0" in stdout
            assert "protective=1 configuration=0 unknown=0" in stdout
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=3)


if __name__ == "__main__":
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        try:
            server.bind(("127.0.0.2", 34964))
        except OSError as error:
            print(f"Cannot bind isolated loopback fixture: {error}", file=sys.stderr)
            sys.exit(77)
        server.settimeout(3)
        for scenario in ("ok", "denied", "malformed", "bad-quality"):
            check(sys.argv[1], server, scenario)
    invalid = subprocess.run(
        [sys.argv[1], "--status", "invalid-ip"], capture_output=True, text=True
    )
    assert invalid.returncode == 1
    assert "numeric IPv4" in invalid.stderr
    print("Turck status loopback passed; no device or output writes.")
