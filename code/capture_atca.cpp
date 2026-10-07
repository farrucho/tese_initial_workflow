#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

/*
 * capture_atca.cpp
 * =================
 *
 * Este programa guarda no disco as amostras raw produzidas pela placa ATCA.
 * O percurso dos dados é:
 *
 *   sinal analógico
 *       -> ADCs da placa (16 canais, códigos signed de 18 bits)
 *       -> FPGA (agrupa os canais e acrescenta informação de diagnóstico)
 *       -> controlador DMA da FPGA (transfere blocos para a RAM do computador)
 *       -> driver Linux (/dev/atca_v6_N)
 *       -> read() deste programa
 *       -> ficheiro .bin
 *
 * DMA significa Direct Memory Access. A FPGA escreve blocos diretamente na
 * memória do computador. O programa não lê cada amostra individualmente: cada
 * read() recebe um buffer DMA completo, que contém milhares de instantes dos
 * 16 canais.
 *
 * O ficheiro guarda exatamente as palavras int32 recebidas. O código ADC de
 * 18 bits encontra-se nos bits superiores e pode ser extraído com um shift
 * aritmético:
 *
 *   adc_code = fpga_word >> 14
 *
 * Os 14 bits inferiores podem conter zeros ou informação de diagnóstico da
 * FPGA. Por isso não são removidos durante a captura: preservamos o raw e
 * deixamos a interpretação para as ferramentas de análise.
 *
 * O programa controla a placa através de ioctl(). Um ioctl é um comando
 * específico do driver, diferente de uma leitura normal de ficheiro. A ordem
 * usada aqui é:
 *
 *   1. consultar o estado e recusar se a placa já estiver ocupada;
 *   2. reinicializar a fila DMA;
 *   3. ativar IRQ, se ainda não estiver ativa;
 *   4. ativar a aquisição;
 *   5. enviar um software trigger;
 *   6. ler buffers DMA completos e escrevê-los no disco;
 *   7. parar apenas os mecanismos que este processo ativou.
 *
 * O documento docs/CAPTURE_FLOW.md explica este percurso com mais detalhe.
 */

namespace {
// Características do formato produzido pelo firmware atualmente instalado.
// A taxa é por canal: em cada segundo existem 2 milhões de instantes, e cada
// instante contém uma palavra int32 para cada um dos 16 canais.
constexpr std::uint64_t kAdcRateHz = 2'000'000;
constexpr std::uint64_t kChannels = 16;

// Este é um limite deliberado do capturador, não uma exigência do driver.
// Evita aquisições demasiado curtas para serem úteis e mantém a duração mínima
// observada de 65,536 ms (16 * 4,096 ms por buffer).
constexpr std::uint64_t kMinimumBuffers = 16;

// ABI do driver: estes números têm de coincidir exatamente com os _IO/_IOR do
// header atca-v6-pcie-ioctl.h usado pelo módulo do kernel.
//
// _IO  : comando sem valor devolvido através de um ponteiro.
// _IOR : o driver escreve um valor no argumento fornecido pelo programa.
constexpr unsigned char kMagic = 'k';
constexpr unsigned long kIrqEnable = _IO(kMagic, 1);
constexpr unsigned long kIrqDisable = _IO(kMagic, 2);
constexpr unsigned long kAcqEnable = _IO(kMagic, 3);
constexpr unsigned long kAcqDisable = _IO(kMagic, 4);
constexpr unsigned long kDmaDisable = _IO(kMagic, 6);
constexpr unsigned long kSoftTrigger = _IO(kMagic, 7);
constexpr unsigned long kGetStatus = _IOR(kMagic, 8, std::uint32_t);
constexpr unsigned long kGetDmaSize = _IOR(kMagic, 11, std::uint32_t);
constexpr unsigned long kDmaReset = _IO(kMagic, 12);
constexpr unsigned long kGetChopper = _IOR(kMagic, 20, std::uint32_t);
constexpr unsigned long kGetControl = _IOR(kMagic, 26, std::uint32_t);

// Bits dos registos de controlo/estado da FPGA. Antes de começar, usamos estes
// bits para confirmar que não existe MARTe ou outra aquisição a usar a placa.
constexpr std::uint32_t kChopperOnBit = 1U << 10;
constexpr std::uint32_t kStreamBit = 1U << 20;
constexpr std::uint32_t kAcquisitionBit = 1U << 23;
constexpr std::uint32_t kSoftwareTriggerBit = 1U << 24;
constexpr std::uint32_t kDmaBit = 1U << 27;
constexpr std::uint32_t kDmaResetBit = 1U << 28;
constexpr std::uint32_t kIrqBit = 1U << 30;
constexpr std::uint32_t kAcquisitionOnStatusBit = 1U << 12;

// O signal handler só altera uma variável segura para sinais. A limpeza real
// é feita no fluxo normal do programa pela AcquisitionGuard.
volatile std::sig_atomic_t stop_requested = 0;
void on_signal(int) { stop_requested = 1; }

// Opções fornecidas na linha de comandos. A placa 9 é a usada neste projeto.
struct Options { unsigned board = 9; double duration = 0.0; std::string output; };

void usage(const char *program) {
    std::cout << "Usage: " << program << " -t SECONDS -o FILE.bin [-b BOARD]\n"
              << "Captures all 16 raw int32 channels at 2 MSPS using software trigger.\n"
              << "Duration is rounded up to a complete DMA buffer.\n";
}

bool parse_options(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "-h" || option == "--help") { usage(argv[0]); std::exit(0); }
        if ((option != "-t" && option != "-o" && option != "-b") || i + 1 >= argc) {
            std::cerr << "Unknown option or missing value: " << option << "\n"; return false;
        }
        const char *value = argv[++i];
        if (option == "-o") options.output = value;
        else if (option == "-t") {
            char *end = nullptr;
            options.duration = std::strtod(value, &end);
            if (end == value || *end != '\0' || !std::isfinite(options.duration) ||
                options.duration <= 0.0 || options.duration > 3600.0) {
                std::cerr << "Duration must be greater than zero and at most 3600 seconds.\n"; return false;
            }
        } else {
            char *end = nullptr;
            const unsigned long board = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || board > 255) {
                std::cerr << "Invalid board number: " << value << "\n"; return false;
            }
            options.board = static_cast<unsigned>(board);
        }
    }
    if (options.duration == 0.0 || options.output.empty()) {
        std::cerr << "Both -t SECONDS and -o FILE.bin are required.\n"; return false;
    }
    return true;
}

