#!/usr/bin/env python3
"""Render TelemetryLink CLI JSON Lines as an HTML report."""

import argparse
import html
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


UINT16_MAX = (1 << 16) - 1
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1

MESSAGE_BASE_KEYS = {
    "kind",
    "version",
    "sequence",
    "sequence_status",
    "type",
    "payload",
}
MESSAGE_OPTIONAL_KEYS = {"expected_sequence", "missing_before"}
HEALTH_COUNTER_KEYS = (
    "bytes_received",
    "frames_decoded",
    "heartbeat_frames",
    "imu_sample_frames",
    "motor_status_frames",
    "noise_bytes",
    "invalid_versions",
    "unknown_types",
    "invalid_lengths",
    "crc_failures",
    "payload_failures",
    "truncated_frames",
    "truncated_bytes",
    "sequence_gaps",
    "missing_frames",
    "out_of_order_frames",
)
HEALTH_KEYS = {"kind", "status", *HEALTH_COUNTER_KEYS}
DEGRADING_COUNTERS = (
    "noise_bytes",
    "invalid_versions",
    "unknown_types",
    "invalid_lengths",
    "crc_failures",
    "payload_failures",
    "truncated_frames",
    "sequence_gaps",
    "out_of_order_frames",
)
REPORT_DIAGNOSTIC_FIELDS = (
    "noise_bytes",
    "invalid_versions",
    "unknown_types",
    "invalid_lengths",
    "crc_failures",
    "payload_failures",
    "truncated_frames",
    "truncated_bytes",
    "sequence_gaps",
    "missing_frames",
    "out_of_order_frames",
)
TYPE_COUNT_KEYS = {
    "heartbeat": "heartbeat_frames",
    "imu_sample": "imu_sample_frames",
    "motor_status": "motor_status_frames",
}
FRAME_WIRE_BYTES = {"heartbeat": 22, "imu_sample": 40, "motor_status": 30}
SEQUENCE_STATUSES = {"first", "in_order", "gap", "out_of_order"}
MOTOR_STATES = {"stopped", "running", "fault"}


class ReportError(ValueError):
    """Raised when the JSONL stream does not match the CLI contract."""


class _DuplicateKeyError(ValueError):
    pass


