#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::size_t kChannels = 16;
constexpr std::size_t kBuffers = 8;
constexpr std::int32_t kAdcScale = 1 << 14;
constexpr unsigned char kIoctlMagic = 'k';
constexpr unsigned long kGetStatus = _IOR(kIoctlMagic, 8, std::uint32_t);
constexpr unsigned long kStreamEnable = _IO(kIoctlMagic, 13);
constexpr unsigned long kStreamDisable = _IO(kIoctlMagic, 14);
constexpr unsigned long kGetControl = _IOR(kIoctlMagic, 26, std::uint32_t);
constexpr std::uint32_t kStreamBit = 1U << 20;
constexpr std::uint32_t kAcquisitionBit = 1U << 23;
constexpr std::uint32_t kAcquisitionOnStatusBit = 1U << 12;

volatile std::sig_atomic_t keep_running = 1;

void stop_on_signal(int) {
    keep_running = 0;
}

std::int32_t adc_counts(std::int32_t packed_value) {
    if (packed_value >= 0) return packed_value / kAdcScale;
    const std::int64_t magnitude = -static_cast<std::int64_t>(packed_value);
    return static_cast<std::int32_t>(-((magnitude + kAdcScale - 1) / kAdcScale));
}

class StreamGuard {
public:
    explicit StreamGuard(int fd) : fd_(fd) {}
    void mark_started() { started_ = true; }
    ~StreamGuard() {
        if (started_) {
            if (::ioctl(fd_, kStreamDisable) < 0)
                std::cerr << "WARNING: could not restore StreamE=0: "
                          << std::strerror(errno) << "\n";
            else
                std::cout << "RT stream returned to its initial OFF state.\n";
        }
        ::close(fd_);
    }
private:
    int fd_;
    bool started_ = false;
};

// Exact 256-byte RT packet exported by atca_v6_stream.
struct Packet {
    std::uint32_t head_time_count;
    std::uint32_t header;
    std::int32_t adc[kChannels];
    std::uint64_t reserved0;
    std::int64_t integral[kChannels];
    std::int64_t reserved1[4];
    std::uint32_t sample_count;
    std::uint32_t reserved2;
    std::uint32_t foot_time_count;
    std::uint32_t footer;
};

static_assert(sizeof(Packet) == 256, "unexpected ATCA RT packet size");

bool copy_stable_packet(volatile Packet *source, Packet &destination) {
    const std::uint32_t before_head = source->head_time_count;
    const std::uint32_t before_foot = source->foot_time_count;
    if (before_head != before_foot) return false;

    auto *output = reinterpret_cast<std::uint32_t *>(&destination);
    volatile std::uint32_t *input = reinterpret_cast<volatile std::uint32_t *>(source);
    for (std::size_t i = 0; i < sizeof(Packet) / sizeof(std::uint32_t); ++i)
        output[i] = input[i];

    return before_head == source->head_time_count &&
           before_foot == source->foot_time_count &&
           destination.head_time_count == destination.foot_time_count;
}

bool find_newest(volatile Packet *packets, Packet &newest) {
    bool found = false;
    for (std::size_t i = 0; i < kBuffers; ++i) {
        Packet candidate{};
        if (!copy_stable_packet(&packets[i], candidate)) continue;
        if (!found || static_cast<std::int32_t>(candidate.head_time_count - newest.head_time_count) > 0) {
            newest = candidate;
            found = true;
        }
    }
    return found;
}

