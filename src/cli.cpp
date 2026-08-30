#include "telemetry_link/telemetry_link.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using telemetry_link::DecodedFrame;
using telemetry_link::Heartbeat;
using telemetry_link::ImuSample;
using telemetry_link::MessageType;
using telemetry_link::MotorStatus;
using telemetry_link::ParserStats;

struct TypeCounts {
    std::uint64_t heartbeat{};
    std::uint64_t imu_sample{};
    std::uint64_t motor_status{};
};

enum class ColorMode {
    automatic,
    always,
    never,
};

struct CliOptions {
    const char* input_path{};
    bool fail_on_degraded{};
    bool diagnostics{};
    ColorMode color_mode{ColorMode::automatic};
};

namespace ansi {

inline constexpr std::string_view reset{"\x1b[0m"};
inline constexpr std::string_view dim{"\x1b[2m"};
inline constexpr std::string_view blue{"\x1b[34m"};
inline constexpr std::string_view green{"\x1b[32m"};
inline constexpr std::string_view yellow{"\x1b[33m"};

} // namespace ansi

[[nodiscard]] bool standard_error_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

[[nodiscard]] std::optional<std::string> environment_variable(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0U;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        std::free(value);
        return std::nullopt;
    }
    std::string copy{value};
    std::free(value);
    return copy;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

[[nodiscard]] bool color_enabled(ColorMode mode) {
    if (environment_variable("NO_COLOR").has_value()) {
        return false;
    }
    if (mode == ColorMode::always) {
        return true;
    }
    if (mode == ColorMode::never) {
        return false;
    }
    const auto term = environment_variable("TERM");
    if (term.has_value() && *term == "dumb") {
        return false;
    }
    return standard_error_is_terminal();
}

void write_styled(
    std::ostream& output,
    bool use_color,
    std::string_view style,
    std::string_view text
) {
    if (use_color) {
        output << style;
    }
    output << text;
    if (use_color) {
        output << ansi::reset;
    }
}

void write_visible(std::ostream& output, std::string_view text) {
    constexpr std::string_view hexadecimal{"0123456789abcdef"};
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x20U && byte != 0x7fU) {
            output.put(character);
            continue;
        }
        output << "\\x" << hexadecimal[(byte >> 4U) & 0x0fU]
               << hexadecimal[byte & 0x0fU];
    }
}

void write_float(std::ostream& output, float value) {
    if (!std::isfinite(value)) {
        output << "null";
        return;
    }
    output << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
}

void write_payload(std::ostream& output, const telemetry_link::Payload& payload) {
    std::visit(
        [&output](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Heartbeat>) {
                output << "{\"uptime_ms\":" << value.uptime_ms
                       << ",\"status_flags\":" << value.status_flags << '}';
            } else if constexpr (std::is_same_v<Value, ImuSample>) {
                output << "{\"accel_mps2\":[";
                write_float(output, value.accel_x_mps2);
                output << ',';
                write_float(output, value.accel_y_mps2);
                output << ',';
                write_float(output, value.accel_z_mps2);
                output << "],\"gyro_rad_s\":[";
                write_float(output, value.gyro_x_rad_s);
                output << ',';
                write_float(output, value.gyro_y_rad_s);
                output << ',';
                write_float(output, value.gyro_z_rad_s);
                output << "]}";
            } else if constexpr (std::is_same_v<Value, MotorStatus>) {
                output << "{\"motor_id\":" << static_cast<unsigned int>(value.motor_id)
                       << ",\"state\":\"" << telemetry_link::motor_state_name(value.state)
                       << "\",\"rpm\":" << value.rpm << ",\"current_a\":";
                write_float(output, value.current_a);
                output << ",\"temperature_c\":";
                write_float(output, value.temperature_c);
                output << '}';
            }
        },
        payload
    );
}

void write_frame(const DecodedFrame& frame, TypeCounts& counts) {
    const auto type = telemetry_link::message_type(frame.message.payload);
    switch (type) {
    case MessageType::heartbeat:
        ++counts.heartbeat;
        break;
    case MessageType::imu_sample:
        ++counts.imu_sample;
        break;
    case MessageType::motor_status:
        ++counts.motor_status;
        break;
    }

    std::cout << "{\"kind\":\"message\",\"version\":"
              << static_cast<unsigned int>(frame.message.version)
              << ",\"sequence\":" << frame.message.sequence
              << ",\"sequence_status\":\""
              << telemetry_link::sequence_status_name(frame.sequence_status) << '"';
    if (frame.expected_sequence.has_value()) {
        std::cout << ",\"expected_sequence\":" << *frame.expected_sequence;
    }
    if (frame.missing_before != 0U) {
        std::cout << ",\"missing_before\":" << frame.missing_before;
    }
    std::cout << ",\"type\":\"" << telemetry_link::message_type_name(type)
              << "\",\"payload\":";
    write_payload(std::cout, frame.message.payload);
    std::cout << "}\n";
}

