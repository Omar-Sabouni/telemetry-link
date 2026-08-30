#include "telemetry_link/telemetry_link.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace telemetry_link {
namespace {

constexpr std::size_t kHeartbeatPayloadSize = 6U;
constexpr std::size_t kImuPayloadSize = 24U;
constexpr std::size_t kMotorStatusPayloadSize = 14U;
constexpr std::uint32_t kSequenceHalfRange = 0x80000000U;
constexpr std::size_t kVersionOffset = kMagic.size();
constexpr std::size_t kTypeOffset = kVersionOffset + 1U;
constexpr std::size_t kLengthOffset = kTypeOffset + 1U;
constexpr std::size_t kSequenceOffset = kLengthOffset + 2U;

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);

consteval std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U
                ? (value >> 1U) ^ 0xedb88320U
                : value >> 1U;
        }
        table[index] = value;
    }
    return table;
}

inline constexpr auto kCrcTable = make_crc_table();

void saturating_add(std::uint64_t& counter, std::uint64_t amount = 1U) noexcept {
    const auto available = std::numeric_limits<std::uint64_t>::max() - counter;
    counter += std::min(amount, available);
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_i32(std::vector<std::uint8_t>& output, std::int32_t value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

void append_f32(std::vector<std::uint8_t>& output, float value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U)
    );
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] std::int32_t read_i32(std::span<const std::uint8_t> bytes) {
    return std::bit_cast<std::int32_t>(read_u32(bytes));
}

[[nodiscard]] float read_f32(std::span<const std::uint8_t> bytes) {
    return std::bit_cast<float>(read_u32(bytes));
}

[[nodiscard]] std::optional<MessageType> decode_message_type(std::uint8_t raw) {
    switch (raw) {
    case static_cast<std::uint8_t>(MessageType::heartbeat):
        return MessageType::heartbeat;
    case static_cast<std::uint8_t>(MessageType::imu_sample):
        return MessageType::imu_sample;
    case static_cast<std::uint8_t>(MessageType::motor_status):
        return MessageType::motor_status;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::size_t expected_payload_size(MessageType type) {
    switch (type) {
    case MessageType::heartbeat:
        return kHeartbeatPayloadSize;
    case MessageType::imu_sample:
        return kImuPayloadSize;
    case MessageType::motor_status:
        return kMotorStatusPayloadSize;
    }
    return 0U;
}

[[nodiscard]] std::vector<std::uint8_t> encode_payload(const Payload& payload) {
    std::vector<std::uint8_t> bytes;
    std::visit(
        [&bytes](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Heartbeat>) {
                bytes.reserve(kHeartbeatPayloadSize);
                append_u32(bytes, value.uptime_ms);
                append_u16(bytes, value.status_flags);
            } else if constexpr (std::is_same_v<Value, ImuSample>) {
                bytes.reserve(kImuPayloadSize);
                append_f32(bytes, value.accel_x_mps2);
                append_f32(bytes, value.accel_y_mps2);
                append_f32(bytes, value.accel_z_mps2);
                append_f32(bytes, value.gyro_x_rad_s);
                append_f32(bytes, value.gyro_y_rad_s);
                append_f32(bytes, value.gyro_z_rad_s);
            } else if constexpr (std::is_same_v<Value, MotorStatus>) {
                if (static_cast<std::uint8_t>(value.state) >
                    static_cast<std::uint8_t>(MotorState::fault)) {
                    throw std::invalid_argument("invalid motor state");
                }
                bytes.reserve(kMotorStatusPayloadSize);
                bytes.push_back(value.motor_id);
                bytes.push_back(static_cast<std::uint8_t>(value.state));
                append_i32(bytes, value.rpm);
                append_f32(bytes, value.current_a);
                append_f32(bytes, value.temperature_c);
            }
        },
        payload
    );
    return bytes;
}

[[nodiscard]] std::optional<Payload> decode_payload(
    MessageType type,
    std::span<const std::uint8_t> bytes
) {
    if (bytes.size() != expected_payload_size(type)) {
        return std::nullopt;
    }

    switch (type) {
    case MessageType::heartbeat:
        return Heartbeat{
            .uptime_ms = read_u32(bytes.subspan(0U, 4U)),
            .status_flags = read_u16(bytes.subspan(4U, 2U)),
        };
    case MessageType::imu_sample:
        return ImuSample{
            .accel_x_mps2 = read_f32(bytes.subspan(0U, 4U)),
            .accel_y_mps2 = read_f32(bytes.subspan(4U, 4U)),
            .accel_z_mps2 = read_f32(bytes.subspan(8U, 4U)),
            .gyro_x_rad_s = read_f32(bytes.subspan(12U, 4U)),
            .gyro_y_rad_s = read_f32(bytes.subspan(16U, 4U)),
            .gyro_z_rad_s = read_f32(bytes.subspan(20U, 4U)),
        };
    case MessageType::motor_status: {
        const auto raw_state = bytes[1U];
        if (raw_state > static_cast<std::uint8_t>(MotorState::fault)) {
            return std::nullopt;
        }
        return MotorStatus{
            .motor_id = bytes[0U],
            .state = static_cast<MotorState>(raw_state),
            .rpm = read_i32(bytes.subspan(2U, 4U)),
            .current_a = read_f32(bytes.subspan(6U, 4U)),
            .temperature_c = read_f32(bytes.subspan(10U, 4U)),
        };
    }
    }
    return std::nullopt;
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        const auto table_index = static_cast<std::uint8_t>((crc ^ byte) & 0xffU);
        crc = (crc >> 8U) ^ kCrcTable[table_index];
    }
    return crc ^ 0xffffffffU;
}

