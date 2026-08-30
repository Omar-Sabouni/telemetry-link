#include "telemetry_link/telemetry_link.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using telemetry_link::DecodedFrame;
using telemetry_link::Heartbeat;
using telemetry_link::ImuSample;
using telemetry_link::MotorState;
using telemetry_link::MotorStatus;
using telemetry_link::Payload;
using telemetry_link::SequenceStatus;
using telemetry_link::StreamParser;

using Bytes = std::vector<std::uint8_t>;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define TL_CHECK(condition)                                                          \
    do {                                                                             \
        if (!(condition)) {                                                           \
            throw TestFailure(                                                       \
                std::string{"check failed: "} + #condition +                        \
                " (line " + std::to_string(__LINE__) + ")"                         \
            );                                                                       \
        }                                                                            \
    } while (false)

[[nodiscard]] std::span<const std::uint8_t> as_span(const Bytes& bytes) {
    return {bytes.data(), bytes.size()};
}

void append(Bytes& destination, const Bytes& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

void append_frames(std::vector<DecodedFrame>& destination, std::vector<DecodedFrame> source) {
    destination.insert(
        destination.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end())
    );
}

void store_u16_le(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    TL_CHECK(offset + 2U <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void store_u32_le(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    TL_CHECK(offset + 4U <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void rewrite_crc(Bytes& frame) {
    TL_CHECK(frame.size() >= telemetry_link::kMagic.size() + telemetry_link::kCrcSize);
    const auto checked_size = frame.size() - telemetry_link::kMagic.size() -
        telemetry_link::kCrcSize;
    const auto checksum = telemetry_link::crc32(
        as_span(frame).subspan(telemetry_link::kMagic.size(), checked_size)
    );
    store_u32_le(frame, frame.size() - telemetry_link::kCrcSize, checksum);
}

[[nodiscard]] bool same_float(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

[[nodiscard]] Heartbeat sample_heartbeat() {
    return Heartbeat{.uptime_ms = 0x11223344U, .status_flags = 0x5566U};
}

[[nodiscard]] ImuSample sample_imu() {
    return ImuSample{
        .accel_x_mps2 = 1.5F,
        .accel_y_mps2 = -2.25F,
        .accel_z_mps2 = 3.75F,
        .gyro_x_rad_s = -0.125F,
        .gyro_y_rad_s = 1.0F,
        .gyro_z_rad_s = -2.0F,
    };
}

[[nodiscard]] MotorStatus sample_motor() {
    return MotorStatus{
        .motor_id = 7U,
        .state = MotorState::running,
        .rpm = -12345,
        .current_a = 12.5F,
        .temperature_c = -4.25F,
    };
}

[[nodiscard]] Bytes heartbeat_frame(std::uint32_t sequence) {
    return telemetry_link::encode_frame(sequence, Payload{sample_heartbeat()});
}

[[nodiscard]] Bytes seeded_noise(std::size_t byte_count, std::mt19937& generator) {
    Bytes noise;
    noise.reserve(byte_count);
    for (std::size_t index = 0U; index < byte_count; ++index) {
        auto byte = static_cast<std::uint8_t>(generator() & 0xffU);
        // Prevent an accidental full magic marker so this test has stable counters.
        if (byte == telemetry_link::kMagic.front()) {
            byte ^= 0x01U;
        }
        noise.push_back(byte);
    }
    return noise;
}

void check_stats_are_zero(const telemetry_link::ParserStats& stats) {
    TL_CHECK(stats.bytes_received == 0U);
    TL_CHECK(stats.frames_decoded == 0U);
    TL_CHECK(stats.noise_bytes == 0U);
    TL_CHECK(stats.invalid_versions == 0U);
    TL_CHECK(stats.unknown_types == 0U);
    TL_CHECK(stats.invalid_lengths == 0U);
    TL_CHECK(stats.crc_failures == 0U);
    TL_CHECK(stats.payload_failures == 0U);
    TL_CHECK(stats.truncated_frames == 0U);
    TL_CHECK(stats.truncated_bytes == 0U);
    TL_CHECK(stats.sequence_gaps == 0U);
    TL_CHECK(stats.missing_frames == 0U);
    TL_CHECK(stats.out_of_order_frames == 0U);
}

void check_heartbeat(const Payload& payload, const Heartbeat& expected) {
    const auto* actual = std::get_if<Heartbeat>(&payload);
    TL_CHECK(actual != nullptr);
    TL_CHECK(actual->uptime_ms == expected.uptime_ms);
    TL_CHECK(actual->status_flags == expected.status_flags);
}

void check_imu(const Payload& payload, const ImuSample& expected) {
    const auto* actual = std::get_if<ImuSample>(&payload);
    TL_CHECK(actual != nullptr);
    TL_CHECK(same_float(actual->accel_x_mps2, expected.accel_x_mps2));
    TL_CHECK(same_float(actual->accel_y_mps2, expected.accel_y_mps2));
    TL_CHECK(same_float(actual->accel_z_mps2, expected.accel_z_mps2));
    TL_CHECK(same_float(actual->gyro_x_rad_s, expected.gyro_x_rad_s));
    TL_CHECK(same_float(actual->gyro_y_rad_s, expected.gyro_y_rad_s));
    TL_CHECK(same_float(actual->gyro_z_rad_s, expected.gyro_z_rad_s));
}

void check_motor(const Payload& payload, const MotorStatus& expected) {
    const auto* actual = std::get_if<MotorStatus>(&payload);
    TL_CHECK(actual != nullptr);
    TL_CHECK(actual->motor_id == expected.motor_id);
    TL_CHECK(actual->state == expected.state);
    TL_CHECK(actual->rpm == expected.rpm);
    TL_CHECK(same_float(actual->current_a, expected.current_a));
    TL_CHECK(same_float(actual->temperature_c, expected.temperature_c));
}

[[nodiscard]] std::vector<DecodedFrame> parse_sequences(
    const std::vector<std::uint32_t>& sequences,
    StreamParser& parser
) {
    Bytes stream;
    for (const auto sequence : sequences) {
        append(stream, heartbeat_frame(sequence));
    }
    return parser.feed(as_span(stream));
}

void test_crc32_known_vector() {
    constexpr std::array<std::uint8_t, 9> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    TL_CHECK(telemetry_link::crc32(input) == 0xcbf43926U);
}

void test_public_name_helpers() {
    TL_CHECK(
        telemetry_link::message_type_name(telemetry_link::MessageType::heartbeat) ==
        "heartbeat"
    );
    TL_CHECK(
        telemetry_link::message_type_name(telemetry_link::MessageType::imu_sample) ==
        "imu_sample"
    );
    TL_CHECK(
        telemetry_link::message_type_name(telemetry_link::MessageType::motor_status) ==
        "motor_status"
    );
    TL_CHECK(
        telemetry_link::message_type_name(
            static_cast<telemetry_link::MessageType>(0xffU)
        ) == "unknown"
    );

    TL_CHECK(telemetry_link::motor_state_name(MotorState::stopped) == "stopped");
    TL_CHECK(telemetry_link::motor_state_name(MotorState::running) == "running");
    TL_CHECK(telemetry_link::motor_state_name(MotorState::fault) == "fault");
    TL_CHECK(
        telemetry_link::motor_state_name(static_cast<MotorState>(0xffU)) == "unknown"
    );

    TL_CHECK(telemetry_link::sequence_status_name(SequenceStatus::first) == "first");
    TL_CHECK(
        telemetry_link::sequence_status_name(SequenceStatus::in_order) == "in_order"
    );
    TL_CHECK(telemetry_link::sequence_status_name(SequenceStatus::gap) == "gap");
    TL_CHECK(
        telemetry_link::sequence_status_name(SequenceStatus::out_of_order) ==
        "out_of_order"
    );
    TL_CHECK(
        telemetry_link::sequence_status_name(static_cast<SequenceStatus>(0xffU)) ==
        "unknown"
    );
}

void test_fixed_golden_heartbeat_bytes() {
    const Bytes expected{
        0x54U, 0x4cU, 0x4eU, 0x4bU,
        0x01U,
        0x01U,
        0x06U, 0x00U,
        0x04U, 0x03U, 0x02U, 0x01U,
        0x44U, 0x33U, 0x22U, 0x11U, 0x66U, 0x55U,
        0x26U, 0x39U, 0x1aU, 0xe7U,
    };

    const auto encoded = telemetry_link::encode_frame(
        0x01020304U,
        Payload{sample_heartbeat()}
    );
    TL_CHECK(encoded == expected);
}

void test_all_message_round_trips() {
    const auto heartbeat = sample_heartbeat();
    const auto imu = sample_imu();
    const auto motor = sample_motor();

    Bytes stream;
    append(stream, telemetry_link::encode_frame(100U, Payload{heartbeat}));
    append(stream, telemetry_link::encode_frame(101U, Payload{imu}));
    append(stream, telemetry_link::encode_frame(102U, Payload{motor}));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 3U);
    TL_CHECK(decoded[0].message.sequence == 100U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    check_heartbeat(decoded[0].message.payload, heartbeat);
    TL_CHECK(decoded[1].message.sequence == 101U);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::in_order);
    check_imu(decoded[1].message.payload, imu);
    TL_CHECK(decoded[2].message.sequence == 102U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::in_order);
    check_motor(decoded[2].message.payload, motor);
    TL_CHECK(parser.stats().frames_decoded == 3U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_valid_payload_may_contain_magic() {
    const Heartbeat heartbeat{
        .uptime_ms = 0x4b4e4c54U, // Little-endian payload bytes are "TLNK".
        .status_flags = 0xa55aU,
    };
    const auto frame = telemetry_link::encode_frame(103U, Payload{heartbeat});

    TL_CHECK(std::equal(
        telemetry_link::kMagic.begin(),
        telemetry_link::kMagic.end(),
        frame.begin() + static_cast<std::ptrdiff_t>(telemetry_link::kFixedHeaderSize)
    ));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(frame));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 103U);
    check_heartbeat(decoded.front().message.payload, heartbeat);
    TL_CHECK(parser.stats().noise_bytes == 0U);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_encoder_rejects_invalid_motor_state() {
    auto motor = sample_motor();
    motor.state = static_cast<MotorState>(0xffU);

    bool rejected = false;
    try {
        static_cast<void>(telemetry_link::encode_frame(104U, Payload{motor}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    TL_CHECK(rejected);
}

void test_parser_payload_limit_boundaries() {
    StreamParser default_limit{telemetry_link::kMaxPayloadSize};
    TL_CHECK(default_limit.feed(as_span(heartbeat_frame(105U))).size() == 1U);

    bool rejected = false;
    try {
        StreamParser too_large{telemetry_link::kMaxPayloadSize + 1U};
        static_cast<void>(too_large);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    TL_CHECK(rejected);

    StreamParser exact_heartbeat_limit{6U};
    TL_CHECK(exact_heartbeat_limit.feed(as_span(heartbeat_frame(106U))).size() == 1U);

    StreamParser below_heartbeat_limit{5U};
    TL_CHECK(below_heartbeat_limit.feed(as_span(heartbeat_frame(107U))).empty());
    TL_CHECK(below_heartbeat_limit.stats().invalid_lengths == 1U);
    TL_CHECK(below_heartbeat_limit.pending_bytes() == 0U);
}

void test_every_split_point() {
    const auto imu = sample_imu();
    const auto frame = telemetry_link::encode_frame(321U, Payload{imu});
    const auto bytes = as_span(frame);

    for (std::size_t split = 0U; split <= frame.size(); ++split) {
        StreamParser parser;
        std::vector<DecodedFrame> decoded;
        append_frames(decoded, parser.feed(bytes.first(split)));
        append_frames(decoded, parser.feed(bytes.subspan(split)));

        TL_CHECK(decoded.size() == 1U);
        TL_CHECK(decoded.front().message.sequence == 321U);
        TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
        check_imu(decoded.front().message.payload, imu);
        TL_CHECK(parser.stats().bytes_received == frame.size());
        TL_CHECK(parser.stats().frames_decoded == 1U);
        TL_CHECK(parser.pending_bytes() == 0U);
    }
}

void test_byte_at_a_time() {
    const auto motor = sample_motor();
    const auto frame = telemetry_link::encode_frame(654U, Payload{motor});
    std::vector<DecodedFrame> decoded;
    StreamParser parser;

    for (std::size_t index = 0U; index < frame.size(); ++index) {
        const std::span<const std::uint8_t> one_byte{frame.data() + index, 1U};
        auto next = parser.feed(one_byte);
        if (index + 1U < frame.size()) {
            TL_CHECK(next.empty());
        }
        append_frames(decoded, std::move(next));
    }

    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 654U);
    check_motor(decoded.front().message.payload, motor);
    TL_CHECK(parser.stats().bytes_received == frame.size());
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_multiple_frames_in_one_chunk() {
    Bytes stream;
    append(stream, heartbeat_frame(7U));
    append(stream, heartbeat_frame(8U));
    append(stream, heartbeat_frame(9U));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 3U);
    TL_CHECK(decoded[0].message.sequence == 7U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].message.sequence == 8U);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::in_order);
    TL_CHECK(decoded[2].message.sequence == 9U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::in_order);
    TL_CHECK(parser.stats().frames_decoded == 3U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_compaction_preserves_partial_next_frame() {
    constexpr std::uint32_t complete_frame_count = 200U;
    constexpr std::size_t partial_size = 9U;

    Bytes stream;
    for (std::uint32_t sequence = 0U; sequence < complete_frame_count; ++sequence) {
        append(stream, heartbeat_frame(sequence));
    }
    TL_CHECK(stream.size() > 4096U);

    const auto next = telemetry_link::encode_frame(
        complete_frame_count,
        Payload{sample_imu()}
    );
    stream.insert(
        stream.end(),
        next.begin(),
        next.begin() + static_cast<std::ptrdiff_t>(partial_size)
    );

    StreamParser parser;
    const auto first = parser.feed(as_span(stream));
    TL_CHECK(first.size() == complete_frame_count);
    TL_CHECK(first.back().message.sequence == complete_frame_count - 1U);
    TL_CHECK(parser.stats().frames_decoded == complete_frame_count);
    TL_CHECK(parser.pending_bytes() == partial_size);

    const auto second = parser.feed(
        std::span<const std::uint8_t>{
            next.data() + static_cast<std::ptrdiff_t>(partial_size),
            next.size() - partial_size,
        }
    );
    TL_CHECK(second.size() == 1U);
    TL_CHECK(second.front().message.sequence == complete_frame_count);
    TL_CHECK(second.front().sequence_status == SequenceStatus::in_order);
    check_imu(second.front().message.payload, sample_imu());
    TL_CHECK(parser.stats().frames_decoded == complete_frame_count + 1U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_noise_and_partial_magic() {
    const auto frame = heartbeat_frame(20U);
    constexpr std::size_t partial_magic_size = 3U;

    Bytes first_chunk{0xdeU, 0xadU, 0xbeU};
    first_chunk.insert(
        first_chunk.end(),
        frame.begin(),
        frame.begin() + static_cast<std::ptrdiff_t>(partial_magic_size)
    );

    StreamParser parser;
    const auto first = parser.feed(as_span(first_chunk));
    TL_CHECK(first.empty());
    TL_CHECK(parser.pending_bytes() == partial_magic_size);

    Bytes second_chunk;
    second_chunk.insert(
        second_chunk.end(),
        frame.begin() + static_cast<std::ptrdiff_t>(partial_magic_size),
        frame.end()
    );
    second_chunk.insert(second_chunk.end(), {0x00U, 0x11U, 0x22U, 0x33U});

    const auto second = parser.feed(as_span(second_chunk));
    TL_CHECK(second.size() == 1U);
    TL_CHECK(second.front().message.sequence == 20U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.stats().noise_bytes == 7U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_seeded_random_noise_around_valid_frames() {
    std::mt19937 generator{0x544c4e4bU};
    const auto leading_noise = seeded_noise(513U, generator);
    const auto middle_noise = seeded_noise(777U, generator);
    const auto trailing_noise = seeded_noise(260U, generator);

    Bytes stream = leading_noise;
    append(stream, heartbeat_frame(25U));
    append(stream, middle_noise);
    append(stream, heartbeat_frame(26U));
    append(stream, trailing_noise);

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 2U);
    TL_CHECK(decoded[0].message.sequence == 25U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].message.sequence == 26U);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::in_order);
    TL_CHECK(
        parser.stats().noise_bytes ==
        leading_noise.size() + middle_noise.size() + trailing_noise.size()
    );
    TL_CHECK(parser.stats().frames_decoded == 2U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_bad_crc_recovers_to_next_frame() {
    auto damaged = heartbeat_frame(30U);
    damaged[telemetry_link::kFixedHeaderSize] ^= 0x80U;
    const auto good = heartbeat_frame(31U);

    Bytes stream = damaged;
    append(stream, good);

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 31U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(parser.stats().crc_failures == 1U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_plausible_incomplete_candidate_recovers_embedded_frame() {
    auto candidate_prefix = telemetry_link::encode_frame(35U, Payload{sample_imu()});
    candidate_prefix.resize(telemetry_link::kFixedHeaderSize);
    const auto embedded = heartbeat_frame(36U);
    append(candidate_prefix, embedded);

    StreamParser parser;
    const auto first = parser.feed(as_span(candidate_prefix));
    TL_CHECK(first.empty());
    TL_CHECK(parser.pending_bytes() == candidate_prefix.size());

    // Six bytes complete the advertised 40-byte IMU candidate. Its final four
    // bytes are a deliberately wrong zero CRC; recovery must rescan from byte one
    // and find the complete heartbeat that began inside the failed candidate.
    const Bytes trailing{0x12U, 0x34U, 0x00U, 0x00U, 0x00U, 0x00U};
    Bytes complete_candidate = candidate_prefix;
    append(complete_candidate, trailing);
    constexpr std::size_t checked_size =
        telemetry_link::kFixedHeaderSize - telemetry_link::kMagic.size() + 24U;
    TL_CHECK(
        telemetry_link::crc32(
            as_span(complete_candidate).subspan(telemetry_link::kMagic.size(), checked_size)
        ) != 0U
    );

    const auto second = parser.feed(as_span(trailing));
    TL_CHECK(second.size() == 1U);
    TL_CHECK(second.front().message.sequence == 36U);
    TL_CHECK(second.front().sequence_status == SequenceStatus::first);
    check_heartbeat(second.front().message.payload, sample_heartbeat());
    TL_CHECK(parser.stats().crc_failures == 1U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_unsupported_headers_recover_to_next_frame() {
    auto unsupported_version = heartbeat_frame(37U);
    unsupported_version[telemetry_link::kMagic.size()] =
        static_cast<std::uint8_t>(telemetry_link::kProtocolVersion + 1U);

    auto unknown_type = heartbeat_frame(38U);
    unknown_type[telemetry_link::kMagic.size() + 1U] = 0xfeU;

    Bytes stream = unsupported_version;
    append(stream, unknown_type);
    append(stream, heartbeat_frame(39U));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 39U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(parser.stats().invalid_versions == 1U);
    TL_CHECK(parser.stats().unknown_types == 1U);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_oversize_length_recovers_to_next_frame() {
    auto invalid = heartbeat_frame(40U);
    const auto oversize = static_cast<std::uint16_t>(telemetry_link::kMaxPayloadSize + 1U);
    store_u16_le(invalid, telemetry_link::kMagic.size() + 2U, oversize);

    Bytes stream = invalid;
    append(stream, heartbeat_frame(41U));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 41U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(parser.stats().invalid_lengths == 1U);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_plausible_wrong_length_recovers_to_next_frame() {
    auto invalid = heartbeat_frame(50U);
    store_u16_le(invalid, telemetry_link::kMagic.size() + 2U, 7U);

    Bytes stream = invalid;
    append(stream, heartbeat_frame(51U));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 51U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(parser.stats().invalid_lengths == 1U);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_truncation_at_every_prefix() {
    const auto frame = telemetry_link::encode_frame(60U, Payload{sample_imu()});
    const auto bytes = as_span(frame);

    for (std::size_t prefix_size = 1U; prefix_size < frame.size(); ++prefix_size) {
        StreamParser parser;
        TL_CHECK(parser.feed(bytes.first(prefix_size)).empty());
        TL_CHECK(parser.pending_bytes() == prefix_size);
        parser.finish();
        TL_CHECK(parser.stats().truncated_frames == 1U);
        TL_CHECK(parser.stats().truncated_bytes == prefix_size);
        TL_CHECK(parser.pending_bytes() == 0U);

        parser.finish();
        TL_CHECK(parser.stats().truncated_frames == 1U);
        TL_CHECK(parser.stats().truncated_bytes == prefix_size);
    }

    StreamParser complete_parser;
    TL_CHECK(complete_parser.feed(bytes).size() == 1U);
    complete_parser.finish();
    TL_CHECK(complete_parser.stats().truncated_frames == 0U);
    TL_CHECK(complete_parser.stats().truncated_bytes == 0U);
}

void test_valid_frame_followed_by_truncated_frame() {
    constexpr std::size_t truncated_size = telemetry_link::kFixedHeaderSize + 3U;
    const auto complete = heartbeat_frame(65U);
    const auto incomplete = telemetry_link::encode_frame(66U, Payload{sample_imu()});

    Bytes stream = complete;
    stream.insert(
        stream.end(),
        incomplete.begin(),
        incomplete.begin() + static_cast<std::ptrdiff_t>(truncated_size)
    );

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 65U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.pending_bytes() == truncated_size);

    parser.finish();
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.stats().truncated_frames == 1U);
    TL_CHECK(parser.stats().truncated_bytes == truncated_size);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_payload_error_recovers() {
    auto invalid = telemetry_link::encode_frame(70U, Payload{sample_motor()});
    constexpr std::size_t motor_state_payload_offset = 1U;
    invalid[telemetry_link::kFixedHeaderSize + motor_state_payload_offset] = 0xffU;
    rewrite_crc(invalid);

    Bytes stream = invalid;
    append(stream, heartbeat_frame(71U));

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 71U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(parser.stats().payload_failures == 1U);
    TL_CHECK(parser.stats().crc_failures == 0U);
    TL_CHECK(parser.stats().frames_decoded == 1U);
    TL_CHECK(parser.pending_bytes() == 0U);
}

void test_sequence_gap() {
    StreamParser parser;
    const auto decoded = parse_sequences({10U, 13U}, parser);
    TL_CHECK(decoded.size() == 2U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::gap);
    TL_CHECK(decoded[1].expected_sequence == 11U);
    TL_CHECK(decoded[1].missing_before == 2U);
    TL_CHECK(parser.stats().sequence_gaps == 1U);
    TL_CHECK(parser.stats().missing_frames == 2U);
    TL_CHECK(parser.stats().out_of_order_frames == 0U);
}

void test_sequence_out_of_order_does_not_move_expected_backwards() {
    StreamParser parser;
    const auto decoded = parse_sequences({10U, 12U, 11U, 13U}, parser);
    TL_CHECK(decoded.size() == 4U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::gap);
    TL_CHECK(decoded[1].missing_before == 1U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::out_of_order);
    TL_CHECK(decoded[2].expected_sequence == 13U);
    TL_CHECK(decoded[3].sequence_status == SequenceStatus::in_order);
    TL_CHECK(decoded[3].expected_sequence == 13U);
    TL_CHECK(parser.stats().sequence_gaps == 1U);
    TL_CHECK(parser.stats().missing_frames == 1U);
    TL_CHECK(parser.stats().out_of_order_frames == 1U);
}

void test_duplicate_sequence_is_out_of_order() {
    StreamParser parser;
    const auto decoded = parse_sequences({77U, 77U, 78U}, parser);
    TL_CHECK(decoded.size() == 3U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::out_of_order);
    TL_CHECK(decoded[1].expected_sequence == 78U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::in_order);
    TL_CHECK(decoded[2].expected_sequence == 78U);
    TL_CHECK(parser.stats().out_of_order_frames == 1U);
    TL_CHECK(parser.stats().sequence_gaps == 0U);
}

void test_sequence_wraparound() {
    StreamParser parser;
    const auto decoded = parse_sequences(
        {std::numeric_limits<std::uint32_t>::max(), 0U, 1U},
        parser
    );
    TL_CHECK(decoded.size() == 3U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::in_order);
    TL_CHECK(decoded[1].expected_sequence == 0U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::in_order);
    TL_CHECK(decoded[2].expected_sequence == 1U);
    TL_CHECK(parser.stats().sequence_gaps == 0U);
    TL_CHECK(parser.stats().out_of_order_frames == 0U);
}

void test_sequence_half_range_is_out_of_order() {
    StreamParser parser;
    const auto decoded = parse_sequences({0U, 0x80000001U, 1U}, parser);
    TL_CHECK(decoded.size() == 3U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].expected_sequence == 1U);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::out_of_order);
    TL_CHECK(decoded[1].missing_before == 0U);
    TL_CHECK(decoded[2].expected_sequence == 1U);
    TL_CHECK(decoded[2].sequence_status == SequenceStatus::in_order);
    TL_CHECK(parser.stats().sequence_gaps == 0U);
    TL_CHECK(parser.stats().missing_frames == 0U);
    TL_CHECK(parser.stats().out_of_order_frames == 1U);
}

void test_reset_clears_buffer_sequence_and_stats() {
    StreamParser parser;
    const auto first = heartbeat_frame(1000U);
    TL_CHECK(parser.feed(as_span(first)).size() == 1U);

    const auto incomplete = telemetry_link::encode_frame(1001U, Payload{sample_imu()});
    constexpr std::size_t prefix_size = 7U;
    TL_CHECK(parser.feed(as_span(incomplete).first(prefix_size)).empty());
    TL_CHECK(parser.pending_bytes() == prefix_size);
    TL_CHECK(parser.stats().bytes_received > 0U);
    TL_CHECK(parser.stats().frames_decoded == 1U);

    parser.reset();
    TL_CHECK(parser.pending_bytes() == 0U);
    check_stats_are_zero(parser.stats());

    const auto after_reset = heartbeat_frame(5000U);
    const auto decoded = parser.feed(as_span(after_reset));
    TL_CHECK(decoded.size() == 1U);
    TL_CHECK(decoded.front().message.sequence == 5000U);
    TL_CHECK(decoded.front().sequence_status == SequenceStatus::first);
    TL_CHECK(!decoded.front().expected_sequence.has_value());
    TL_CHECK(parser.stats().bytes_received == after_reset.size());
    TL_CHECK(parser.stats().frames_decoded == 1U);
}

void test_move_construction_preserves_state_and_resets_source() {
    const auto complete = heartbeat_frame(1100U);
    const auto partial = telemetry_link::encode_frame(1101U, Payload{sample_imu()});
    constexpr std::size_t prefix_size = 30U;

    Bytes initial = complete;
    initial.insert(
        initial.end(),
        partial.begin(),
        partial.begin() + static_cast<std::ptrdiff_t>(prefix_size)
    );

    StreamParser source;
    TL_CHECK(source.feed(as_span(initial)).size() == 1U);
    TL_CHECK(source.pending_bytes() == prefix_size);

    StreamParser destination{std::move(source)};
    TL_CHECK(destination.pending_bytes() == prefix_size);
    TL_CHECK(destination.stats().frames_decoded == 1U);
    TL_CHECK(source.pending_bytes() == 0U);
    check_stats_are_zero(source.stats());

    const auto completed = destination.feed(
        as_span(partial).subspan(prefix_size)
    );
    TL_CHECK(completed.size() == 1U);
    TL_CHECK(completed.front().message.sequence == 1101U);
    TL_CHECK(completed.front().sequence_status == SequenceStatus::in_order);

    const auto reused = source.feed(as_span(heartbeat_frame(9000U)));
    TL_CHECK(reused.size() == 1U);
    TL_CHECK(reused.front().sequence_status == SequenceStatus::first);
}

void test_move_assignment_preserves_state_and_handles_self_move() {
    const auto complete = heartbeat_frame(1200U);
    const auto partial = telemetry_link::encode_frame(1201U, Payload{sample_imu()});
    constexpr std::size_t prefix_size = 30U;

    Bytes initial = complete;
    initial.insert(
        initial.end(),
        partial.begin(),
        partial.begin() + static_cast<std::ptrdiff_t>(prefix_size)
    );

    StreamParser source;
    TL_CHECK(source.feed(as_span(initial)).size() == 1U);

    StreamParser destination;
    TL_CHECK(destination.feed(as_span(heartbeat_frame(42U))).size() == 1U);
    destination = std::move(source);
    TL_CHECK(destination.pending_bytes() == prefix_size);
    TL_CHECK(destination.stats().frames_decoded == 1U);
    TL_CHECK(source.pending_bytes() == 0U);
    check_stats_are_zero(source.stats());

    auto* const destination_alias = &destination;
    destination = std::move(*destination_alias);
    TL_CHECK(destination.pending_bytes() == prefix_size);
    TL_CHECK(destination.stats().frames_decoded == 1U);

    const auto completed = destination.feed(
        as_span(partial).subspan(prefix_size)
    );
    TL_CHECK(completed.size() == 1U);
    TL_CHECK(completed.front().message.sequence == 1201U);
    TL_CHECK(completed.front().sequence_status == SequenceStatus::in_order);
}

void test_corrupt_frame_does_not_advance_sequence() {
    const auto first = heartbeat_frame(90U);
    auto corrupt = heartbeat_frame(91U);
    corrupt[telemetry_link::kFixedHeaderSize + 2U] ^= 0x40U;
    const auto third = heartbeat_frame(92U);

    Bytes stream = first;
    append(stream, corrupt);
    append(stream, third);

    StreamParser parser;
    const auto decoded = parser.feed(as_span(stream));
    TL_CHECK(decoded.size() == 2U);
    TL_CHECK(decoded[0].message.sequence == 90U);
    TL_CHECK(decoded[0].sequence_status == SequenceStatus::first);
    TL_CHECK(decoded[1].message.sequence == 92U);
    TL_CHECK(decoded[1].sequence_status == SequenceStatus::gap);
    TL_CHECK(decoded[1].expected_sequence == 91U);
    TL_CHECK(decoded[1].missing_before == 1U);
    TL_CHECK(parser.stats().crc_failures == 1U);
    TL_CHECK(parser.stats().sequence_gaps == 1U);
    TL_CHECK(parser.stats().missing_frames == 1U);
}

using TestFunction = void (*)();

struct TestCase {
    const char* name;
    TestFunction run;
};

} // namespace

int main() {
    const std::array tests{
        TestCase{"crc32 known vector", test_crc32_known_vector},
        TestCase{"public name helpers", test_public_name_helpers},
        TestCase{"fixed golden heartbeat bytes", test_fixed_golden_heartbeat_bytes},
        TestCase{"all message round trips", test_all_message_round_trips},
        TestCase{"valid payload containing magic", test_valid_payload_may_contain_magic},
        TestCase{"invalid motor state encoding", test_encoder_rejects_invalid_motor_state},
        TestCase{"parser payload limit boundaries", test_parser_payload_limit_boundaries},
        TestCase{"every split point", test_every_split_point},
        TestCase{"byte at a time", test_byte_at_a_time},
        TestCase{"multiple frames in one chunk", test_multiple_frames_in_one_chunk},
        TestCase{"compaction preserves partial frame", test_compaction_preserves_partial_next_frame},
        TestCase{"noise and partial magic", test_noise_and_partial_magic},
        TestCase{"seeded random noise", test_seeded_random_noise_around_valid_frames},
        TestCase{"bad CRC recovery", test_bad_crc_recovers_to_next_frame},
        TestCase{"plausible incomplete candidate recovery", test_plausible_incomplete_candidate_recovers_embedded_frame},
        TestCase{"unsupported header recovery", test_unsupported_headers_recover_to_next_frame},
        TestCase{"oversize length recovery", test_oversize_length_recovers_to_next_frame},
        TestCase{"plausible wrong length recovery", test_plausible_wrong_length_recovers_to_next_frame},
        TestCase{"truncation at every prefix", test_truncation_at_every_prefix},
        TestCase{"valid then truncated frame", test_valid_frame_followed_by_truncated_frame},
        TestCase{"payload error recovery", test_payload_error_recovers},
        TestCase{"sequence gap", test_sequence_gap},
        TestCase{"sequence out of order", test_sequence_out_of_order_does_not_move_expected_backwards},
        TestCase{"duplicate sequence", test_duplicate_sequence_is_out_of_order},
        TestCase{"sequence wraparound", test_sequence_wraparound},
        TestCase{"sequence half-range", test_sequence_half_range_is_out_of_order},
        TestCase{"reset clears state", test_reset_clears_buffer_sequence_and_stats},
        TestCase{"move construction", test_move_construction_preserves_state_and_resets_source},
        TestCase{"move assignment", test_move_assignment_preserves_state_and_handles_self_move},
        TestCase{"corrupt frame sequence isolation", test_corrupt_frame_does_not_advance_sequence},
    };

    std::size_t failures = 0U;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    if (failures != 0U) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << tests.size() << " tests passed\n";
    return 0;
}
