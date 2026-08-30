#include "telemetry_link/telemetry_link.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kDefaultFrames = 2'000'000U;
constexpr std::size_t kDefaultTrials = 5U;
constexpr std::size_t kChunkSize = 4096U;
constexpr std::size_t kMinimumFrames = 100'000U;
constexpr std::size_t kMaximumFrames = 10'000'000U;
constexpr std::size_t kMaximumTrials = 20U;

struct RunResult {
    double seconds{};
    std::uint64_t decoded_frames{};
    std::uint64_t checksum{};
};

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] telemetry_link::Payload sample_payload(std::size_t index) {
    switch (index % 3U) {
    case 0U:
        return telemetry_link::Heartbeat{
            .uptime_ms = static_cast<std::uint32_t>(index * 10U),
            .status_flags = static_cast<std::uint16_t>(index & 0x0fU),
        };
    case 1U:
        return telemetry_link::ImuSample{
            .accel_x_mps2 = static_cast<float>(index % 100U) * 0.01F,
            .accel_y_mps2 = -0.25F,
            .accel_z_mps2 = 9.80665F,
            .gyro_x_rad_s = 0.001F,
            .gyro_y_rad_s = -0.002F,
            .gyro_z_rad_s = 0.003F,
        };
    default:
        return telemetry_link::MotorStatus{
            .motor_id = static_cast<std::uint8_t>(index % 4U),
            .state = telemetry_link::MotorState::running,
            .rpm = static_cast<std::int32_t>(1200U + index % 500U),
            .current_a = 4.5F,
            .temperature_c = 42.0F,
        };
    }
}

[[nodiscard]] std::vector<std::uint8_t> make_corpus(std::size_t frame_count) {
    std::vector<std::uint8_t> corpus;
    corpus.reserve(frame_count * 32U);
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto frame = telemetry_link::encode_frame(
            static_cast<std::uint32_t>(index),
            sample_payload(index)
        );
        corpus.insert(corpus.end(), frame.begin(), frame.end());
    }
    return corpus;
}

[[nodiscard]] RunResult parse_corpus(std::span<const std::uint8_t> corpus) {
    telemetry_link::StreamParser parser;
    std::uint64_t checksum = 0U;
    std::uint64_t decoded_count = 0U;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t offset = 0U; offset < corpus.size(); offset += kChunkSize) {
        const auto length = std::min(kChunkSize, corpus.size() - offset);
        const auto decoded = parser.feed(corpus.subspan(offset, length));
        decoded_count += static_cast<std::uint64_t>(decoded.size());
        for (const auto& frame : decoded) {
            checksum += frame.message.sequence;
            checksum += static_cast<std::uint8_t>(
                telemetry_link::message_type(frame.message.payload)
            );
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    parser.finish();

    if (parser.stats().frames_decoded != decoded_count ||
        parser.stats().crc_failures != 0U ||
        parser.stats().truncated_frames != 0U) {
        return {};
    }

    return RunResult{
        .seconds = std::chrono::duration<double>(stop - start).count(),
        .decoded_frames = decoded_count,
        .checksum = checksum,
    };
}

[[nodiscard]] std::string_view compiler_name() {
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string_view build_mode() {
#ifdef TELEMETRY_LINK_BUILD_CONFIG
    return TELEMETRY_LINK_BUILD_CONFIG;
#else
    return "unknown";
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: " << argv[0] << " [FRAME_COUNT [TRIALS]]\n";
        return 2;
    }

    std::size_t frame_count = kDefaultFrames;
    std::size_t trial_count = kDefaultTrials;
    if (argc >= 2 && (!parse_size(argv[1], frame_count) ||
                      frame_count < kMinimumFrames ||
                      frame_count > kMaximumFrames)) {
        std::cerr << "FRAME_COUNT must be between " << kMinimumFrames
                  << " and " << kMaximumFrames << "\n";
        return 2;
    }
    if (argc == 3 && (!parse_size(argv[2], trial_count) || trial_count == 0U ||
                      trial_count > kMaximumTrials)) {
        std::cerr << "TRIALS must be between 1 and " << kMaximumTrials << "\n";
        return 2;
    }

    const auto corpus = make_corpus(frame_count);
    const auto warmup = parse_corpus(corpus);
    if (warmup.decoded_frames != frame_count) {
        std::cerr << "benchmark validation failed during warm-up\n";
        return 1;
    }

    std::vector<RunResult> results;
    results.reserve(trial_count);
    for (std::size_t trial = 0U; trial < trial_count; ++trial) {
        const auto result = parse_corpus(corpus);
        if (result.decoded_frames != frame_count || result.checksum != warmup.checksum) {
            std::cerr << "benchmark validation failed during trial " << trial + 1U << '\n';
            return 1;
        }
        results.push_back(result);
    }

    std::sort(
        results.begin(),
        results.end(),
        [](const RunResult& left, const RunResult& right) {
            return left.seconds < right.seconds;
        }
    );
    const auto middle = results.size() / 2U;
    auto median_seconds = results[middle].seconds;
    if (results.size() % 2U == 0U) {
        median_seconds = (results[middle - 1U].seconds + results[middle].seconds) / 2.0;
    }
    const auto mebibytes = static_cast<double>(corpus.size()) / (1024.0 * 1024.0);
    const auto mib_per_second = mebibytes / median_seconds;
    const auto frames_per_second = static_cast<double>(frame_count) / median_seconds;

    std::cout << "TelemetryLink parse benchmark\n"
              << "  compiler: " << compiler_name() << '\n'
              << "  build: " << build_mode() << '\n'
              << "  workload: " << frame_count << " mixed frames, "
              << std::fixed << std::setprecision(2) << mebibytes << " MiB, "
              << kChunkSize << "-byte chunks\n"
              << "  timing: median of " << trial_count
              << " trials; generation and file I/O excluded\n"
              << "  elapsed: " << std::setprecision(6) << median_seconds << " s\n"
              << "  throughput: " << std::setprecision(2) << mib_per_second << " MiB/s\n"
              << "  frame rate: " << std::setprecision(0) << frames_per_second
              << " frames/s\n"
              << "  decoded: " << warmup.decoded_frames
              << ", checksum: " << warmup.checksum << '\n';
    return 0;
}
