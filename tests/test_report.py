#!/usr/bin/env python3
"""Tests for the dependency-free TelemetryLink HTML report renderer."""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List


ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / "tools" / "render_report.py"
SPEC = importlib.util.spec_from_file_location("telemetry_link_render_report", RENDERER)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load report renderer")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


def heartbeat(sequence: int, status: str, **extra: Any) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "kind": "message",
        "version": 1,
        "sequence": sequence,
        "sequence_status": status,
        "type": "heartbeat",
        "payload": {"uptime_ms": 1250, "status_flags": 3},
    }
    record.update(extra)
    return record


def imu(sequence: int, status: str, **extra: Any) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "kind": "message",
        "version": 1,
        "sequence": sequence,
        "sequence_status": status,
        "type": "imu_sample",
        "payload": {
            "accel_mps2": [0.2, -0.4, 9.83],
            "gyro_rad_s": [None, 0, -0.03],
        },
    }
    record.update(extra)
    return record


def motor(sequence: int, status: str, **extra: Any) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "kind": "message",
        "version": 1,
        "sequence": sequence,
        "sequence_status": status,
        "type": "motor_status",
        "payload": {
            "motor_id": 2,
            "state": "running",
            "rpm": 1450,
            "current_a": 2.25,
            "temperature_c": 38.5,
        },
    }
    record.update(extra)
    return record


def health(**changes: Any) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "kind": "health",
        "status": "ok",
        "bytes_received": 0,
        "frames_decoded": 0,
        "heartbeat_frames": 0,
        "imu_sample_frames": 0,
        "motor_status_frames": 0,
        "noise_bytes": 0,
        "invalid_versions": 0,
        "unknown_types": 0,
        "invalid_lengths": 0,
        "crc_failures": 0,
        "payload_failures": 0,
        "truncated_frames": 0,
        "truncated_bytes": 0,
        "sequence_gaps": 0,
        "missing_frames": 0,
        "out_of_order_frames": 0,
    }
    record.update(changes)
    return record


def mixed_records() -> List[Dict[str, Any]]:
    return [
        heartbeat(10, "first"),
        imu(12, "gap", expected_sequence=11, missing_before=1),
        motor(11, "out_of_order", expected_sequence=13),
        health(
            status="degraded",
            bytes_received=127,
            frames_decoded=3,
            heartbeat_frames=1,
            imu_sample_frames=1,
            motor_status_frames=1,
            noise_bytes=5,
            crc_failures=1,
            sequence_gaps=1,
            missing_frames=1,
            out_of_order_frames=1,
        ),
    ]


def jsonl(records: List[Dict[str, Any]]) -> bytes:
    return (
        "\n".join(json.dumps(record, separators=(",", ":")) for record in records)
        + "\n"
    ).encode(
        "utf-8",
    )


