#include <ge/world/state.hpp>

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// This executable is registered only on Linux ELF with the libstdc++ C++11 ABI
// and a linker supporting --wrap. No runtime fault hook is needed in ge_world.
namespace {

enum class Failure { none, write, flush, close, publish };

struct FaultState {
    Failure armed{Failure::none};
    bool active{};
    unsigned hits{};
    std::FILE* stream{};
    unsigned close_calls{};
    std::size_t requested_items{};
    std::size_t written_items{};
    bool failed_close_released_stream{};
};

FaultState fault;

[[nodiscard]] bool consume(Failure stage) noexcept {
    if (!fault.active || fault.armed != stage) {
        return false;
    }
    fault.armed = Failure::none;
    ++fault.hits;
    errno = EIO;
    return true;
}

class ScopedFailure final {
public:
    explicit ScopedFailure(Failure stage) noexcept {
        fault = {};
        fault.armed = stage;
        fault.active = true;
    }

    ScopedFailure(const ScopedFailure&) = delete;
    ScopedFailure& operator=(const ScopedFailure&) = delete;

    ~ScopedFailure() {
        fault.active = false;
        fault.armed = Failure::none;
    }
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            path_ = root / ("ge-state-io-faults-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("cannot create test directory: " + error.message());
            }
        }
        throw std::runtime_error("cannot reserve test directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

using ge::world::PersistentState;
using ge::world::StateEntry;

constexpr std::uint64_t baseline = 0x100000001ULL;
constexpr std::uint64_t saved_sequence = 41U;
constexpr std::uint64_t attempted_sequence = 42U;
constexpr std::array<StateEntry, 3> initial{{
    {{1U}, 10},
    {{0x100000001ULL}, 20},
    {{std::numeric_limits<std::uint64_t>::max()}, 30},
}};
constexpr std::array<StateEntry, 3> edited{{
    {initial[0].id, -101}, {initial[1].id, 202}, {initial[2].id, -303},
}};

void check_values(const PersistentState& state, std::span<const StateEntry> expected,
                  std::uint64_t sequence) {
    CHECK(state.baseline_id() == baseline);
    CHECK(state.sequence() == sequence);
    CHECK(state.size() == expected.size());
    for (const auto& entry : expected) {
        CHECK(state.get(entry.id) == entry.value);
    }
}

[[nodiscard]] std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input.is_open());
    const auto length = static_cast<std::streamoff>(input.tellg());
    CHECK(length >= 0);
    CHECK(length <= static_cast<std::streamoff>(ge::world::max_state_file_bytes));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(input.good());
    return bytes;
}

void check_only_save_remains(const std::filesystem::path& path) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        CHECK(entry.path() == path);
        CHECK(entry.is_regular_file());
        ++count;
    }
    CHECK(count == 1U);
}

void failed_save_preserves_destination_and_retries(Failure stage) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "save.gestate";
    PersistentState state(baseline, initial);
    state.save(path, saved_sequence);
    const auto original = file_bytes(path);
    const auto original_snapshot = ge::world::decode_state(original);
    CHECK(original_snapshot.baseline_id() == baseline);
    CHECK(original_snapshot.sequence() == saved_sequence);
    CHECK(std::equal(original_snapshot.entries().begin(), original_snapshot.entries().end(),
                     initial.begin(), initial.end()));
    for (const auto& entry : edited) {
        CHECK(state.set(entry.id, entry.value));
    }

    const auto observed = [&] {
        ScopedFailure injection(stage);
        CHECK_THROWS_AS(state.save(path, attempted_sequence), std::runtime_error);
        return fault;
    }();
    CHECK(observed.hits == 1U);
    CHECK(observed.armed == Failure::none);
    CHECK(observed.close_calls == 1U);
    if (stage == Failure::write) {
        CHECK(observed.written_items > 0U);
        CHECK(observed.written_items < observed.requested_items);
    }
    if (stage == Failure::close) {
        CHECK(observed.failed_close_released_stream);
    }
    CHECK(file_bytes(path) == original);
    check_values(state, edited, saved_sequence);
    check_only_save_remains(path);

    // The rejected sequence remains available: every stage gets its own retry.
    state.save(path, attempted_sequence);
    check_values(state, edited, attempted_sequence);
    const auto replacement = file_bytes(path);
    CHECK(replacement != original);
    const auto snapshot = ge::world::decode_state(replacement);
    CHECK(snapshot.baseline_id() == baseline);
    CHECK(snapshot.sequence() == attempted_sequence);
    CHECK(std::equal(snapshot.entries().begin(), snapshot.entries().end(),
                     edited.begin(), edited.end()));
    PersistentState restarted(baseline, initial);
    restarted.restore(path);
    check_values(restarted, edited, attempted_sequence);
    check_only_save_remains(path);
}

} // namespace

extern "C" std::size_t __real_fwrite(const void*, std::size_t, std::size_t, std::FILE*);
extern "C" int __real_fflush(std::FILE*);
extern "C" int __real_fclose(std::FILE*);

extern "C" std::size_t __wrap_fwrite(const void* data, std::size_t size,
                                    std::size_t count, std::FILE* stream) {
    if (fault.active && fault.stream == nullptr) {
        fault.stream = stream;
    }
    if (fault.active && fault.stream == stream && consume(Failure::write)) {
        fault.requested_items = count;
        fault.written_items = __real_fwrite(data, size, count / 2U, stream);
        errno = EIO;
        return fault.written_items;
    }
    return __real_fwrite(data, size, count, stream);
}

extern "C" int __wrap_fflush(std::FILE* stream) {
    if (fault.active && fault.stream == stream && consume(Failure::flush)) {
        return EOF;
    }
    return __real_fflush(stream);
}

extern "C" int __wrap_fclose(std::FILE* stream) {
    const bool tracked = fault.active && fault.stream == stream;
    if (tracked && ++fault.close_calls > 1U) {
        // Report a repeated close as a failed check instead of invoking libc
        // with an already released stream and making the regression undefined.
        errno = EIO;
        return EOF;
    }
    const auto result = __real_fclose(stream);
    if (tracked && consume(Failure::close)) {
        // fclose consumes the stream even when its return value reports failure.
        fault.failed_close_released_stream = result == 0;
        return EOF;
    }
    return result;
}

// The libc rename call lives inside shared libstdc++, so wrapping that C symbol
// would miss it. Wrap the libstdc++ overload directly; CMake probes this ABI.
extern "C" void real_rename(const std::filesystem::path&, const std::filesystem::path&,
                            std::error_code&) noexcept
    asm("__real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" void wrap_rename(const std::filesystem::path&, const std::filesystem::path&,
                            std::error_code&) noexcept
    asm("__wrap__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" void wrap_rename(const std::filesystem::path& from, const std::filesystem::path& to,
                            std::error_code& error) noexcept {
    if (consume(Failure::publish)) {
        error = std::make_error_code(std::errc::permission_denied);
        return;
    }
    real_rename(from, to, error);
}

int main() {
    return ge::test::run("state_io_faults", {
        {"partial fwrite preserves save and retries", [] {
             failed_save_preserves_destination_and_retries(Failure::write);
         }},
        {"fflush failure preserves save and retries", [] {
             failed_save_preserves_destination_and_retries(Failure::flush);
         }},
        {"fclose failure preserves save and retries", [] {
             failed_save_preserves_destination_and_retries(Failure::close);
         }},
        {"rename failure preserves save and retries", [] {
             failed_save_preserves_destination_and_retries(Failure::publish);
         }},
    });
}
