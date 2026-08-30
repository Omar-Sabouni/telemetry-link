#include "telemetry_link/telemetry_link.hpp"

#include <cstdint>
#include <span>
#include <variant>

int main() {
    constexpr std::uint32_t sequence = 42U;
    const telemetry_link::Heartbeat heartbeat{
        .uptime_ms = 1234U,
        .status_flags = 0x05U,
    };
    const auto encoded = telemetry_link::encode_frame(sequence, heartbeat);

    telemetry_link::StreamParser parser;
    const auto decoded = parser.feed(std::span<const std::uint8_t>{encoded});
    if (decoded.size() != 1U || decoded.front().message.sequence != sequence) {
        return 1;
    }

    const auto* payload = std::get_if<telemetry_link::Heartbeat>(
        &decoded.front().message.payload
    );
    if (payload == nullptr || payload->uptime_ms != heartbeat.uptime_ms ||
        payload->status_flags != heartbeat.status_flags) {
        return 1;
    }

    return 0;
}