// Pequenos wrappers para ioctl(): a sobrecarga com void* é usada nos comandos
// que devolvem um uint32; a outra é usada nos comandos enable/disable/trigger.
bool ioctl_ok(int fd, unsigned long request, void *argument, const char *name) {
    if (::ioctl(fd, request, argument) == 0) return true;
    std::cerr << name << " failed: " << std::strerror(errno) << "\n"; return false;
}
bool ioctl_ok(int fd, unsigned long request, const char *name) {
    if (::ioctl(fd, request) == 0) return true;
    std::cerr << name << " failed: " << std::strerror(errno) << "\n"; return false;
}

/*
 * Garante a reposição do estado da placa quando saímos normalmente, ocorre um
 * erro ou o utilizador carrega em Ctrl+C.
 *
 * A classe regista apenas aquilo que ESTE programa ativou. Por exemplo, se a
 * IRQ já estava ligada antes da captura, irq_ fica false e finish() não a
 * desliga. Isto evita modificar estado que possa pertencer a outro serviço.
 *
 * O ioctl de AcqDisable devolve o máximo de buffers que estiveram pendentes.
 * Esse valor ajuda a perceber se o programa esteve atrasado a consumir DMA.
 */
class AcquisitionGuard {
public:
    explicit AcquisitionGuard(int fd) : fd_(fd) {}
    void irq_enabled() { irq_ = true; }
    void acquisition_enabled() { acquisition_ = true; }
    int finish() {
        if (finished_) return max_pending_;
        ::ioctl(fd_, kDmaDisable);
        if (acquisition_) max_pending_ = ::ioctl(fd_, kAcqDisable);
        if (irq_) ::ioctl(fd_, kIrqDisable);
        finished_ = true;
        return max_pending_;
    }
    ~AcquisitionGuard() { finish(); }
private:
    int fd_; bool irq_ = false; bool acquisition_ = false; bool finished_ = false; int max_pending_ = -1;
};

// write() pode escrever menos bytes do que pedimos, mesmo sem ser um erro.
// Esta função insiste até escrever o bloco completo ou encontrar um erro real.
bool write_all(int fd, const unsigned char *data, std::size_t size) {
    while (size != 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written; size -= static_cast<std::size_t>(written);
    }
    return true;
}

