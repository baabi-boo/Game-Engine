#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>

#include "test_support.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto root = fs::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            path_ = root / ("ge-io-audit-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("cannot create test directory: " + error.message());
            }
        }
        throw std::runtime_error("cannot reserve test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        (void)fs::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string utf8(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

bool make_file_symlink(const fs::path& target, const fs::path& link) {
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (error == std::errc::operation_not_permitted ||
        error == std::errc::permission_denied || error == std::errc::not_supported) {
        std::cout << "[SKIP] native symlink creation unavailable: " << error.message() << '\n';
        return false;
    }
    CHECK(!error);
    return true;
}

void utf8_native_paths_and_mount_commands() {
    TemporaryDirectory temporary;
    const auto native_root = temporary.path() / fs::path(u8"r\u00e9pertoire-\u65e5");
    CHECK(fs::create_directory(native_root));

    ge::core::Kernel kernel;
    CHECK(kernel.config().set("io.root", utf8(native_root)));
    CHECK(kernel.config().set("io.writable_root", utf8(native_root)));
    ge::io::register_filesystem(kernel);
    auto& filesystem = kernel.services().get<ge::io::VirtualFileSystem>();
    const auto relative = fs::path(u8"caf\u00e9") / fs::path(u8"\u65e5\u672c.txt");
    const auto suffix = "/" + utf8(relative);
    filesystem.write_text("/user" + suffix, "UTF-8 filename", {true, true, {}});
    CHECK(fs::is_regular_file(native_root / relative));
    CHECK(filesystem.read_text("/assets" + suffix) == "UTF-8 filename");
    CHECK(filesystem.list("/assets", {true, true, 8}).size() == 2);

    const std::array<std::string, 2> arguments{utf8(native_root), "/command"};
    CHECK(kernel.command("io.mount", arguments).success);
    CHECK(filesystem.read_text("/command" + suffix) == "UTF-8 filename");
    CHECK(filesystem.remove("/user" + suffix));
    CHECK(!fs::exists(native_root / relative));
}

void removing_native_alias_preserves_target() {
    TemporaryDirectory temporary;
    ge::io::VirtualFileSystem filesystem;
    (void)filesystem.mount_directory(temporary.path(), {"/files", 0, true});
    filesystem.write_text("/files/target.txt", "preserved target");
    const auto alias = temporary.path() / "alias.txt";
    if (!make_file_symlink("target.txt", alias)) {
        return;
    }

    CHECK(filesystem.read_text("/files/alias.txt") == "preserved target");
    CHECK(filesystem.remove("/files/alias.txt"));
    CHECK(!fs::is_symlink(alias));
    CHECK(!filesystem.exists("/files/alias.txt"));
    CHECK(filesystem.read_text("/files/target.txt") == "preserved target");
    CHECK(fs::is_regular_file(temporary.path() / "target.txt"));
    CHECK(filesystem.remove("/files/target.txt"));
    CHECK(!filesystem.remove("/files/target.txt"));
}

void removing_alias_outside_mount_is_rejected() {
    TemporaryDirectory temporary;
    const auto native_root = temporary.path() / "mount";
    CHECK(fs::create_directory(native_root));
    const auto outside = temporary.path() / "outside.txt";
    {
        std::ofstream output(outside, std::ios::binary);
        output << "outside target";
        output.close();
        CHECK(output.good());
    }
    const auto alias = native_root / "escape.txt";
    if (!make_file_symlink(fs::path("..") / "outside.txt", alias)) {
        return;
    }
    ge::io::VirtualFileSystem filesystem;
    (void)filesystem.mount_directory(native_root, {"/files", 0, true});
    try {
        (void)filesystem.remove("/files/escape.txt");
        CHECK(false);
    } catch (const ge::io::IoException& error) {
        CHECK(error.code() == ge::io::IoError::outside_mount);
    }
    CHECK(fs::is_symlink(alias));
    CHECK(fs::is_regular_file(outside));
    CHECK(fs::file_size(outside) == 14);
}

} // namespace

int main() {
    return ge::test::run("io_audit", {
        {"utf8_native_paths_and_mount_commands", utf8_native_paths_and_mount_commands},
        {"removing_native_alias_preserves_target", removing_native_alias_preserves_target},
        {"removing_alias_outside_mount_is_rejected", removing_alias_outside_mount_is_rejected},
    });
}