[[nodiscard]] bool is_clean(const ParserStats& stats) {
    return stats.noise_bytes == 0U &&
        stats.invalid_versions == 0U &&
        stats.unknown_types == 0U &&
        stats.invalid_lengths == 0U &&
        stats.crc_failures == 0U &&
        stats.payload_failures == 0U &&
        stats.truncated_frames == 0U &&
        stats.sequence_gaps == 0U &&
        stats.out_of_order_frames == 0U;
}

void write_health(const ParserStats& stats, const TypeCounts& counts) {
    std::cout << "{\"kind\":\"health\",\"status\":\""
              << (is_clean(stats) ? "ok" : "degraded")
              << "\",\"bytes_received\":" << stats.bytes_received
              << ",\"frames_decoded\":" << stats.frames_decoded
              << ",\"heartbeat_frames\":" << counts.heartbeat
              << ",\"imu_sample_frames\":" << counts.imu_sample
              << ",\"motor_status_frames\":" << counts.motor_status
              << ",\"noise_bytes\":" << stats.noise_bytes
              << ",\"invalid_versions\":" << stats.invalid_versions
              << ",\"unknown_types\":" << stats.unknown_types
              << ",\"invalid_lengths\":" << stats.invalid_lengths
              << ",\"crc_failures\":" << stats.crc_failures
              << ",\"payload_failures\":" << stats.payload_failures
              << ",\"truncated_frames\":" << stats.truncated_frames
              << ",\"truncated_bytes\":" << stats.truncated_bytes
              << ",\"sequence_gaps\":" << stats.sequence_gaps
              << ",\"missing_frames\":" << stats.missing_frames
              << ",\"out_of_order_frames\":" << stats.out_of_order_frames
              << "}\n";
}

void write_diagnostic_label(std::string_view label, bool use_color) {
    write_styled(std::cerr, use_color, ansi::dim, label);
    const auto padding = label.size() < 10U ? 10U - label.size() : 1U;
    for (std::size_t index = 0U; index < padding; ++index) {
        std::cerr.put(' ');
    }
}

void write_diagnostics(
    const ParserStats& stats,
    const TypeCounts& counts,
    bool file_input,
    bool use_color
) {
    const auto clean = is_clean(stats);
    write_styled(
        std::cerr,
        use_color,
        ansi::blue,
        "TelemetryLink diagnostics"
    );
    std::cerr << '\n';
    write_diagnostic_label("Status", use_color);
    write_styled(
        std::cerr,
        use_color,
        clean ? ansi::green : ansi::yellow,
        clean ? "Clean" : "Degraded"
    );
    if (clean) {
        std::cerr << '\n';
    } else if (stats.frames_decoded != 0U) {
        std::cerr << " - damage detected; valid frames recovered\n";
    } else {
        std::cerr << " - damage detected; no valid frames recovered\n";
    }

    write_diagnostic_label("Input", use_color);
    std::cerr << (file_input ? "file" : "standard input") << '\n';
    write_diagnostic_label("Bytes", use_color);
    std::cerr << stats.bytes_received << " received\n";
    write_diagnostic_label("Frames", use_color);
    std::cerr << stats.frames_decoded << " decoded\n";
    write_diagnostic_label("Frame mix", use_color);
    std::cerr << counts.heartbeat << " heartbeat / " << counts.imu_sample
              << " IMU / " << counts.motor_status << " motor\n";
    write_diagnostic_label("Framing", use_color);
    std::cerr << stats.noise_bytes << " noise / " << stats.invalid_versions
              << " invalid version / " << stats.unknown_types << " unknown type / "
              << stats.invalid_lengths << " invalid length\n";
    write_diagnostic_label("Integrity", use_color);
    std::cerr << stats.crc_failures << " CRC failure / " << stats.payload_failures
              << " payload failure / " << stats.truncated_frames << " truncated ("
              << stats.truncated_bytes << " bytes)\n";
    write_diagnostic_label("Sequence", use_color);
    std::cerr << stats.sequence_gaps << " gaps / " << stats.missing_frames
              << " missing / " << stats.out_of_order_frames << " out of order\n";
}

[[nodiscard]] bool flush_output() {
    std::cout.flush();
    if (std::cout) {
        return true;
    }

    std::cerr << "telemetry-link: output write failed\n";
    return false;
}

void print_usage(std::ostream& output, const char* program) {
    output << "Usage: ";
    write_visible(output, program);
    output << " [OPTIONS] [--] [FILE|-]\n\n"
           << "Decode TelemetryLink protocol v1 frames from a file or standard input.\n"
           << "Arbitrary chunks, damaged frames, and sequence gaps are handled as\n"
           << "stream-parser concerns. FILE defaults to standard input.\n\n"
           << "Streams:\n"
           << "  stdout  One JSON object per decoded frame, then one health object.\n"
           << "  stderr  Errors and the opt-in human diagnostic summary.\n\n"
           << "Options:\n"
           << "  --diagnostics       Write a compact link-health summary to stderr.\n"
           << "  --color MODE        Diagnostic ANSI color: auto, always, or never.\n"
           << "  --fail-on-degraded  Exit 3 after writing health if damage was detected.\n"
           << "  -h, --help          Show this help text.\n\n"
           << "NO_COLOR disables diagnostic ANSI color, including --color always.\n\n"
           << "Examples:\n  ";
    write_visible(output, program);
    output << " capture.bin\n  producer | ";
    write_visible(output, program);
    output << " --diagnostics | json-consumer\n\n"
           << "Exit status: 0 complete, 1 I/O failure, 2 usage,\n"
           << "             3 degraded with --fail-on-degraded.\n";
}