// As duas funções seguintes só servem para construir JSON válido e colocar um
// timestamp UTC nos metadados; não participam na aquisição da placa.
std::string json_escape(const std::string &text) {
    std::ostringstream result;
    for (const unsigned char c : text) {
        if (c == '\\' || c == '"') result << '\\' << c;
        else if (c >= 0x20) result << c;
        else result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    }
    return result.str();
}
std::string utc_now() {
    const std::time_t now = std::time(nullptr); std::tm value{}; gmtime_r(&now, &value);
    char buffer[32]; std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value); return buffer;
}

// Escreve a descrição necessária para interpretar o .bin no futuro. O JSON
// não contém amostras; contém formato, taxa, duração, estado inicial e contagens.
bool write_metadata(int fd, const Options &options, const std::string &device,
                    std::uint32_t control, std::uint32_t status, std::uint32_t chopper,
                    std::uint32_t dma_size, std::uint64_t wanted_buffers,
                    std::uint64_t captured_buffers, bool complete, double wall_time,
                    int max_pending_buffers) {
    struct utsname system_info{}; ::uname(&system_info);
    const std::uint64_t samples_per_buffer = dma_size / (kChannels * sizeof(std::int32_t));
    const std::uint64_t samples = captured_buffers * samples_per_buffer;
    std::ostringstream json;
    json << std::fixed << std::setprecision(9) << "{\n"
         << "  \"format_version\": 1,\n"
         << "  \"created_utc\": \"" << utc_now() << "\",\n"
         << "  \"device\": \"" << json_escape(device) << "\",\n"
         << "  \"output_file\": \"" << json_escape(options.output) << "\",\n"
         << "  \"complete\": " << (complete ? "true" : "false") << ",\n"
         << "  \"requested_duration_s\": " << options.duration << ",\n"
         << "  \"captured_duration_s\": " << static_cast<double>(samples) / kAdcRateHz << ",\n"
         << "  \"wall_time_s\": " << wall_time << ",\n"
         << "  \"adc_rate_hz\": " << kAdcRateHz << ",\n"
         << "  \"channels\": " << kChannels << ",\n"
         << "  \"sample_type\": \"int32 little-endian\",\n"
         << "  \"layout\": \"sample-major interleaved: ch00..ch15\",\n"
         << "  \"fpga_left_shift_bits\": 14,\n"
         << "  \"dma_buffer_bytes\": " << dma_size << ",\n"
         << "  \"samples_per_channel_per_buffer\": " << samples_per_buffer << ",\n"
         << "  \"requested_buffers\": " << wanted_buffers << ",\n"
         << "  \"captured_buffers\": " << captured_buffers << ",\n"
         << "  \"samples_per_channel\": " << samples << ",\n"
         << "  \"data_bytes\": " << captured_buffers * dma_size << ",\n"
         << "  \"driver_max_pending_buffers\": " << max_pending_buffers << ",\n"
         << "  \"software_trigger\": true,\n"
         << "  \"chopper_enabled_initially\": " << ((control & kChopperOnBit) ? "true" : "false") << ",\n"
         << "  \"chopper_counters_hex\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << chopper << "\",\n"
         << "  \"initial_control_hex\": \"0x" << std::setw(8) << control << "\",\n"
         << "  \"initial_status_hex\": \"0x" << std::setw(8) << status << "\",\n" << std::dec
         << "  \"kernel\": \"" << json_escape(system_info.release) << "\"\n}\n";
    const std::string contents = json.str();
    return write_all(fd, reinterpret_cast<const unsigned char *>(contents.data()), contents.size());
}
} // namespace