unsigned parse_positive(const char *text, const char *name) {
    try {
        std::size_t used = 0;
        const unsigned long value = std::stoul(text, &used);
        if (text[used] != '\0' || value == 0 || value > 1'000'000) throw std::exception{};
        return static_cast<unsigned>(value);
    } catch (...) {
        std::cerr << "Invalid " << name << ": " << text << "\n";
        std::exit(2);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::cout << "Usage: " << argv[0] << " [board=9] [readings=20] [interval_ms=500]\n";
        return 0;
    }

    const unsigned board = argc > 1 ? parse_positive(argv[1], "board") : 9;
    const unsigned readings = argc > 2 ? parse_positive(argv[2], "readings") : 20;
    const unsigned interval_ms = argc > 3 ? parse_positive(argv[3], "interval") : 500;
    if (argc > 4) {
        std::cerr << "Too many arguments. Use --help.\n";
        return 2;
    }

    const std::string device = "/dev/atca_v6_dmart_" + std::to_string(board);
    const std::string control_device = "/dev/atca_v6_" + std::to_string(board);
    const int control_fd = ::open(control_device.c_str(), O_RDONLY | O_CLOEXEC);
    if (control_fd < 0) {
        std::cerr << "Could not open " << control_device << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    StreamGuard stream_guard(control_fd);
    bool started_stream = false;
    std::uint32_t control = 0;
    std::uint32_t status = 0;
    if (::ioctl(control_fd, kGetControl, &control) < 0 ||
        ::ioctl(control_fd, kGetStatus, &status) < 0) {
        std::cerr << "Could not read board state: " << std::strerror(errno) << "\n";
        return 1;
    }
    if ((control & kAcquisitionBit) != 0 || (status & kAcquisitionOnStatusBit) != 0) {
        std::cerr << "Refusing to change streaming while an acquisition is active"
                  << " (control=0x" << std::hex << control << ", status=0x" << status << ").\n";
        return 2;
    }
    if ((control & kStreamBit) == 0) {
        if (::ioctl(control_fd, kStreamEnable) < 0) {
            std::cerr << "Could not enable the RT stream: " << std::strerror(errno) << "\n";
            return 1;
        }
        stream_guard.mark_started();
        started_stream = true;
        std::cout << "RT stream was OFF; temporarily enabled it.\n";
    } else {
        std::cout << "RT stream was already ON; it will be left ON.\n";
    }

    const int fd = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "Could not open " << device << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    void *mapping = ::mmap(nullptr, static_cast<std::size_t>(page_size),
                           PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        std::cerr << "Could not map " << device << ": " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    // The circular page can still contain packets from a previous run. At
    // 10 kHz, 5 ms is ample time for all eight 256-byte slots to be replaced.
    if (started_stream)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto *packets = static_cast<volatile Packet *>(mapping);
    std::cout << "Viewing " << device << " (no acquisition, IRQ or trigger).\n";
    std::cout << "counter    sample";
    for (std::size_t channel = 0; channel < kChannels; ++channel)
        std::cout << "       ch" << std::setw(2) << std::setfill('0') << channel;
    std::cout << std::setfill(' ') << "\n";

    std::uint32_t previous_counter = 0;
    bool have_previous = false;
    unsigned unchanged = 0;
    std::signal(SIGINT, stop_on_signal);
    std::signal(SIGTERM, stop_on_signal);
    for (unsigned line = 0; line < readings && keep_running; ++line) {
        Packet packet{};
        if (!find_newest(packets, packet)) {
            std::cout << "No complete DMA packet is currently visible.\n";
            ++unchanged;
        } else {
            const bool changed = !have_previous || packet.head_time_count != previous_counter;
            unchanged += changed ? 0 : 1;
            if (changed) unchanged = 0;
            std::cout << std::setw(10) << packet.head_time_count << " "
                      << std::setw(9) << packet.sample_count;
            for (const std::int32_t packed_value : packet.adc)
                std::cout << " " << std::setw(10) << adc_counts(packed_value);
            std::cout << (changed ? "" : "  (unchanged)") << "\n";
            previous_counter = packet.head_time_count;
            have_previous = true;
        }
        if (line + 1 < readings)
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    ::munmap(mapping, static_cast<std::size_t>(page_size));
    ::close(fd);
    if (!have_previous || unchanged + 1 >= readings) {
        std::cerr << "The RT buffer did not change after enabling streaming.\n";
        return 3;
    }
    return 0;
}
