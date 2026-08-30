#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace telemetry_link {

inline constexpr std::array<std::uint8_t, 4> kMagic{0x54U, 0x4cU, 0x4eU, 0x4bU}; // "TLNK"
inline constexpr std::uint8_t kProtocolVersion = 1U;
inline constexpr std::size_t kFixedHeaderSize = kMagic.size() + 8U;
inline constexpr std::size_t kCrcSize = 4U;
inline constexpr std::size_t kMaxPayloadSize = 1024U;

enum class MessageType : std::uint8_t {
    heartbeat = 0x01U,
    imu_sample = 0x02U,
    motor_status = 0x03U,
};

enum class MotorState : std::uint8_t {
    stopped = 0U,
    running = 1U,
    fault = 2U,
};

struct Heartbeat {
    std::uint32_t uptime_ms{};
    std::uint16_t status_flags{};
};

struct ImuSample {
    float accel_x_mps2{};
    float accel_y_mps2{};
    float accel_z_mps2{};
    float gyro_x_rad_s{};
    float gyro_y_rad_s{};
    float gyro_z_rad_s{};
};

struct MotorStatus {
    std::uint8_t motor_id{};
    MotorState state{MotorState::stopped};
    std::int32_t rpm{};
    float current_a{};
    float temperature_c{};
};

using Payload = std::variant<Heartbeat, ImuSample, MotorStatus>;

struct Message {
    std::uint8_t version{kProtocolVersion};
    std::uint32_t sequence{};
    Payload payload{};
};

enum class SequenceStatus {
    first,
    in_order,
    gap,
    out_of_order,
};

struct DecodedFrame {
    Message message{};
    SequenceStatus sequence_status{SequenceStatus::first};
    std::optional<std::uint32_t> expected_sequence{};
    std::uint32_t missing_before{};
};

struct ParserStats {
    std::uint64_t bytes_received{};
    std::uint64_t frames_decoded{};
    std::uint64_t noise_bytes{};
    std::uint64_t invalid_versions{};
    std::uint64_t unknown_types{};
    std::uint64_t invalid_lengths{};
    std::uint64_t crc_failures{};
    std::uint64_t payload_failures{};
    std::uint64_t truncated_frames{};
    std::uint64_t truncated_bytes{};
    std::uint64_t sequence_gaps{};
    std::uint64_t missing_frames{};
    std::uint64_t out_of_order_frames{};
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] MessageType message_type(const Payload& payload) noexcept;
[[nodiscard]] std::string_view message_type_name(MessageType type) noexcept;
[[nodiscard]] std::string_view motor_state_name(MotorState state) noexcept;
[[nodiscard]] std::string_view sequence_status_name(SequenceStatus status) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encode_frame(
    std::uint32_t sequence,
    const Payload& payload
);

class StreamParser {
public:
    explicit StreamParser(std::size_t max_payload_size = kMaxPayloadSize);
    StreamParser(const StreamParser&) = default;
    StreamParser& operator=(const StreamParser&) = default;
    StreamParser(StreamParser&& other) noexcept;
    StreamParser& operator=(StreamParser&& other) noexcept;

    [[nodiscard]] std::vector<DecodedFrame> feed(std::span<const std::uint8_t> bytes);
    void finish();
    void reset();

    [[nodiscard]] const ParserStats& stats() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;

private:
    [[nodiscard]] std::vector<DecodedFrame> parse_available();
    void compact_buffer();
    void annotate_sequence(DecodedFrame& frame);

    std::size_t max_payload_size_;
    std::vector<std::uint8_t> buffer_;
    std::size_t head_{};
    std::optional<std::uint32_t> next_sequence_{};
    ParserStats stats_{};
};

} // namespace telemetry_link