int main(int argc, char **argv) {
    // 1) Interpretar argumentos e preparar uma saída limpa com Ctrl+C/SIGTERM.
    Options options;
    if (!parse_options(argc, argv, options)) { usage(argv[0]); return 2; }
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);

    // O_CREAT|O_EXCL usado mais abaixo também impede overwrite. Esta verificação
    // antecipada permite dar uma mensagem mais clara antes de tocar na placa.
    const std::string metadata_path = options.output + ".json";
    if (::access(options.output.c_str(), F_OK) == 0 || ::access(metadata_path.c_str(), F_OK) == 0) {
        std::cerr << "Output or metadata file already exists; refusing to overwrite it.\n"; return 2;
    }

    // 2) Abrir o char device criado pelo driver. O_RDONLY significa que os dados
    // chegam através de read(); os ioctl continuam disponíveis neste descritor.
    const std::string device = "/dev/atca_v6_" + std::to_string(options.board);
    const int device_fd = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (device_fd < 0) { std::cerr << "Could not open " << device << ": " << std::strerror(errno) << "\n"; return 1; }
    // Consultas sem alteração de estado. dma_size vem do driver e determina o
    // tamanho exato que cada read() tem de pedir.
    std::uint32_t control = 0, status = 0, dma_size = 0, chopper = 0;
    const bool state_ok = ioctl_ok(device_fd, kGetControl, &control, "get control") &&
        ioctl_ok(device_fd, kGetStatus, &status, "get status") &&
        ioctl_ok(device_fd, kGetDmaSize, &dma_size, "get DMA size") &&
        ioctl_ok(device_fd, kGetChopper, &chopper, "get chopper counters");
    if (!state_ok) { ::close(device_fd); return 1; }
    // Uma IRQ já ativa é aceitável enquanto aquisição e DMA estiverem parados.
    // Guardamos essa informação para a preservar no fim.
    const bool irq_was_enabled = (control & kIrqBit) != 0;

    // Não iniciamos nada se stream RT, aquisição, trigger ou DMA já estiverem
    // ativos. Isso protege uma eventual aplicação MARTe ou outra aquisição.
    const std::uint32_t active_mask = kStreamBit | kAcquisitionBit |
                                      kSoftwareTriggerBit | kDmaBit | kDmaResetBit;
    if ((control & active_mask) != 0 || (status & kAcquisitionOnStatusBit) != 0) {
        std::cerr << "Board is not idle; refusing active raw capture (control=0x" << std::hex << control
                  << ", status=0x" << status << ").\n"; ::close(device_fd); return 3;
    }
    // Um instante ocupa 16 canais * 4 bytes = 64 bytes. Um buffer válido tem de
    // conter um número inteiro desses instantes e ter um tamanho razoável.
    if (dma_size == 0 || dma_size % (kChannels * sizeof(std::int32_t)) != 0 || dma_size > 64U * 1024U * 1024U) {
        std::cerr << "Invalid DMA buffer size reported by driver: " << dma_size << " bytes.\n"; ::close(device_fd); return 1;
    }

    // 3) Converter a duração pedida num número inteiro de buffers DMA.
    // Não podemos pedir uma fração de buffer; por isso a duração efetiva é
    // arredondada para cima. Com 512 KiB: 8192 amostras/canal = 4,096 ms.
    const std::uint64_t samples_per_buffer = dma_size / (kChannels * sizeof(std::int32_t));
    const long double requested_samples = std::ceil(static_cast<long double>(options.duration) * kAdcRateHz);
    const std::uint64_t buffers = std::max(kMinimumBuffers, static_cast<std::uint64_t>(
        std::ceil(requested_samples / samples_per_buffer)));
    if (buffers == 0 || buffers > std::numeric_limits<std::uint64_t>::max() / dma_size) {
        std::cerr << "Requested capture is too large.\n"; ::close(device_fd); return 2;
    }
    // Verificar antecipadamente o espaço livre evita iniciar uma aquisição que
    // sabemos que não poderá ser guardada completamente.
    const std::uint64_t output_bytes = buffers * dma_size;
    const std::size_t slash = options.output.find_last_of('/');
    const std::string output_directory = slash == std::string::npos ? "." :
        (slash == 0 ? "/" : options.output.substr(0, slash));
    struct statvfs filesystem{};
    if (::statvfs(output_directory.c_str(), &filesystem) == 0) {
        const std::uint64_t available = static_cast<std::uint64_t>(filesystem.f_bavail) * filesystem.f_frsize;
        if (available < output_bytes + dma_size) {
            std::cerr << "Insufficient free disk space: need " << output_bytes << " bytes, have "
                      << available << " bytes.\n"; ::close(device_fd); return 1;
        }
    }

    // 4) Criar simultaneamente o binário e o JSON. O_EXCL recusa ficheiros já
    // existentes, evitando destruir acidentalmente uma captura anterior.
    const int output_fd = ::open(options.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (output_fd < 0) { std::cerr << "Could not create output: " << std::strerror(errno) << "\n"; ::close(device_fd); return 1; }
    const int metadata_fd = ::open(metadata_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (metadata_fd < 0) {
        std::cerr << "Could not create metadata: " << std::strerror(errno) << "\n";
        ::close(output_fd); ::unlink(options.output.c_str()); ::close(device_fd); return 1;
    }

    const double exact_duration = static_cast<double>(buffers * samples_per_buffer) / kAdcRateHz;
    const double size_mib = static_cast<double>(buffers * dma_size) / (1024.0 * 1024.0);
    std::cout << "Raw capture: " << device << ", 16 channels at 2 MSPS\nRequested " << options.duration
              << " s; capturing " << exact_duration << " s in " << buffers << " DMA buffers (" << size_mib << " MiB).\n";

    // 5) Preparar um único buffer em RAM. Ele é reutilizado em todas as leituras;
    // a captura completa nunca precisa de caber na memória do processo.
    AcquisitionGuard guard(device_fd); std::vector<unsigned char> buffer(dma_size);

    // Sequência ativa da placa:
    //   DmaReset    limpa/realinha a fila DMA;
    //   IrqEnable   permite ao driver ser avisado quando um buffer fica pronto;
    //   AcqEnable   arma a lógica de aquisição da FPGA;
    //   SoftTrigger define o instante inicial da captura.
    std::uint64_t captured = 0; bool capture_ok = ioctl_ok(device_fd, kDmaReset, "DMA reset");
    if (capture_ok && !irq_was_enabled) {
        capture_ok = ioctl_ok(device_fd, kIrqEnable, "enable IRQ");
        if (capture_ok) guard.irq_enabled();
    }
    if (capture_ok) { capture_ok = ioctl_ok(device_fd, kAcqEnable, "enable acquisition"); if (capture_ok) guard.acquisition_enabled(); }
    if (capture_ok) capture_ok = ioctl_ok(device_fd, kSoftTrigger, "software trigger");
    const auto wall_start = std::chrono::steady_clock::now();

    // 6) Cada read() bloqueia até o driver disponibilizar um buffer DMA completo.
    // Dentro do buffer, os int32 estão intercalados assim:
    //   ch00, ch01, ..., ch15, ch00, ch01, ..., ch15, ...
    // Exigimos sempre o tamanho completo; uma leitura curta seria ambígua e é
    // tratada como erro em vez de produzir silenciosamente um ficheiro corrupto.
    while (capture_ok && !stop_requested && captured < buffers) {
        const ssize_t received = ::read(device_fd, buffer.data(), buffer.size());
        if (received < 0 && errno == EINTR && !stop_requested) continue;
        if (received != static_cast<ssize_t>(buffer.size())) {
            if (received < 0) std::cerr << "DMA read failed: " << std::strerror(errno) << "\n";
            else std::cerr << "DMA read returned " << received << " bytes; expected " << buffer.size() << ".\n";
            capture_ok = false; break;
        }
        if (!write_all(output_fd, buffer.data(), buffer.size())) {
            std::cerr << "Output write failed: " << std::strerror(errno) << "\n"; capture_ok = false; break;
        }
        ++captured;
    }
    // 7) Parar DMA/aquisição antes de fazer fsync. finish() é idempotente e o
    // destrutor funciona como segunda proteção caso alguma saída futura mude.
    const int max_pending = guard.finish();
    const double wall_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    if (::fsync(output_fd) != 0) { std::cerr << "Could not flush output: " << std::strerror(errno) << "\n"; capture_ok = false; }
    ::close(output_fd);
    // O ficheiro parcial é mantido após erro ou Ctrl+C. O campo complete no JSON
    // permite às ferramentas distinguir uma captura completa de uma parcial.
    const bool complete = capture_ok && !stop_requested && captured == buffers;
    if (!write_metadata(metadata_fd, options, device, control, status, chopper, dma_size,
                        buffers, captured, complete, wall_time, max_pending) || ::fsync(metadata_fd) != 0) {
        std::cerr << "Could not write metadata completely.\n"; capture_ok = false;
    }
    ::close(metadata_fd); ::close(device_fd);
    std::cout << "Captured " << captured << "/" << buffers << " DMA buffers: "
              << captured * samples_per_buffer << " samples/channel, "
              << static_cast<double>(captured * samples_per_buffer) / kAdcRateHz << " s.\n"
              << "Maximum pending DMA buffers reported by driver: " << max_pending << "\n"
              << "Data: " << options.output << "\nMetadata: " << metadata_path << "\n";
    if (stop_requested) return 130;
    return (capture_ok && complete) ? 0 : 1;
}