def _object_from_pairs(pairs: Iterable[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKeyError("duplicate object key {!r}".format(key))
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError("non-standard JSON constant {!r}".format(value))


def _line_error(line_number: int, message: str) -> ReportError:
    return ReportError("line {}: {}".format(line_number, message))


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_integer(
    value: Any, field: str, line_number: int, minimum: int, maximum: int
) -> int:
    if not _is_integer(value) or not minimum <= value <= maximum:
        raise _line_error(
            line_number,
            "{} must be an integer from {} through {}".format(field, minimum, maximum),
        )
    return value


def _require_number_or_null(value: Any, field: str, line_number: int) -> None:
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _line_error(line_number, "{} must be a finite number or null".format(field))
    if not math.isfinite(value):
        raise _line_error(line_number, "{} must be a finite number or null".format(field))


def _require_exact_keys(
    record: Mapping[str, Any],
    required: set,
    optional: set,
    line_number: int,
    name: str,
) -> None:
    actual = set(record)
    missing = sorted(required - actual)
    unknown = sorted(actual - required - optional)
    if missing:
        raise _line_error(
            line_number, "{} is missing field(s): {}".format(name, ", ".join(missing))
        )
    if unknown:
        raise _line_error(
            line_number, "{} has unknown field(s): {}".format(name, ", ".join(unknown))
        )


def _validate_payload(record: Mapping[str, Any], line_number: int) -> None:
    message_type = record["type"]
    payload = record["payload"]
    if not isinstance(payload, dict):
        raise _line_error(line_number, "payload must be an object")

    if message_type == "heartbeat":
        _require_exact_keys(
            payload, {"uptime_ms", "status_flags"}, set(), line_number, "payload"
        )
        _require_integer(
            payload["uptime_ms"], "payload.uptime_ms", line_number, 0, UINT32_MAX
        )
        _require_integer(
            payload["status_flags"], "payload.status_flags", line_number, 0, UINT16_MAX
        )
        return

    if message_type == "imu_sample":
        _require_exact_keys(
            payload, {"accel_mps2", "gyro_rad_s"}, set(), line_number, "payload"
        )
        for field in ("accel_mps2", "gyro_rad_s"):
            values = payload[field]
            if not isinstance(values, list) or len(values) != 3:
                raise _line_error(
                    line_number,
                    "payload.{} must contain three values".format(field),
                )
            for index, value in enumerate(values):
                _require_number_or_null(
                    value, "payload.{}[{}]".format(field, index), line_number
                )
        return

    if message_type == "motor_status":
        _require_exact_keys(
            payload,
            {"motor_id", "state", "rpm", "current_a", "temperature_c"},
            set(),
            line_number,
            "payload",
        )
        _require_integer(payload["motor_id"], "payload.motor_id", line_number, 0, 255)
        if not isinstance(payload["state"], str) or payload["state"] not in MOTOR_STATES:
            raise _line_error(
                line_number,
                "payload.state must be stopped, running, or fault",
            )
        _require_integer(
            payload["rpm"], "payload.rpm", line_number, INT32_MIN, INT32_MAX
        )
        _require_number_or_null(payload["current_a"], "payload.current_a", line_number)
        _require_number_or_null(
            payload["temperature_c"], "payload.temperature_c", line_number
        )
        return

    raise _line_error(line_number, "type must be heartbeat, imu_sample, or motor_status")


def _validate_message_shape(record: Mapping[str, Any], line_number: int) -> None:
    _require_exact_keys(
        record,
        MESSAGE_BASE_KEYS,
        MESSAGE_OPTIONAL_KEYS,
        line_number,
        "message record",
    )
    if record["kind"] != "message":
        raise _line_error(line_number, "kind must be message")
    if record["version"] != 1 or not _is_integer(record["version"]):
        raise _line_error(line_number, "version must be integer 1")
    _require_integer(record["sequence"], "sequence", line_number, 0, UINT32_MAX)
    if (
        not isinstance(record["sequence_status"], str)
        or record["sequence_status"] not in SEQUENCE_STATUSES
    ):
        raise _line_error(line_number, "invalid sequence_status")
    if "expected_sequence" in record:
        _require_integer(
            record["expected_sequence"],
            "expected_sequence",
            line_number,
            0,
            UINT32_MAX,
        )
    if "missing_before" in record:
        _require_integer(
            record["missing_before"],
            "missing_before",
            line_number,
            1,
            UINT32_MAX,
        )
    _validate_payload(record, line_number)


def _validate_sequence(messages: Sequence[Mapping[str, Any]]) -> None:
    expected: Optional[int] = None
    for index, record in enumerate(messages):
        line_number = index + 1
        sequence = record["sequence"]
        status = record["sequence_status"]
        if expected is None:
            if status != "first":
                raise _line_error(
                    line_number,
                    "the first message must have sequence_status first",
                )
            if "expected_sequence" in record or "missing_before" in record:
                raise _line_error(
                    line_number,
                    "the first message must not contain expected_sequence or missing_before",
                )
            expected = (sequence + 1) & UINT32_MAX
            continue

        if record.get("expected_sequence") != expected:
            raise _line_error(
                line_number,
                "expected_sequence must be {}".format(expected),
            )

        if sequence == expected:
            actual_status = "in_order"
            expected_missing: Optional[int] = None
            expected = (sequence + 1) & UINT32_MAX
        else:
            forward_distance = (sequence - expected) & UINT32_MAX
            if forward_distance < (1 << 31):
                actual_status = "gap"
                expected_missing = forward_distance
                expected = (sequence + 1) & UINT32_MAX
            else:
                actual_status = "out_of_order"
                expected_missing = None

        if status != actual_status:
            raise _line_error(
                line_number,
                "sequence_status must be {} for this sequence".format(actual_status),
            )
        if expected_missing is None:
            if "missing_before" in record:
                raise _line_error(
                    line_number,
                    "missing_before is only valid for a sequence gap",
                )
        elif record.get("missing_before") != expected_missing:
            raise _line_error(
                line_number,
                "missing_before must be {}".format(expected_missing),
            )


def _validate_health_shape(record: Mapping[str, Any], line_number: int) -> None:
    _require_exact_keys(record, HEALTH_KEYS, set(), line_number, "health record")
    if record["kind"] != "health":
        raise _line_error(line_number, "kind must be health")
    if not isinstance(record["status"], str) or record["status"] not in {
        "ok",
        "degraded",
    }:
        raise _line_error(line_number, "status must be ok or degraded")
    for field in HEALTH_COUNTER_KEYS:
        _require_integer(record[field], field, line_number, 0, UINT64_MAX)


def _validate_cross_record_contract(
    messages: Sequence[Mapping[str, Any]], health: Mapping[str, Any], health_line: int
) -> None:
    if health["frames_decoded"] != len(messages):
        raise _line_error(health_line, "frames_decoded does not match message records")

    type_counts = {key: 0 for key in TYPE_COUNT_KEYS}
    minimum_bytes = 0
    gaps = 0
    missing = 0
    out_of_order = 0
    for message in messages:
        message_type = message["type"]
        type_counts[message_type] += 1
        minimum_bytes += FRAME_WIRE_BYTES[message_type]
        if message["sequence_status"] == "gap":
            gaps += 1
            missing = min(UINT64_MAX, missing + message["missing_before"])
        elif message["sequence_status"] == "out_of_order":
            out_of_order += 1

    for message_type, health_field in TYPE_COUNT_KEYS.items():
        if health[health_field] != type_counts[message_type]:
            raise _line_error(
                health_line,
                "{} does not match {} message records".format(
                    health_field, message_type
                ),
            )
    if health["bytes_received"] < minimum_bytes:
        raise _line_error(
            health_line,
            "bytes_received is smaller than the decoded wire frames",
        )
    if health["sequence_gaps"] != gaps:
        raise _line_error(health_line, "sequence_gaps does not match message records")
    if health["missing_frames"] != missing:
        raise _line_error(health_line, "missing_frames does not match message records")
    if health["out_of_order_frames"] != out_of_order:
        raise _line_error(
            health_line,
            "out_of_order_frames does not match message records",
        )
    if (health["truncated_frames"] == 0) != (health["truncated_bytes"] == 0):
        raise _line_error(
            health_line,
            "truncated_frames and truncated_bytes must both be zero or both be nonzero",
        )

    expected_status = (
        "degraded" if any(health[field] != 0 for field in DEGRADING_COUNTERS) else "ok"
    )
    if health["status"] != expected_status:
        raise _line_error(
            health_line,
            "status must be {} for these diagnostic counters".format(expected_status),
        )


def parse_jsonl(data: bytes) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    """Parse and strictly validate one complete TelemetryLink CLI JSONL stream."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ReportError("input is not valid UTF-8: {}".format(error))
    if not text:
        raise ReportError("input is empty; one final health record is required")
    if not text.endswith("\n"):
        raise ReportError("JSONL input must end with a newline")

    messages: List[Dict[str, Any]] = []
    health: Optional[Dict[str, Any]] = None
    health_line = 0
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            raise _line_error(line_number, "blank lines are not allowed")
        try:
            record = json.loads(
                line,
                object_pairs_hook=_object_from_pairs,
                parse_constant=_reject_constant,
            )
        except (json.JSONDecodeError, ValueError) as error:
            raise _line_error(line_number, "invalid JSON: {}".format(error))
        if not isinstance(record, dict):
            raise _line_error(line_number, "each JSON Line must be an object")

        kind = record.get("kind")
        if health is not None:
            if kind == "health":
                raise _line_error(line_number, "multiple health records are not allowed")
            raise _line_error(
                line_number,
                "records after the final health record are not allowed",
            )
        if kind == "health":
            _validate_health_shape(record, line_number)
            health = record
            health_line = line_number
        elif kind == "message":
            _validate_message_shape(record, line_number)
            messages.append(record)
        else:
            raise _line_error(line_number, "kind must be message or health")

    if health is None:
        raise ReportError("input must end with exactly one health record")
    _validate_sequence(messages)
    _validate_cross_record_contract(messages, health, health_line)
    return messages, health


def _escape(value: Any) -> str:
    return html.escape(str(value), quote=True)


def _visible_console(value: Any) -> str:
    visible = []
    for character in str(value):
        codepoint = ord(character)
        if codepoint < 0x20 or codepoint == 0x7F:
            visible.append("\\x{:02x}".format(codepoint))
        else:
            visible.append(character)
    return "".join(visible)


def _format_integer(value: int) -> str:
    return format(value, ",d")


def _format_float(value: Any, digits: int = 3) -> str:
    if value is None:
        return "null"
    if value == 0:
        return "0"
    return "{:.{}g}".format(value, digits)


def _payload_summary(message: Mapping[str, Any]) -> str:
    payload = message["payload"]
    if message["type"] == "heartbeat":
        return "Uptime {} ms / flags 0x{:04x}".format(
            _format_integer(payload["uptime_ms"]), payload["status_flags"]
        )
    if message["type"] == "imu_sample":
        accel = ", ".join(_format_float(value) for value in payload["accel_mps2"])
        gyro = ", ".join(_format_float(value) for value in payload["gyro_rad_s"])
        return "Accel [{}] m/s² / gyro [{}] rad/s".format(accel, gyro)
    return "Motor {} / {} / {} rpm / {} A / {} °C".format(
        payload["motor_id"],
        payload["state"],
        _format_integer(payload["rpm"]),
        _format_float(payload["current_a"]),
        _format_float(payload["temperature_c"]),
    )


def _plural(value: int, singular: str, plural: Optional[str] = None) -> str:
    return "{} {}".format(
        _format_integer(value), singular if value == 1 else (plural or singular + "s")
    )


def _health_brief(health: Mapping[str, Any]) -> str:
    """Describe the stream using only evidence retained in the health record."""
    parts = [_plural(health["frames_decoded"], "frame")]
    if health["missing_frames"]:
        parts.append(_plural(health["missing_frames"], "missing frame"))
    if health["crc_failures"]:
        parts.append(_plural(health["crc_failures"], "CRC failure"))
    if health["noise_bytes"]:
        parts.append(_plural(health["noise_bytes"], "noise byte"))
    if health["out_of_order_frames"]:
        parts.append(_plural(health["out_of_order_frames"], "out-of-order frame"))

    other_events = sum(
        health[field]
        for field in (
            "invalid_versions",
            "unknown_types",
            "invalid_lengths",
            "payload_failures",
            "truncated_frames",
        )
    )
    if other_events:
        parts.append(_plural(other_events, "other recovery event"))
    if health["status"] == "ok":
        parts.append("no detected anomalies")
    return ", ".join(parts)


def _focus_index(messages: Sequence[Mapping[str, Any]]) -> Optional[int]:
    for index, message in enumerate(messages):
        if message["sequence_status"] in {"gap", "out_of_order"}:
            return index
    return 0 if messages else None


def _message_rows(
    messages: Sequence[Mapping[str, Any]],
    health: Mapping[str, Any],
    focus_index: Optional[int],
) -> str:
    rows = []
    for index, message in enumerate(messages):
        status = message["sequence_status"]
        status_label = {
            "first": "stream start",
            "in_order": "in order",
            "gap": "gap",
            "out_of_order": "out of order",
        }[status]
        note = ""
        if status == "gap":
            note = "{} missing".format(_format_integer(message["missing_before"]))
        elif status == "out_of_order":
            note = "expected {}".format(_format_integer(message["expected_sequence"]))

        focused = index == focus_index
        row_classes = "record-row record-row--{}{}".format(
            status.replace("_", "-"), " record-row--focused" if focused else ""
        )
        focus_text = (
            '<span class="visually-hidden">Focused record. </span>' if focused else ""
        )
        rows.append(
            """<tr class="{row_classes}" data-state="{status}"{focused_attr}>
              <th scope="row" class="number">{sequence}</th>
              <td><code>{type_label}</code></td>
              <td class="continuity"><span class="state-mark" aria-hidden="true"></span>{focus_text}<span>{status_label}</span>{note}</td>
              <td class="payload">{payload}</td>
            </tr>""".format(
                row_classes=row_classes,
                status=_escape(status),
                focused_attr=' data-focused="true"' if focused else "",
                sequence=_format_integer(message["sequence"]),
                type_label=_escape(message["type"]),
                focus_text=focus_text,
                status_label=_escape(status_label),
                note=("<small>{}</small>".format(_escape(note)) if note else ""),
                payload=_escape(_payload_summary(message)),
            )
        )

    health_focused = focus_index is None
    rows.append(
        """<tr class="record-row record-row--health record-row--{status}{focused}" data-state="{status}"{focused_attr}>
          <th scope="row" class="number">EOF</th>
          <td><code>health</code></td>
          <td class="continuity"><span class="state-mark" aria-hidden="true"></span>{focus_text}<span>{status}</span></td>
          <td class="payload">{summary}</td>
        </tr>""".format(
            status=_escape(health["status"]),
            focused=" record-row--focused" if health_focused else "",
            focused_attr=' data-focused="true"' if health_focused else "",
            focus_text=(
                '<span class="visually-hidden">Focused record. </span>'
                if health_focused
                else ""
            ),
            summary=_escape(_health_brief(health)),
        )
    )
    if not messages:
        rows.insert(
            0,
            '<tr class="empty-row"><td colspan="4"><strong>No decoded messages.</strong> '
            "No sequence baseline was established.</td></tr>",
        )
    return "\n".join(rows)


def _stream_strip(
    messages: Sequence[Mapping[str, Any]], health: Mapping[str, Any]
) -> str:
    """Draw every record as one block, like a trace on a logic analyser."""
    blocks = [
        '<li data-state="{}" title="sequence {}: {}"></li>'.format(
            _escape(message["sequence_status"]),
            _escape(_format_integer(message["sequence"])),
            _escape(message["sequence_status"].replace("_", " ")),
        )
        for message in messages
    ]
    blocks.append(
        '<li class="strip-health" data-state="{0}" title="health: {0}"></li>'.format(
            _escape(health["status"])
        )
    )
    return """<figure class="stream-strip" aria-labelledby="strip-caption">
      <ol aria-hidden="true">{}</ol>
      <figcaption id="strip-caption"><span class="fig-label">Figure 1.</span> One block per record. Hatched: frames missing before it.</figcaption>
    </figure>""".format("".join(blocks))


def _diagnostic_rows(health: Mapping[str, Any], fields: Sequence[str]) -> str:
    rows = [
        '<tr class="counter-row counter-row--alert"><th scope="row"><code>{}</code></th>'
        '<td class="number">{}</td></tr>'.format(
            _escape(field), _format_integer(health[field])
        )
        for field in fields
        if health[field]
    ]
    if not rows:
        return '<tr class="counter-row counter-row--clear"><td colspan="2">All zero.</td></tr>'
    return "\n".join(rows)


def _json_value(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(", ", ": "))


def _focus_detail(
    messages: Sequence[Mapping[str, Any]],
    health: Mapping[str, Any],
    focus_index: Optional[int],
) -> str:
    if focus_index is None:
        record = health
        line_number = 1
        heading = "EOF / health"
        reason = "Nothing decoded."
    else:
        record = messages[focus_index]
        line_number = focus_index + 1
        heading = "sequence {} / {}".format(record["sequence"], record["type"])
        if record["sequence_status"] in {"gap", "out_of_order"}:
            reason = "First problem:"
        else:
            reason = "First record:"

    raw = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
    return """<details class="record-detail" id="record-detail">
        <summary>{reason} <span>{heading}</span></summary>
        <pre tabindex="0" role="region" aria-label="Raw JSON for the focused record; scrollable"><code>{raw}</code></pre>
      </details>""".format(
        heading=_escape(heading),
        reason=_escape(reason),
        raw=_escape(raw),
    )


def render_html(
    messages: Sequence[Mapping[str, Any]], health: Mapping[str, Any]
) -> str:
    """Return an HTML reader for CLI JSONL."""
    health_status = health["status"]
    focus_index = _focus_index(messages)

    document = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <meta name="theme-color" content="#ffffff">
  <title>TelemetryLink stream report</title>
  <style>
    /* Laid out like a component datasheet: a part-number rule at the top,
       numbered sections, and a trace strip of every record in the stream. */
    :root {{
      color-scheme: light;
      --sans: "Helvetica Neue", Helvetica, Arial, sans-serif;
      --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      --ink: #111111;
      --muted: #555555;
      --rule: #111111;
      --rule-soft: #d4d4d4;
      --shade: #ececec;
      --surface: #ffffff;
      --ok: #1f7a3a;
      --warn: #b86e00;
      --warn-bg: #fff3dc;
      --danger: #c21f2c;
      --danger-bg: #fde7e8;
      --focus: #0057b8;
    }}
    * {{ box-sizing: border-box; }}
    html {{ min-width: 320px; background: #e6e6e6; }}
    body {{
      margin: 0;
      min-width: 320px;
      padding: 32px 16px;
      background: #e6e6e6;
      color: var(--ink);
      font: 14px/1.45 var(--sans);
    }}
    .skip-link {{
      position: fixed; z-index: 20; top: 6px; left: 6px; padding: 7px 9px;
      color: var(--surface); background: var(--ink);
      transform: translateY(-150%);
    }}
    .skip-link:focus {{ transform: translateY(0); }}
    .sheet {{
      width: min(1180px, 100%);
      margin: 0 auto;
      padding: 36px 44px 48px;
      background: var(--surface);
      box-shadow: 0 1px 3px rgb(0 0 0 / 18%);
    }}
    .reader-header {{
      display: flex;
      flex-wrap: wrap;
      align-items: flex-end;
      justify-content: space-between;
      gap: 8px 24px;
      padding-bottom: 8px;
      border-bottom: 3px solid var(--rule);
    }}
    .reader-title h1 {{ margin: 0; font: 700 30px/1 var(--sans); letter-spacing: -0.01em; }}
    .reader-title span {{ display: block; margin-top: 6px; color: var(--muted); font-size: 14px; }}
    .health-line {{ display: flex; flex-wrap: wrap; align-items: baseline; gap: 6px; margin: 0; font-size: 14px; }}
    .health-line strong {{ text-transform: capitalize; }}
    .health-line--ok strong {{ color: var(--ok); }}
    .health-line--degraded strong {{ color: var(--danger); }}
    .health-line > span:last-child {{ color: var(--muted); }}
    .health-line .state-mark {{ display: none; }}
    .state-mark::before {{
      content: "";
      display: inline-block;
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: currentColor;
    }}
    .reader:focus {{ outline: none; }}
    h2, h3, p {{ margin-top: 0; }}
    h2 {{ margin: 32px 0 4px; font: 700 18px/1.2 var(--sans); }}
    h2 .section-number {{ display: inline-block; min-width: 1.8em; }}
    .section-note {{ margin: 0 0 10px; color: var(--muted); font-size: 13px; }}
    code, pre, .number, .line-number {{ font-family: var(--mono); }}

    /* Figure 1: one block per record, in output order */
    .stream-strip {{ margin: 22px 0 0; }}
    .stream-strip ol {{
      display: flex;
      flex-wrap: wrap;
      gap: 3px;
      margin: 0;
      padding: 10px 0;
      border-block: 1px solid var(--rule-soft);
      list-style: none;
    }}
    .stream-strip li {{
      width: 22px;
      height: 30px;
      border: 1px solid var(--ink);
      background: var(--surface);
    }}
    .stream-strip li[data-state="gap"] {{ border-color: var(--warn); background: repeating-linear-gradient(135deg, var(--warn) 0 2px, var(--warn-bg) 2px 6px); }}
    .stream-strip li[data-state="out_of_order"] {{ border-color: var(--danger); background: var(--danger-bg); }}
    .stream-strip li[data-state="first"] {{ background: var(--ink); }}
    .stream-strip li.strip-health {{ width: 30px; margin-left: 10px; border-width: 2px; }}
    .stream-strip li.strip-health[data-state="degraded"] {{ border-color: var(--danger); background: var(--danger); }}
    .stream-strip li.strip-health[data-state="ok"] {{ border-color: var(--ok); background: var(--ok); }}
    .stream-strip figcaption {{ margin-top: 6px; color: var(--muted); font-size: 13px; }}
    .fig-label {{ color: var(--ink); font-weight: 700; }}

    .workspace {{
      display: grid;
      grid-template-columns: minmax(0, 1fr);
      gap: 12px;
    }}
    .table-scroll {{ overflow: auto; overscroll-behavior-inline: contain; }}
    .table-scroll:focus, .counter-scroll:focus, pre:focus {{
      outline: 2px solid var(--focus);
      outline-offset: 2px;
    }}
    table {{ width: 100%; border-collapse: collapse; font-size: 13px; }}
    caption {{
      padding-bottom: 6px;
      color: var(--muted);
      font-size: 13px;
      text-align: left;
    }}
    .visually-hidden {{
      position: absolute;
      width: 1px;
      height: 1px;
      padding: 0;
      margin: -1px;
      overflow: hidden;
      clip: rect(0, 0, 0, 0);
      white-space: nowrap;
      border: 0;
    }}
    th, td {{ padding: 6px 10px 6px 0; border-bottom: 1px solid var(--rule-soft); text-align: left; vertical-align: top; }}
    thead th {{
      padding-top: 7px;
      border-top: 2px solid var(--rule);
      border-bottom: 1px solid var(--rule);
      background: var(--shade);
      font-weight: 700;
      white-space: nowrap;
    }}
    thead th:first-child {{ padding-left: 8px; }}
    tbody td:first-child {{ padding-left: 8px; }}
    .records-table {{ min-width: 640px; table-layout: fixed; }}
    .records-table th:nth-child(1) {{ width: 90px; }}
    .records-table th:nth-child(2) {{ width: 130px; }}
    .records-table th:nth-child(3) {{ width: 170px; }}
    .record-row:last-child > * {{ border-bottom: 2px solid var(--rule); }}
    .record-row--health > * {{ border-top: 1px solid var(--rule); }}
    .empty-row td {{ padding: 15px 10px; color: var(--muted); }}
    .line-number {{ color: var(--muted); }}
    .number {{ font-variant-numeric: tabular-nums; white-space: nowrap; }}
    .continuity {{ white-space: nowrap; }}
    .continuity .state-mark {{ display: inline-block; width: 15px; }}
    .continuity small {{ margin-left: 6px; color: var(--muted); font-size: 12px; white-space: normal; }}
    [data-state="first"] .state-mark, [data-state="in_order"] .state-mark {{ visibility: hidden; }}
    [data-state="first"] .continuity, [data-state="in_order"] .continuity {{ color: var(--muted); }}
    [data-state="ok"] .state-mark {{ color: var(--ok); }}
    [data-state="gap"] .state-mark {{ color: var(--warn); }}
    [data-state="out_of_order"] .state-mark, [data-state="degraded"] .state-mark {{ color: var(--danger); }}
    .payload {{ overflow-wrap: anywhere; font-family: var(--mono); font-size: 12px; }}

    .record-detail summary {{ cursor: pointer; font-weight: 700; }}
    .record-detail summary span {{ font-weight: 400; color: var(--muted); }}
    pre {{
      max-height: 200px;
      margin: 8px 0 0;
      padding: 10px 12px;
      overflow: auto;
      border: 1px solid var(--rule-soft);
      background: #f7f7f7;
      font-size: 12px;
      line-height: 1.5;
      white-space: pre-wrap;
      overflow-wrap: anywhere;
    }}
    .counter-scroll {{ overflow: auto; overscroll-behavior-inline: contain; }}
    .counter-table {{ max-width: 420px; }}
    .counter-table th code {{ overflow-wrap: anywhere; }}
    .counter-table tbody th {{ font-weight: 400; }}
    .counter-table .number {{ width: 80px; text-align: right; }}
    .counter-row--clear .state-mark {{ color: var(--ok); }}
    .counter-row--alert .state-mark {{ color: var(--danger); }}
    .counter-table tbody tr:last-child > * {{ border-bottom: 2px solid var(--rule); }}
    @media (max-width: 900px) {{
      .workspace {{ grid-template-columns: minmax(0, 1fr); }}
    }}
    @media (max-width: 600px) {{
      body {{ padding: 0; }}
      .sheet {{ padding: 20px 14px 32px; box-shadow: none; }}
      .counter-table .number {{ width: 48px; }}
    }}
    @media print {{
      .skip-link {{ display: none; }}
      html, body {{ padding: 0; background: #fff; }}
      body {{ font-size: 9pt; }}
      .sheet {{ width: 100%; padding: 0; box-shadow: none; }}
      .workspace {{ display: block; }}
      .table-scroll, .counter-scroll, pre {{ max-height: none; overflow: visible; }}
      .records-table {{ min-width: 0; table-layout: auto; }}
      .counter-table {{ min-width: 0; }}
      .counters-panel {{ break-before: page; }}
      thead {{ display: table-header-group; }}
    }}
  </style>
</head>
<body>
  <a class="skip-link" href="#report">Skip to stream</a>
  <div class="sheet">
  <header class="reader-header">
    <div class="reader-title"><h1>TelemetryLink</h1><span>Stream report</span></div>
    <p class="health-line health-line--{health_status}" role="status">
      <span class="state-mark" aria-hidden="true"></span><strong>{health_status}</strong><span>{health_brief}</span>
    </p>
  </header>
  <main class="reader" id="report" tabindex="-1">
    {stream_strip}
    <section aria-labelledby="stream-title">
      <h2 id="stream-title"><span class="section-number">1</span>Decoded stream</h2>
      <div class="workspace">
        <div class="table-scroll" tabindex="0" role="region" aria-label="Chronological JSONL record table; horizontally scrollable">
        <table class="records-table">
          <caption><span class="fig-label">Table 1.</span> Records in output order, ending with the health record.</caption>
          <thead><tr><th scope="col">Sequence</th><th scope="col">Record</th><th scope="col">State</th><th scope="col">Decoded value</th></tr></thead>
          <tbody>{message_rows}</tbody>
        </table>
        </div>
        {focus_detail}
      </div>
    </section>

    <section class="counters-panel" aria-labelledby="counters-title">
      <h2 id="counters-title"><span class="section-number">2</span>Parser and sequence counters</h2>
      <div class="counter-scroll" tabindex="0" role="region" aria-label="End-of-stream parser recovery and sequence counters; horizontally scrollable">
        <table class="counter-table">
          <caption><span class="fig-label">Table 2.</span> Non-zero counters from the health record. They overlap.</caption>
          <thead><tr><th scope="col">Counter</th><th scope="col">Count</th></tr></thead>
          <tbody>{diagnostic_rows}</tbody>
        </table>
      </div>
    </section>
  </main>
  </div>
</body>
</html>
""".format(
        health_status=_escape(health_status),
        health_brief=_escape(_health_brief(health)),
        diagnostic_rows=_diagnostic_rows(health, REPORT_DIAGNOSTIC_FIELDS),
        message_rows=_message_rows(messages, health, focus_index),
        focus_detail=_focus_detail(messages, health, focus_index),
        stream_strip=_stream_strip(messages, health),
    )
    return document


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render TelemetryLink CLI JSONL as an HTML report."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="-",
        metavar="INPUT",
        help="JSONL input path, or - for standard input (default: -)",
    )
    parser.add_argument(
        "positional_output",
        nargs="?",
        metavar="OUTPUT",
        help="HTML output path, or - for standard output",
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="option_output",
        metavar="OUTPUT",
        help="HTML output path, or - for standard output (default: -)",
    )
    args = parser.parse_args(argv)
    if args.positional_output is not None and args.option_output is not None:
        parser.error("OUTPUT may be positional or passed with --output, not both")
    args.output = args.option_output or args.positional_output or "-"
    return args


def _read_input(path: str) -> bytes:
    if path == "-":
        return sys.stdin.buffer.read()
    return Path(path).read_bytes()


def _write_output(path: str, data: bytes) -> None:
    if path == "-":
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        return
    Path(path).write_bytes(data)


def main(argv: Sequence[str] = ()) -> int:
    args = _parse_args(argv)
    try:
        source = _read_input(args.input)
    except OSError as error:
        print(
            "render_report.py: cannot read input: {}".format(_visible_console(error)),
            file=sys.stderr,
        )
        return 1

    try:
        messages, health = parse_jsonl(source)
    except ReportError as error:
        print(
            "render_report.py: invalid TelemetryLink JSONL: {}".format(
                _visible_console(error)
            ),
            file=sys.stderr,
        )
        return 2

    output = (render_html(messages, health) + "\n").encode("utf-8")
    try:
        _write_output(args.output, output)
    except BrokenPipeError:
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
        print(
            "render_report.py: cannot write output: {}".format(_visible_console(error)),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