MessageType message_type(const Payload& payload) noexcept {
    return std::visit(
        [](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Heartbeat>) {
                return MessageType::heartbeat;
            } else if constexpr (std::is_same_v<Value, ImuSample>) {
                return MessageType::imu_sample;
            } else {
                return MessageType::motor_status;
            }
        },
        payload
    );
}

std::string_view message_type_name(MessageType type) noexcept {
    switch (type) {
    case MessageType::heartbeat:
        return "heartbeat";
    case MessageType::imu_sample:
        return "imu_sample";
    case MessageType::motor_status:
        return "motor_status";
    }
    return "unknown";
}

std::string_view motor_state_name(MotorState state) noexcept {
    switch (state) {
    case MotorState::stopped:
        return "stopped";
    case MotorState::running:
        return "running";
    case MotorState::fault:
        return "fault";
    }
    return "unknown";
}

std::string_view sequence_status_name(SequenceStatus status) noexcept {
    switch (status) {
    case SequenceStatus::first:
        return "first";
    case SequenceStatus::in_order:
        return "in_order";
    case SequenceStatus::gap:
        return "gap";
    case SequenceStatus::out_of_order:
        return "out_of_order";
    }
    return "unknown";
}

std::vector<std::uint8_t> encode_frame(
    std::uint32_t sequence,
    const Payload& payload
) {
    const auto type = message_type(payload);
    const auto payload_bytes = encode_payload(payload);
    if (payload_bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("payload does not fit the wire length field");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(kFixedHeaderSize + payload_bytes.size() + kCrcSize);
    frame.insert(frame.end(), kMagic.begin(), kMagic.end());
    frame.push_back(kProtocolVersion);
    frame.push_back(static_cast<std::uint8_t>(type));
    append_u16(frame, static_cast<std::uint16_t>(payload_bytes.size()));
    append_u32(frame, sequence);
    frame.insert(frame.end(), payload_bytes.begin(), payload_bytes.end());

    const auto checksum = crc32(std::span<const std::uint8_t>{frame}.subspan(kMagic.size()));
    append_u32(frame, checksum);
    return frame;
}

StreamParser::StreamParser(std::size_t max_payload_size)
    : max_payload_size_(max_payload_size) {
    if (max_payload_size > kMaxPayloadSize) {
        throw std::invalid_argument("maximum payload size exceeds the protocol limit");
    }
}

StreamParser::StreamParser(StreamParser&& other) noexcept
    : max_payload_size_(other.max_payload_size_),
      buffer_(std::move(other.buffer_)),
      head_(other.head_),
      next_sequence_(other.next_sequence_),
      stats_(other.stats_) {
    other.reset();
}

StreamParser& StreamParser::operator=(StreamParser&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    max_payload_size_ = other.max_payload_size_;
    buffer_ = std::move(other.buffer_);
    head_ = other.head_;
    next_sequence_ = other.next_sequence_;
    stats_ = other.stats_;
    other.reset();
    return *this;
}

std::vector<DecodedFrame> StreamParser::feed(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    saturating_add(stats_.bytes_received, static_cast<std::uint64_t>(bytes.size()));
    return parse_available();
}

std::vector<DecodedFrame> StreamParser::parse_available() {
    std::vector<DecodedFrame> decoded;

    while (buffer_.size() > head_) {
        auto magic_position = head_;
        while (magic_position + kMagic.size() <= buffer_.size() &&
               !std::equal(
                   kMagic.begin(),
                   kMagic.end(),
                   buffer_.begin() + static_cast<std::ptrdiff_t>(magic_position)
               )) {
            ++magic_position;
        }

        if (magic_position + kMagic.size() > buffer_.size()) {
            std::size_t keep_count = 0U;
            const auto available_suffix = buffer_.size() - head_;
            const auto max_prefix = std::min(kMagic.size() - 1U, available_suffix);
            for (auto prefix_size = max_prefix; prefix_size > 0U; --prefix_size) {
                if (std::equal(
                        buffer_.end() - static_cast<std::ptrdiff_t>(prefix_size),
                        buffer_.end(),
                        kMagic.begin()
                    )) {
                    keep_count = prefix_size;
                    break;
                }
            }
            const auto discarded = buffer_.size() - head_ - keep_count;
            saturating_add(stats_.noise_bytes, static_cast<std::uint64_t>(discarded));
            head_ += discarded;
            break;
        }

        if (magic_position > head_) {
            saturating_add(
                stats_.noise_bytes,
                static_cast<std::uint64_t>(magic_position - head_)
            );
            head_ = magic_position;
        }

        const auto available = buffer_.size() - head_;
        if (available < kFixedHeaderSize) {
            break;
        }

        const auto candidate = std::span<const std::uint8_t>{buffer_}.subspan(head_);
        const auto version = candidate[kVersionOffset];
        if (version != kProtocolVersion) {
            saturating_add(stats_.invalid_versions);
            ++head_;
            continue;
        }

        const auto payload_length = static_cast<std::size_t>(
            read_u16(candidate.subspan(kLengthOffset, 2U))
        );
        if (payload_length > max_payload_size_) {
            saturating_add(stats_.invalid_lengths);
            ++head_;
            continue;
        }

        const auto type = decode_message_type(candidate[kTypeOffset]);
        if (!type.has_value()) {
            saturating_add(stats_.unknown_types);
            ++head_;
            continue;
        }
        if (payload_length != expected_payload_size(*type)) {
            saturating_add(stats_.invalid_lengths);
            ++head_;
            continue;
        }

        const auto frame_size = kFixedHeaderSize + payload_length + kCrcSize;
        if (available < frame_size) {
            break;
        }

        const auto checksum_offset = kFixedHeaderSize + payload_length;
        const auto wire_checksum = read_u32(candidate.subspan(checksum_offset, kCrcSize));
        const auto checked_region = candidate.subspan(
            kMagic.size(),
            kFixedHeaderSize - kMagic.size() + payload_length
        );
        if (crc32(checked_region) != wire_checksum) {
            saturating_add(stats_.crc_failures);
            ++head_;
            continue;
        }

        const auto payload = decode_payload(
            *type,
            candidate.subspan(kFixedHeaderSize, payload_length)
        );
        if (!payload.has_value()) {
            saturating_add(stats_.payload_failures);
            head_ += frame_size;
            continue;
        }

        DecodedFrame frame{
            .message = Message{
                .version = version,
                .sequence = read_u32(candidate.subspan(kSequenceOffset, 4U)),
                .payload = *payload,
            },
        };
        decoded.push_back(std::move(frame));
        annotate_sequence(decoded.back());
        saturating_add(stats_.frames_decoded);
        head_ += frame_size;
    }

    compact_buffer();
    return decoded;
}

void StreamParser::annotate_sequence(DecodedFrame& frame) {
    const auto sequence = frame.message.sequence;
    if (!next_sequence_.has_value()) {
        frame.sequence_status = SequenceStatus::first;
        next_sequence_ = sequence + 1U;
        return;
    }

    const auto expected = *next_sequence_;
    frame.expected_sequence = expected;
    if (sequence == expected) {
        frame.sequence_status = SequenceStatus::in_order;
        next_sequence_ = sequence + 1U;
        return;
    }

    const auto forward_distance = sequence - expected;
    if (forward_distance < kSequenceHalfRange) {
        frame.sequence_status = SequenceStatus::gap;
        frame.missing_before = forward_distance;
        saturating_add(stats_.sequence_gaps);
        saturating_add(
            stats_.missing_frames,
            static_cast<std::uint64_t>(forward_distance)
        );
        next_sequence_ = sequence + 1U;
        return;
    }

    frame.sequence_status = SequenceStatus::out_of_order;
    saturating_add(stats_.out_of_order_frames);
}

void StreamParser::compact_buffer() {
    if (head_ == buffer_.size()) {
        buffer_.clear();
        head_ = 0U;
        return;
    }

    if (head_ >= 4096U || head_ >= buffer_.size() / 2U) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0U;
    }
}

void StreamParser::finish() {
    const auto remaining = pending_bytes();
    if (remaining != 0U) {
        saturating_add(stats_.truncated_frames);
        saturating_add(stats_.truncated_bytes, static_cast<std::uint64_t>(remaining));
    }
    std::vector<std::uint8_t>{}.swap(buffer_);
    head_ = 0U;
}

void StreamParser::reset() {
    std::vector<std::uint8_t>{}.swap(buffer_);
    head_ = 0U;
    next_sequence_.reset();
    stats_ = {};
}

const ParserStats& StreamParser::stats() const noexcept {
    return stats_;
}

std::size_t StreamParser::pending_bytes() const noexcept {
    return buffer_.size() - head_;
}

} // namespace telemetry_link
