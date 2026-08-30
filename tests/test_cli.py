#!/usr/bin/env python3
"""End-to-end checks for the simulator and telemetry-link CLI."""

import argparse
import json
import os
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple


CLI = Path()
SIMULATOR = Path()


def run_process(
    command: Sequence[str],
    data: bytes = b"",
    environment: Optional[Mapping[str, str]] = None,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        list(command),
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
        timeout=10,
    )


def simulator_bytes(scenario: str, count: int = 21) -> bytes:
    result = run_process(
        (
            sys.executable,
            str(SIMULATOR),
            "--scenario",
            scenario,
            "--count",
            str(count),
        )
    )
    if result.returncode != 0:
        raise AssertionError(
            "simulator failed with exit {}:\n{}".format(
                result.returncode,
                result.stderr.decode("utf-8", errors="replace"),
            )
        )
    return result.stdout


def decode_json_lines(output: bytes) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    if not output.endswith(b"\n"):
        raise AssertionError("CLI output is not newline terminated")

    def reject_nonstandard_constant(value: str) -> None:
        raise ValueError("non-standard JSON constant: {}".format(value))

    try:
        records = [
            json.loads(line, parse_constant=reject_nonstandard_constant)
            for line in output.decode("utf-8").splitlines()
        ]
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise AssertionError("CLI did not produce valid JSON Lines") from error

    if not records or records[-1].get("kind") != "health":
        raise AssertionError("last JSON Line is not the health summary")
    if any(record.get("kind") != "message" for record in records[:-1]):
        raise AssertionError("non-message record found before health summary")
    return records[:-1], records[-1]


def encode_frame(sequence: int, message_type: int, payload: bytes) -> bytes:
    body = struct.pack("<BBHI", 1, message_type, len(payload), sequence)
    checksum = zlib.crc32(body + payload) & 0xFFFFFFFF
    return b"TLNK" + body + payload + struct.pack("<I", checksum)


def run_cli_from_stdin(data: bytes) -> Tuple[bytes, List[Dict[str, Any]], Dict[str, Any]]:
    result = run_process((str(CLI), "-"), data)
    if result.returncode != 0:
        raise AssertionError(
            "CLI failed with exit {}:\n{}".format(
                result.returncode,
                result.stderr.decode("utf-8", errors="replace"),
            )
        )
    messages, health = decode_json_lines(result.stdout)
    return result.stdout, messages, health


