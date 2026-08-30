# TelemetryLink wire protocol

TelemetryLink v1 carries independent telemetry messages in a byte stream. A
receiver may be given arbitrary chunks: a chunk need not contain a complete
frame and may contain several frames.

## Frame layout

All multibyte integers, IEEE-754 binary32 values, and the CRC field are encoded
little-endian. Offsets are measured from the first magic byte.

| Offset | Size | Field | Encoding |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | ASCII `TLNK` (`54 4c 4e 4b`) |
| 4 | 1 | Version | Unsigned integer; v1 is `01` |
| 5 | 1 | Message type | Unsigned integer; see below |
| 6 | 2 | Payload length | Unsigned little-endian byte count, `N` |
| 8 | 4 | Sequence number | Unsigned little-endian global counter |
| 12 | `N` | Payload | Message-type-specific bytes |
| `12 + N` | 4 | CRC32 | Unsigned little-endian checksum |

The fixed header is 12 bytes. The total frame size is `16 + N` bytes, so frame
overhead is 16 bytes. The length field can represent 65,535 bytes, but v1
receivers impose an absolute defensive maximum of 1,024 bytes before retaining
a candidate. A receiver may configure a lower ceiling. The three v1 messages
have still smaller exact lengths, so the largest accepted v1 frame is currently
the 40-byte IMU frame.

## CRC

The checksum is CRC-32/ISO-HDLC (also commonly called IEEE CRC-32):

- width: 32 bits
- polynomial: `0x04c11db7` (`0xedb88320` in reflected implementations)
- initial value: `0xffffffff`
- input and output reflected
- final XOR: `0xffffffff`
- check value for ASCII `123456789`: `0xcbf43926`

The checked region starts at the version byte (offset 4), includes the type,
length, sequence number, and every payload byte, and ends immediately before
the CRC. The magic bytes and CRC field are not included. Python's
`zlib.crc32(checked_region) & 0xffffffff` produces the v1 value. The resulting
32-bit value is placed on the wire least-significant byte first.

## Message payloads

The notation in the table uses Python `struct` format strings; the leading `<`
means little-endian with standard sizes and no alignment padding.

| Type | Name | Payload format | Bytes |
| ---: | --- | --- | ---: |
| `0x01` | Heartbeat | `<IH` | 6 |
| `0x02` | IMU sample | `<6f` | 24 |
| `0x03` | Motor status | `<BBiff` | 14 |

Known message types must have exactly the documented payload length. An
otherwise well-formed frame with a different length is invalid.

### Heartbeat (`0x01`)

| Payload offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `uint32` | Device uptime in milliseconds |
| 4 | `uint16` | Status flags bitmask |

Status-flag bit assignments are application-specific in v1; TelemetryLink
preserves the complete value.

### IMU sample (`0x02`)

The six fields are IEEE-754 binary32 values in this order:

1. acceleration X, metres per second squared
2. acceleration Y, metres per second squared
3. acceleration Z, metres per second squared
4. angular velocity X, radians per second
5. angular velocity Y, radians per second
6. angular velocity Z, radians per second

The implementation requires 32-bit IEC 60559/IEEE-754 `float`. The wire format
does not itself prohibit infinities or NaNs; applications that require finite
measurements must enforce that policy when consuming decoded values.

### Motor status (`0x03`)

| Payload offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `uint8` | Motor identifier |
| 1 | `uint8` | State: `0` stopped, `1` running, `2` fault |
| 2 | `int32` | Signed revolutions per minute |
| 6 | `float32` | Current in amperes |
| 10 | `float32` | Temperature in degrees Celsius |

Other state values are invalid payloads.

## Stream parsing and recovery

The receiver parses incrementally and applies these rules:

1. Search for `TLNK`. Bytes before it are noise. If a chunk ends with a prefix
   of the magic (`T`, `TL`, or `TLN`), retain that prefix for the next call.
2. Once a candidate has 12 header bytes, validate the version, configured
   defensive limit (never above 1,024 bytes), known type, and exact length for
   that type.
3. If the header is plausible but incomplete, retain it until the advertised
   payload and CRC arrive.
4. Verify CRC before decoding the payload. A CRC-valid payload with an invalid
   semantic value (for example, motor state 9) is consumed as one bad frame.
5. On an invalid header or failed CRC, advance only one byte past the start of
   the candidate and search for magic again. Do not skip the advertised frame
   length: that length may be the corrupted field, and skipping it could discard
   the next valid frame.
6. On success, consume exactly `16 + N` bytes and continue immediately, which
   permits multiple frames in one input chunk.

Magic may legally occur inside a payload. CRC and the complete candidate length
disambiguate it. A false magic followed by a syntactically plausible length is
indistinguishable from a partially received real frame, so recovery can be
delayed until that candidate's advertised bytes arrive. The defensive length
limit bounds this latency and retained-candidate cost; exact v1 schema lengths
currently provide a tighter 40-byte bound. Eagerly jumping to a later magic
would instead risk dropping a valid frame whose payload contains `TLNK`.

The candidate limit does not bound the size of a chunk supplied to a parser or
the number of decoded frames returned from one call. Implementations may copy
that chunk while parsing it. Applications requiring a fixed peak-memory budget
must therefore provide bounded chunks in addition to configuring the payload
ceiling.

At end of input, `finish()` classifies any retained bytes (including a partial
magic or partial frame) as one truncated fragment, records its byte count, and
clears them. Truncation is not reported as a CRC failure. `finish()` preserves
sequence history; `reset()` clears buffered bytes, sequence history, and all
statistics for a genuinely new stream. Statistics are unsigned 64-bit counters
and saturate at `2^64 - 1` instead of wrapping.

## Sequence semantics

The sender uses one sequence counter across all message types and increments it
once for each transmitted frame modulo `2^32`. Only successfully decoded frames
participate in receiver-side ordering.

The first decoded frame establishes a baseline. Thereafter, let
`expected = last_accepted + 1` and calculate `distance = sequence - expected`
using unsigned 32-bit arithmetic:

- `distance == 0`: in order; advance the baseline.
- `0 < distance < 0x80000000`: ahead; report a gap containing exactly
  `distance` missing frames, then advance the baseline.
- `distance >= 0x80000000`: out of order (including a duplicate or the exactly
  half-range ambiguous case); report it but do not move the baseline.

Thus `0xffffffff` followed by `0x00000000` is in order. The comparison assumes
fewer than `2^31` sequence positions separate meaningful observations, as is
standard for modular serial-number arithmetic. A device reboot that resets its
counter cannot be distinguished from old/out-of-order traffic; callers must
start a new parser session with `reset()` when a stream reset is known.