class ReportRendererTest(unittest.TestCase):
    def test_mixed_stream_renders_complete_semantic_report(self) -> None:
        messages, summary = REPORT.parse_jsonl(jsonl(mixed_records()))
        output = REPORT.render_html(messages, summary)

        self.assertIn("<!doctype html>", output)
        self.assertIn("TelemetryLink stream report", output)
        self.assertIn('<strong>degraded</strong>', output)
        self.assertIn('role="status"', output)
        self.assertIn("Parser and sequence counters", output)
        self.assertIn('<main class="reader" id="report" tabindex="-1">', output)
        self.assertEqual(output.count('<tr class="record-row'), 4)
        self.assertIn('data-state="gap" data-focused="true"', output)
        self.assertIn('data-state="out_of_order"', output)
        self.assertIn("1 missing", output)
        self.assertIn("expected 13", output)
        self.assertIn("Accel [0.2, -0.4, 9.83]", output)
        self.assertIn("Motor 2 / running / 1,450 rpm", output)
        self.assertIn("sequence 12 / imu_sample", output)
        self.assertIn("First problem:", output)
        self.assertIn("&quot;missing_before&quot;:1", output)
        self.assertIn(
            'aria-label="Chronological JSONL record table; horizontally scrollable"',
            output,
        )
        self.assertIn(
            'aria-label="Raw JSON for the focused record; scrollable"', output
        )
        self.assertIn(
            'aria-label="End-of-stream parser recovery and sequence counters; horizontally scrollable"',
            output,
        )
        self.assertEqual(output.count('tabindex="0" role="region"'), 3)
        self.assertIn('scope="row" class="number">12</th>', output)
        self.assertIn('scope="row" class="number">EOF</th>', output)
        self.assertIn("They overlap", output)
        self.assertNotIn("continuity%", output.lower())
        self.assertNotIn("lost", output.lower())
        self.assertNotIn("dropped", output.lower())

    def test_clean_empty_stream_has_explicit_empty_state(self) -> None:
        messages, summary = REPORT.parse_jsonl(jsonl([health()]))
        output = REPORT.render_html(messages, summary)
        self.assertEqual(messages, [])
        self.assertIn("<strong>ok</strong>", output)
        self.assertIn("0 frames, no detected anomalies", output)
        self.assertIn("No decoded messages", output)
        self.assertIn("No sequence baseline", output)
        self.assertNotIn("100.0%", output)

    def test_sequence_wraparound_is_validated(self) -> None:
        records = [
            heartbeat(0xFFFFFFFF, "first"),
            heartbeat(0, "in_order", expected_sequence=0),
            health(bytes_received=44, frames_decoded=2, heartbeat_frames=2),
        ]
        messages, _ = REPORT.parse_jsonl(jsonl(records))
        self.assertEqual([message["sequence"] for message in messages], [0xFFFFFFFF, 0])

    def test_stdin_stdout_and_both_output_path_forms(self) -> None:
        source = jsonl(mixed_records())
        stdout_result = subprocess.run(
            (sys.executable, str(RENDERER)),
            input=source,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(stdout_result.returncode, 0, stdout_result.stderr.decode())
        self.assertTrue(stdout_result.stdout.startswith(b"<!doctype html>"))

        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "capture.jsonl"
            positional_path = Path(directory) / "positional.html"
            option_path = Path(directory) / "option.html"
            input_path.write_bytes(source)
            positional = subprocess.run(
                (sys.executable, str(RENDERER), str(input_path), str(positional_path)),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            option = subprocess.run(
                (
                    sys.executable,
                    str(RENDERER),
                    str(input_path),
                    "--output",
                    str(option_path),
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(positional.returncode, 0, positional.stderr.decode())
            self.assertEqual(option.returncode, 0, option.stderr.decode())
            self.assertEqual(positional_path.read_bytes(), stdout_result.stdout)
            self.assertEqual(option_path.read_bytes(), stdout_result.stdout)

    def test_output_is_byte_for_byte_deterministic(self) -> None:
        source = jsonl(mixed_records())
        results = [
            subprocess.run(
                (sys.executable, str(RENDERER), "-", "-"),
                input=source,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            for _ in range(3)
        ]
        self.assertTrue(all(result.returncode == 0 for result in results))
        self.assertEqual(results[0].stdout, results[1].stdout)
        self.assertEqual(results[1].stdout, results[2].stdout)

    def test_untrusted_text_is_escaped_and_contract_injection_is_rejected(self) -> None:
        self.assertEqual(
            REPORT._escape('<script data-x="&">'),
            "&lt;script data-x=&quot;&amp;&quot;&gt;",
        )
        records = mixed_records()
        records[2]["payload"]["state"] = "<script>alert(1)</script>"
        with self.assertRaisesRegex(REPORT.ReportError, "payload.state"):
            REPORT.parse_jsonl(jsonl(records))

        result = subprocess.run(
            (sys.executable, str(RENDERER)),
            input=jsonl(records),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertNotIn(b"<script>", result.stderr)

        hostile_key = jsonl([health()]).replace(
            b'"kind":"health"', b'"kind":"health","bad\\n\\u001bkey":1'
        )
        hostile_result = subprocess.run(
            (sys.executable, str(RENDERER)),
            input=hostile_key,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(hostile_result.returncode, 2)
        self.assertNotIn(b"\x1b", hostile_result.stderr)
        self.assertIn(b"bad\\x0a\\x1bkey", hostile_result.stderr)

    def test_malformed_jsonl_and_record_order_are_rejected(self) -> None:
        cases = {
            "empty": b"",
            "not newline terminated": jsonl([health()]).rstrip(b"\n"),
            "invalid JSON": b'{"kind":"health"\n',
            "blank line": b"\n" + jsonl([health()]),
            "no health": jsonl([heartbeat(1, "first")]),
            "multiple health": jsonl([health(), health()]),
            "trailing message": jsonl([health(), heartbeat(1, "first")]),
            "array record": b"[]\n",
            "unknown kind": b'{"kind":"summary"}\n',
            "nonstandard constant": b'{"kind":"health","status":NaN}\n',
        }
        for name, source in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(REPORT.ReportError):
                    REPORT.parse_jsonl(source)

    def test_duplicate_unknown_missing_and_invalid_fields_are_rejected(self) -> None:
        duplicate = b'{"kind":"health","kind":"health"}\n'
        with self.assertRaisesRegex(REPORT.ReportError, "duplicate object key"):
            REPORT.parse_jsonl(duplicate)

        records = mixed_records()
        records[0]["invented"] = 1
        with self.assertRaisesRegex(REPORT.ReportError, "unknown field"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        del records[0]["version"]
        with self.assertRaisesRegex(REPORT.ReportError, "missing field"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[0]["sequence"] = True
        with self.assertRaisesRegex(REPORT.ReportError, "sequence must be an integer"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[1]["payload"]["accel_mps2"] = [1.0, 2.0]
        with self.assertRaisesRegex(REPORT.ReportError, "three values"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[0]["sequence_status"] = []
        with self.assertRaisesRegex(REPORT.ReportError, "sequence_status"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[2]["payload"]["state"] = {}
        with self.assertRaisesRegex(REPORT.ReportError, "payload.state"):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[-1]["status"] = []
        with self.assertRaisesRegex(REPORT.ReportError, "status must be"):
            REPORT.parse_jsonl(jsonl(records))

    def test_sequence_annotations_are_strictly_checked(self) -> None:
        modifications = (
            (0, "sequence_status", "in_order"),
            (1, "expected_sequence", 10),
            (1, "missing_before", 2),
            (2, "sequence_status", "in_order"),
        )
        for index, field, value in modifications:
            with self.subTest(index=index, field=field):
                records = mixed_records()
                records[index][field] = value
                with self.assertRaises(REPORT.ReportError):
                    REPORT.parse_jsonl(jsonl(records))

    def test_health_totals_status_and_correlations_are_checked(self) -> None:
        changes = (
            {"frames_decoded": 2},
            {"heartbeat_frames": 2},
            {"bytes_received": 20},
            {"sequence_gaps": 0},
            {"missing_frames": 0},
            {"out_of_order_frames": 0},
            {"status": "ok"},
            {"truncated_frames": 1, "truncated_bytes": 0},
        )
        for change in changes:
            with self.subTest(change=change):
                records = mixed_records()
                records[-1].update(change)
                with self.assertRaises(REPORT.ReportError):
                    REPORT.parse_jsonl(jsonl(records))

    def test_nonfinite_and_out_of_range_numbers_are_rejected(self) -> None:
        records = mixed_records()
        records[0]["payload"]["uptime_ms"] = 1 << 32
        with self.assertRaises(REPORT.ReportError):
            REPORT.parse_jsonl(jsonl(records))

        records = mixed_records()
        records[1]["payload"]["gyro_rad_s"][0] = float("inf")
        source = jsonl(records)
        self.assertIn(b"Infinity", source)
        with self.assertRaises(REPORT.ReportError):
            REPORT.parse_jsonl(source)

        records = mixed_records()
        records[-1]["noise_bytes"] = -1
        with self.assertRaises(REPORT.ReportError):
            REPORT.parse_jsonl(jsonl(records))

    def test_cli_reports_read_write_and_validation_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing.jsonl"
            read_result = subprocess.run(
                (sys.executable, str(RENDERER), str(missing)),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(read_result.returncode, 1)
            self.assertIn(b"cannot read input", read_result.stderr)

            invalid_result = subprocess.run(
                (sys.executable, str(RENDERER)),
                input=b"{}\n",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(invalid_result.returncode, 2)
            self.assertIn(b"invalid TelemetryLink JSONL", invalid_result.stderr)

            input_path = Path(directory) / "capture.jsonl"
            input_path.write_bytes(jsonl([health()]))
            output_directory = Path(directory) / "directory-output"
            output_directory.mkdir()
            write_result = subprocess.run(
                (sys.executable, str(RENDERER), str(input_path), str(output_directory)),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(write_result.returncode, 1)
            self.assertIn(b"cannot write output", write_result.stderr)


if __name__ == "__main__":
    unittest.main()