class TelemetryLinkCliIntegrationTest(unittest.TestCase):
    def test_simulator_is_deterministic_for_every_scenario(self) -> None:
        for scenario in ("normal", "dropped", "corrupted", "mixed"):
            with self.subTest(scenario=scenario):
                first = simulator_bytes(scenario, count=11)
                second = simulator_bytes(scenario, count=11)
                self.assertEqual(first, second)
                self.assertTrue(first)

    def test_file_and_stdin_produce_the_same_json_lines(self) -> None:
        data = simulator_bytes("normal", count=12)
        stdin_output, messages, health = run_cli_from_stdin(data)

        with tempfile.TemporaryDirectory() as directory:
            stream_path = Path(directory) / "normal.tlink"
            generated = run_process(
                (
                    sys.executable,
                    str(SIMULATOR),
                    "--scenario",
                    "normal",
                    "--count",
                    "12",
                    "--output",
                    str(stream_path),
                )
            )
            self.assertEqual(generated.returncode, 0, generated.stderr.decode())
            self.assertEqual(generated.stdout, b"")
            self.assertEqual(stream_path.read_bytes(), data)

            file_result = run_process((str(CLI), str(stream_path)))
            self.assertEqual(file_result.returncode, 0, file_result.stderr.decode())
            self.assertEqual(file_result.stdout, stdin_output)

        self.assertEqual(len(messages), 12)
        self.assertEqual(messages[0]["sequence_status"], "first")
        self.assertTrue(
            all(message["sequence_status"] == "in_order" for message in messages[1:])
        )
        self.assertEqual(
            {message["type"] for message in messages},
            {"heartbeat", "imu_sample", "motor_status"},
        )
        payloads = {message["type"]: message["payload"] for message in messages[:3]}
        self.assertEqual(set(payloads["heartbeat"]), {"uptime_ms", "status_flags"})
        self.assertEqual(set(payloads["imu_sample"]), {"accel_mps2", "gyro_rad_s"})
        self.assertEqual(
            set(payloads["motor_status"]),
            {"motor_id", "state", "rpm", "current_a", "temperature_c"},
        )
        self.assertEqual(health["status"], "ok")
        self.assertEqual(health["frames_decoded"], 12)
        self.assertEqual(health["heartbeat_frames"], 4)
        self.assertEqual(health["imu_sample_frames"], 4)
        self.assertEqual(health["motor_status_frames"], 4)
        self.assertEqual(
            {
                message["payload"]["state"]
                for message in messages
                if message["type"] == "motor_status"
            },
            {"stopped", "running", "fault"},
        )

    def test_large_stream_and_default_stdin(self) -> None:
        data = simulator_bytes("normal", count=200)
        self.assertGreater(len(data), 4096)

        result = run_process((str(CLI),), data)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        messages, health = decode_json_lines(result.stdout)
        self.assertEqual(len(messages), 200)
        self.assertEqual(messages[-1]["sequence"], 199)
        self.assertEqual(health["status"], "ok")
        self.assertEqual(health["bytes_received"], len(data))

    def test_short_live_stream_is_emitted_before_eof(self) -> None:
        data = simulator_bytes("normal", count=1)
        process = subprocess.Popen(
            (str(CLI), "-"),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertIsNotNone(process.stdin)
        self.assertIsNotNone(process.stdout)
        self.assertIsNotNone(process.stderr)

        received = queue.Queue()
        reader = threading.Thread(
            target=lambda: received.put(process.stdout.readline()),
            daemon=True,
        )
        reader.start()
        first_line = b""
        arrived_before_eof = False
        try:
            process.stdin.write(data)
            process.stdin.flush()
            try:
                first_line = received.get(timeout=5)
                arrived_before_eof = bool(first_line)
            except queue.Empty:
                pass
        finally:
            process.stdin.close()
            reader.join(timeout=5)

        remainder = process.stdout.read()
        stderr = process.stderr.read()
        returncode = process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()
        self.assertTrue(
            arrived_before_eof,
            "CLI buffered a complete frame until the input stream closed",
        )
        self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
        messages, health = decode_json_lines(first_line + remainder)
        self.assertEqual(len(messages), 1)
        self.assertEqual(health["frames_decoded"], 1)

    @unittest.skipUnless(os.name == "posix", "closed-pipe behavior is POSIX-specific")
    def test_output_write_failure_is_reported(self) -> None:
        data = simulator_bytes("normal", count=1)
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run(
                (str(CLI), "-"),
                input=data,
                stdout=write_fd,
                stderr=subprocess.PIPE,
                check=False,
                restore_signals=False,
                timeout=10,
            )
        finally:
            os.close(write_fd)

        self.assertEqual(result.returncode, 1)
        self.assertIn(b"output write failed", result.stderr)

    def test_nonfinite_floats_are_strict_json_nulls(self) -> None:
        payload = struct.pack(
            "<6f",
            float("nan"),
            float("inf"),
            float("-inf"),
            -0.0,
            1.25,
            -2.5,
        )
        _, messages, health = run_cli_from_stdin(encode_frame(7, 0x02, payload))
        self.assertEqual(messages[0]["payload"]["accel_mps2"], [None, None, None])
        self.assertEqual(messages[0]["payload"]["gyro_rad_s"], [-0.0, 1.25, -2.5])
        self.assertEqual(health["status"], "ok")

    def test_empty_and_truncated_inputs_have_explicit_health(self) -> None:
        _, empty_messages, empty_health = run_cli_from_stdin(b"")
        self.assertEqual(empty_messages, [])
        self.assertEqual(empty_health["status"], "ok")
        self.assertEqual(empty_health["bytes_received"], 0)
        self.assertEqual(empty_health["frames_decoded"], 0)

        truncated = simulator_bytes("normal", count=1)[:5]
        _, messages, health = run_cli_from_stdin(truncated)
        self.assertEqual(messages, [])
        self.assertEqual(health["status"], "degraded")
        self.assertEqual(health["truncated_frames"], 1)
        self.assertEqual(health["truncated_bytes"], len(truncated))

    def test_cli_options_and_exit_codes(self) -> None:
        help_result = run_process((str(CLI), "--help"))
        self.assertEqual(help_result.returncode, 0)
        self.assertIn(b"--fail-on-degraded", help_result.stdout)
        self.assertIn(b"--diagnostics", help_result.stdout)
        self.assertIn(b"--color MODE", help_result.stdout)
        self.assertIn(b"Streams:", help_result.stdout)
        self.assertIn(b"stdout  One JSON object", help_result.stdout)
        self.assertEqual(help_result.stderr, b"")

        unknown_result = run_process((str(CLI), "--not-an-option"))
        self.assertEqual(unknown_result.returncode, 2)
        self.assertIn(b"unknown option", unknown_result.stderr)

        too_many_result = run_process((str(CLI), "first.bin", "second.bin"))
        self.assertEqual(too_many_result.returncode, 2)
        self.assertIn(b"too many input files", too_many_result.stderr)

        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing.bin"
            missing_result = run_process((str(CLI), str(missing)))
        self.assertEqual(missing_result.returncode, 1)
        self.assertIn(b"cannot open", missing_result.stderr)

        clean_data = simulator_bytes("normal", count=6)
        clean_result = run_process((str(CLI), "--fail-on-degraded", "-"), clean_data)
        self.assertEqual(clean_result.returncode, 0)
        _, clean_health = decode_json_lines(clean_result.stdout)
        self.assertEqual(clean_health["status"], "ok")

        damaged_data = simulator_bytes("mixed", count=18)
        damaged_result = run_process(
            (str(CLI), "--fail-on-degraded", "-"),
            damaged_data,
        )
        self.assertEqual(damaged_result.returncode, 3)
        _, damaged_health = decode_json_lines(damaged_result.stdout)
        self.assertEqual(damaged_health["status"], "degraded")

        for arguments in (("--color",), ("--color=rainbow",)):
            with self.subTest(arguments=arguments):
                invalid_color = run_process((str(CLI), *arguments))
                self.assertEqual(invalid_color.returncode, 2)
                self.assertIn(b"auto, always, or never", invalid_color.stderr)

        hostile_color = run_process((str(CLI), "--color=bad\x1b\nmode"))
        self.assertEqual(hostile_color.returncode, 2)
        self.assertNotIn(b"\x1b", hostile_color.stderr)
        self.assertIn(b"bad\\x1b\\x0amode", hostile_color.stderr)

    def test_diagnostics_are_opt_in_and_never_change_stdout(self) -> None:
        data = simulator_bytes("mixed", count=18)
        default_result = run_process((str(CLI), "-"), data)
        diagnostic_result = run_process(
            (str(CLI), "--diagnostics", "--color", "never", "-"), data
        )

        self.assertEqual(default_result.returncode, 0)
        self.assertEqual(default_result.stderr, b"")
        self.assertEqual(diagnostic_result.returncode, 0)
        self.assertEqual(diagnostic_result.stdout, default_result.stdout)
        _, health = decode_json_lines(diagnostic_result.stdout)
        self.assertEqual(health["status"], "degraded")
        self.assertEqual(
            diagnostic_result.stderr.replace(b"\r\n", b"\n"),
            b"TelemetryLink diagnostics\n"
            b"Status    Degraded - damage detected; valid frames recovered\n"
            b"Input     standard input\n"
            b"Bytes     498 received\n"
            b"Frames    13 decoded\n"
            b"Frame mix 5 heartbeat / 4 IMU / 4 motor\n"
            b"Framing   105 noise / 0 invalid version / 0 unknown type / 1 invalid length\n"
            b"Integrity 2 CRC failure / 0 payload failure / 0 truncated (0 bytes)\n"
            b"Sequence  5 gaps / 5 missing / 0 out of order\n",
        )
        self.assertNotIn(b"\x1b", diagnostic_result.stderr)

    def test_diagnostic_color_is_explicit_and_no_color_is_authoritative(self) -> None:
        data = simulator_bytes("normal", count=3)
        color_environment = os.environ.copy()
        color_environment.pop("NO_COLOR", None)
        colored = run_process(
            (str(CLI), "--diagnostics", "--color=always", "-"),
            data,
            color_environment,
        )
        self.assertEqual(colored.returncode, 0, colored.stderr.decode())
        self.assertIn(b"\x1b[34mTelemetryLink diagnostics", colored.stderr)
        self.assertIn(b"\x1b[32mClean\x1b[0m", colored.stderr)
        self.assertNotIn(b"\x1b", colored.stdout)

        no_color_environment = color_environment.copy()
        no_color_environment["NO_COLOR"] = ""
        uncolored = run_process(
            (str(CLI), "--diagnostics", "--color=always", "-"),
            data,
            no_color_environment,
        )
        self.assertEqual(uncolored.returncode, 0, uncolored.stderr.decode())
        self.assertNotIn(b"\x1b", uncolored.stderr)
        self.assertIn(
            b"TelemetryLink diagnostics\nStatus    Clean",
            uncolored.stderr.replace(b"\r\n", b"\n"),
        )
        self.assertEqual(uncolored.stdout, colored.stdout)

    def test_degraded_exit_still_writes_json_and_human_summary(self) -> None:
        data = simulator_bytes("mixed", count=18)
        result = run_process(
            (
                str(CLI),
                "--fail-on-degraded",
                "--diagnostics",
                "--color=never",
                "-",
            ),
            data,
        )
        self.assertEqual(result.returncode, 3)
        _, health = decode_json_lines(result.stdout)
        self.assertEqual(health["status"], "degraded")
        self.assertIn(b"Status    Degraded", result.stderr)
        self.assertIn(b"Sequence  5 gaps / 5 missing", result.stderr)

        truncated = simulator_bytes("normal", count=1)[:5]
        truncated_result = run_process(
            (str(CLI), "--diagnostics", "--color=never", "-"), truncated
        )
        self.assertEqual(truncated_result.returncode, 0)
        _, truncated_health = decode_json_lines(truncated_result.stdout)
        self.assertEqual(truncated_health["truncated_frames"], 1)
        self.assertIn(b"no valid frames recovered", truncated_result.stderr)

    def test_simulator_reports_output_errors_without_traceback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = run_process(
                (
                    sys.executable,
                    str(SIMULATOR),
                    "--output",
                    directory,
                )
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"cannot write output", result.stderr)
        self.assertNotIn(b"Traceback", result.stderr)

    @unittest.skipUnless(os.name == "posix", "closed-pipe behavior is POSIX-specific")
    def test_simulator_handles_closed_stdout_cleanly(self) -> None:
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run(
                (
                    sys.executable,
                    str(SIMULATOR),
                    "--count",
                    "100",
                ),
                stdout=write_fd,
                stderr=subprocess.PIPE,
                check=False,
                restore_signals=False,
                timeout=10,
            )
        finally:
            os.close(write_fd)

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b"")

    def test_fault_scenarios_report_damage_and_recover(self) -> None:
        expectations = {
            "normal": {"clean": True},
            "dropped": {"sequence_gaps": True},
            "corrupted": {"crc_failures": True, "sequence_gaps": True},
            "mixed": {
                "crc_failures": True,
                "sequence_gaps": True,
                "invalid_lengths": True,
                "noise_bytes": True,
            },
        }

        for scenario, expected in expectations.items():
            with self.subTest(scenario=scenario):
                data = simulator_bytes(scenario, count=21)
                _, messages, health = run_cli_from_stdin(data)

                self.assertGreater(len(messages), 0)
                self.assertEqual(health["frames_decoded"], len(messages))
                self.assertEqual(messages[-1]["sequence"], 20)

                if expected.get("clean"):
                    self.assertEqual(health["status"], "ok")
                    self.assertEqual(len(messages), 21)
                else:
                    self.assertEqual(health["status"], "degraded")
                    self.assertLess(len(messages), 21)

                for counter in (
                    "crc_failures",
                    "sequence_gaps",
                    "invalid_lengths",
                    "noise_bytes",
                ):
                    if expected.get(counter):
                        self.assertGreater(health[counter], 0)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--simulator", required=True, type=Path)
    return parser.parse_args(argv)


if __name__ == "__main__":
    arguments = parse_args(sys.argv[1:])
    CLI = arguments.cli.resolve()
    SIMULATOR = arguments.simulator.resolve()

    if not CLI.is_file():
        raise SystemExit("CLI executable not found: {}".format(CLI))
    if not SIMULATOR.is_file():
        raise SystemExit("simulator not found: {}".format(SIMULATOR))

    unittest.main(argv=[sys.argv[0]])
