#include <ge/config/settings.hpp>
#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>

#include "test_support.hpp"

#include <cstdint>
#include <string>

namespace {

void conflicting_kernel_aliases_preserve_settings() {
    for (const auto* alias : {"a.option", "z.option"}) {
        ge::core::Kernel kernel;
        ge::config::Settings settings(kernel);
        ge::config::SettingSchema schema;
        schema.key = "option";
        schema.kernel_key = alias;
        schema.default_value = std::int64_t{7};
        CHECK(settings.define(schema).success);
        CHECK(settings.add_layer("runtime", 1).success);
        CHECK(settings.set("runtime", "option", std::int64_t{9}).success);
        const auto revision = settings.revision();
        const auto snapshot = settings.snapshot();
        const auto configuration = kernel.config().values();

        ge::core::Configuration imported;
        CHECK(imported.set("option", "11"));
        CHECK(imported.set(alias, "12"));
        const auto rejected = settings.import_configuration("runtime", imported);
        CHECK(!rejected.success);
        CHECK(!rejected.applied);
        CHECK(settings.revision() == revision);
        CHECK(settings.snapshot() == snapshot);
        CHECK(kernel.config().values() == configuration);

        CHECK(imported.erase("option"));
        CHECK(settings.import_configuration("runtime", imported).success);
        CHECK(settings.integer("option") == 12);
        CHECK(kernel.config().text(alias) == "12");
    }
}

void format_header_obeys_line_limit() {
    ge::core::Kernel kernel;
    auto& filesystem = kernel.services().emplace<ge::io::VirtualFileSystem>();
    static_cast<void>(filesystem.mount_memory({"/settings", 0, true}));
    filesystem.write_text("/settings/retained.cfg", "retained");

    ge::config::SettingsLimits limits;
    limits.max_line_bytes = 8;
    ge::config::Settings settings(kernel, limits);
    CHECK(settings.add_layer("runtime", 1).success);
    CHECK(!settings.save_layer("runtime", "/settings/retained.cfg").success);
    CHECK(filesystem.read_text("/settings/retained.cfg") == "retained");
    CHECK(!settings.save_effective("/settings/new.cfg").success);
    CHECK(!filesystem.exists("/settings/new.cfg"));

    limits.max_line_bytes = 9;
    ge::config::Settings exact(kernel, limits);
    CHECK(exact.add_layer("runtime", 1).success);
    CHECK(exact.save_effective("/settings/exact.cfg").success);
    CHECK(filesystem.read_text("/settings/exact.cfg") == "GECONFIG1\n");
    CHECK(exact.load("runtime", "/settings/exact.cfg").success);
}

} // namespace

int main() {
    return ge::test::run("config_settings_audit", {
        {"conflicting_kernel_aliases_preserve_settings", conflicting_kernel_aliases_preserve_settings},
        {"format_header_obeys_line_limit", format_header_obeys_line_limit},
    });
}
