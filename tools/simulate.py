#!/usr/bin/env python3
"""Generate deterministic TelemetryLink byte streams for demos and tests."""

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import Sequence, Tuple


MAGIC = b"TLNK"
VERSION = 1

HEARTBEAT = 0x01
IMU_SAMPLE = 0x02
MOTOR_STATUS = 0x03

# magic + version + type + payload length + sequence number
FIXED_HEADER_SIZE = 12


def _payload_for(index: int) -> Tuple[int, bytes]:
    """Return a repeatable mixture of all supported message payloads."""
    message_kind = index % 3
    if message_kind == 0:
        payload = struct.pack(
            "<IH",
            1_000 + index * 250,
            index & 0x0003,
        )
        return HEARTBEAT, payload

    if message_kind == 1:
        scale = float(index + 1)
        payload = struct.pack(
            "<6f",
            0.10 * scale,
            -0.20 * scale,
            9.81 + 0.01 * scale,
            0.005 * scale,
            -0.010 * scale,
            0.015 * scale,
        )
        return IMU_SAMPLE, payload

    payload = struct.pack(
        "<BBiff",
        index % 4,
        (index // 3) % 3,
        1_200 + index * 25,
        1.5 + index * 0.125,
        35.0 + index * 0.25,
    )
    return MOTOR_STATUS, payload


def encode_frame(sequence: int, message_type: int, payload: bytes) -> bytes:
    """Encode one frame using CRC-32 over version through payload."""
    body = struct.pack("<BBHI", VERSION, message_type, len(payload), sequence)
    body += payload
    checksum = zlib.crc32(body) & 0xFFFFFFFF
    return MAGIC + body + struct.pack("<I", checksum)


def _corrupt_payload(frame: bytes, index: int) -> bytes:
    """Flip one payload bit without updating the stored CRC."""
    damaged = bytearray(frame)
    payload_size = len(frame) - FIXED_HEADER_SIZE - 4
    payload_offset = FIXED_HEADER_SIZE + (index % payload_size)
    damaged[payload_offset] ^= 0x40
    return bytes(damaged)


def generate_stream(scenario: str, count: int, start_sequence: int) -> bytes:
    """Build a deterministic stream for one of the documented scenarios."""
    output = bytearray()

    if scenario == "mixed":
        # Exercise both noise scanning and immediate rejection of an impossible
        # length before the first valid frame.
        output.extend(b"\x00\xffnoise")
        output.extend(MAGIC)
        output.extend(struct.pack("<BBHI", VERSION, HEARTBEAT, 0xFFFF, 0))
        output.extend(b"\xa5")

    for index in range(count):
        sequence = (start_sequence + index) & 0xFFFFFFFF

        if scenario == "dropped" and index % 5 == 2:
            continue
        if scenario == "mixed" and index % 7 == 2:
            continue

        message_type, payload = _payload_for(index)
        frame = encode_frame(sequence, message_type, payload)

        if scenario == "corrupted" and index % 5 == 2:
            frame = _corrupt_payload(frame, index)
        elif scenario == "mixed" and index % 7 == 4:
            frame = _corrupt_payload(frame, index)

        if scenario == "mixed" and index % 7 == 5:
            output.extend(b"\x13\x37garbage")
        output.extend(frame)

    return bytes(output)


def _positive_count(text: str) -> int:
    value = int(text, 10)
    if not 1 <= value <= 1_000_000:
        raise argparse.ArgumentTypeError("count must be between 1 and 1000000")
    return value


def _sequence_number(text: str) -> int:
    base = 16 if text.lower().startswith("0x") else 10
    value = int(text, base)
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("sequence must fit in uint32")
    return value


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a TelemetryLink binary stream.")
    parser.add_argument(
        "--scenario",
        choices=("normal", "dropped", "corrupted", "mixed"),
        default="normal",
        help="fault pattern to generate (default: normal)",
    )
    parser.add_argument(
        "--count",
        type=_positive_count,
        default=18,
        help="number of logical sequence numbers to generate (default: 18)",
    )
    parser.add_argument(
        "--start-sequence",
        type=_sequence_number,
        default=0,
        help="first uint32 sequence number; decimal or 0x-prefixed",
    )
    parser.add_argument(
        "--output",
        metavar="PATH",
        default="-",
        help="binary output path, or - for stdout (default: -)",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] = ()) -> int:
    args = _parse_args(argv)
    data = generate_stream(args.scenario, args.count, args.start_sequence)

    try:
        if args.output == "-":
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        else:
            Path(args.output).write_bytes(data)
    except BrokenPipeError:
        # Prevent Python's interpreter-shutdown flush from retrying the broken
        # stream, printing a second error, and replacing this exit status.
        try:
            null_fd = os.open(os.devnull, os.O_WRONLY)
            try:
                os.dup2(null_fd, sys.stdout.fileno())
            finally:
                os.close(null_fd)
        except OSError:
            pass
        return 0
    except OSError as error:
        print("simulate.py: cannot write output: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