[[nodiscard]] bool parse_color_mode(std::string_view value, ColorMode& mode) {
    if (value == "auto") {
        mode = ColorMode::automatic;
        return true;
    }
    if (value == "always") {
        mode = ColorMode::always;
        return true;
    }
    if (value == "never") {
        mode = ColorMode::never;
        return true;
    }
    std::cerr << "telemetry-link: invalid color mode '";
    write_visible(std::cerr, value);
    std::cerr << "'; expected auto, always, or never\n";
    return false;
}

[[nodiscard]] bool parse_options(
    int argc,
    char* argv[],
    CliOptions& options,
    bool& show_help
) {
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (!positional_only && argument == "--") {
            positional_only = true;
            continue;
        }
        if (!positional_only && (argument == "-h" || argument == "--help")) {
            show_help = true;
            continue;
        }
        if (!positional_only && argument == "--fail-on-degraded") {
            options.fail_on_degraded = true;
            continue;
        }
        if (!positional_only && argument == "--diagnostics") {
            options.diagnostics = true;
            continue;
        }
        if (!positional_only && argument == "--color") {
            if (index + 1 >= argc) {
                std::cerr << "telemetry-link: --color requires auto, always, or never\n";
                return false;
            }
            ++index;
            if (!parse_color_mode(argv[index], options.color_mode)) {
                return false;
            }
            continue;
        }
        if (!positional_only && argument.starts_with("--color=")) {
            if (!parse_color_mode(argument.substr(8U), options.color_mode)) {
                return false;
            }
            continue;
        }
        if (!positional_only && argument.size() > 1U && argument.front() == '-') {
            std::cerr << "telemetry-link: unknown option '";
            write_visible(std::cerr, argument);
            std::cerr << "'\n";
            return false;
        }
        if (options.input_path != nullptr) {
            std::cerr << "telemetry-link: too many input files\n";
            return false;
        }
        options.input_path = argv[index];
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout.imbue(std::locale::classic());

    CliOptions options;
    bool show_help = false;
    if (!parse_options(argc, argv, options, show_help)) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }
    if (show_help) {
        print_usage(std::cout, argv[0]);
        return flush_output() ? 0 : 1;
    }

    std::ifstream file;
    std::istream* input = &std::cin;
    if (options.input_path != nullptr && std::string_view{options.input_path} != "-") {
        file.open(options.input_path, std::ios::binary);
        if (!file) {
            std::cerr << "telemetry-link: cannot open '";
            write_visible(std::cerr, options.input_path);
            std::cerr << "'\n";
            return 1;
        }
        input = &file;
    } else {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
            std::cerr << "telemetry-link: cannot set standard input to binary mode\n";
            return 1;
        }
#endif
    }

    telemetry_link::StreamParser parser;
    TypeCounts counts;
    std::array<std::uint8_t, 4096> chunk{};

    for (;;) {
        // istream::read() waits for the complete requested size on a live pipe.
        // Block for one byte, then take everything already buffered so short
        // telemetry bursts can be decoded before the producer closes the stream.
        char first_byte{};
        if (!input->get(first_byte)) {
            if (input->bad() || !input->eof()) {
                std::cerr << "telemetry-link: input read failed\n";
                return 1;
            }
            break;
        }

        chunk.front() = static_cast<std::uint8_t>(
            static_cast<unsigned char>(first_byte)
        );
        const auto additional_bytes = input->readsome(
            reinterpret_cast<char*>(chunk.data() + 1U),
            static_cast<std::streamsize>(chunk.size() - 1U)
        );
        const auto bytes_read = 1U + static_cast<std::size_t>(additional_bytes);
        const auto decoded = parser.feed(std::span<const std::uint8_t>{
            chunk.data(),
            bytes_read,
        });
        for (const auto& frame : decoded) {
            write_frame(frame, counts);
        }
        if (!decoded.empty() && !flush_output()) {
            return 1;
        }

        if (input->bad()) {
            std::cerr << "telemetry-link: input read failed\n";
            return 1;
        }
    }

    parser.finish();
    write_health(parser.stats(), counts);
    if (!flush_output()) {
        return 1;
    }
    if (options.diagnostics) {
        write_diagnostics(
            parser.stats(),
            counts,
            options.input_path != nullptr &&
                std::string_view{options.input_path} != "-",
            color_enabled(options.color_mode)
        );
    }
    return options.fail_on_degraded && !is_clean(parser.stats()) ? 3 : 0;
}
