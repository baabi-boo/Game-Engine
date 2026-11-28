#include <ge/assets/catalog.hpp>

#include "test_support.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace {

using namespace ge::assets;

void type_change_retains_previous_resource_until_replacement_fits() {
    ge::io::VirtualFileSystem filesystem;
    (void)filesystem.mount_memory({"/assets", 0, true});
    filesystem.write_text("/assets/value", "data");
    AssetCatalog catalog(filesystem, {.max_source_bytes = 64, .max_resource_bytes = 64});
    ResourceCache cache({.budget_bytes = 8, .max_resources = 4, .max_resource_bytes = 64});
    unsigned decodes{};
    bool fail_decode = true;
    AssetTypeDescriptor converted;
    converted.id = 2;
    converted.name = "converted";
    converted.maximum_source_bytes = 64;
    converted.maximum_resource_bytes = 8;
    converted.decode = [&](const ImportContext& context) {
        ++decodes;
        if (fail_decode) {
            throw std::runtime_error("replacement decode failed");
        }
        ResourcePayload payload;
        payload.bytes.assign(context.source.begin(), context.source.end());
        payload.format = "converted";
        return payload;
    };
    catalog.register_type(std::move(converted));

    AssetDefinition definition;
    definition.source_path = "/assets/value";
    const auto asset = catalog.add(std::move(definition));
    const auto previous = catalog.load(asset, cache);
    const auto previous_revision = catalog.inspect(asset)->revision;
    CHECK(catalog.inspect(asset)->last_good_type == binary_asset_type);
    definition = catalog.inspect(asset)->definition;
    definition.type = 2;
    catalog.update(std::move(definition));

    CHECK_THROWS_AS(catalog.load(asset, cache), std::runtime_error);
    CHECK(cache.inspect(previous));
    CHECK(decodes == 0);
    CHECK(cache.bytes(previous).size() == 4);
    CHECK(cache.bytes(previous).front() == std::byte{'d'});
    CHECK(cache.inspect(previous)->pins == 0);
    CHECK(cache.statistics().pending_bytes == 0);
    CHECK(catalog.inspect(asset)->last_good_revision == previous_revision);
    CHECK(catalog.inspect(asset)->last_good_type == binary_asset_type);

    CHECK(cache.set_budget(12));
    fail_decode = false;
    const auto replacement = catalog.load(asset, cache);
    CHECK(replacement.type == 2);
    CHECK(decodes == 1);
    CHECK(catalog.inspect(asset)->last_good_revision == catalog.inspect(asset)->revision);
    CHECK(catalog.inspect(asset)->last_good_type == 2);
    CHECK(cache.inspect(previous));
    CHECK(cache.inspect(previous)->pins == 0);
    CHECK(cache.inspect(replacement)->pins == 0);

    const auto saved = catalog.encode();
    catalog.restore(saved);
    CHECK(!catalog.inspect(asset)->last_good_revision);
    CHECK(catalog.inspect(asset)->last_good_type == 0);
}

} // namespace

int main() {
    return ge::test::run("asset_catalog_audit", {
        {"type_change_retains_previous_resource_until_replacement_fits",
         type_change_retains_previous_resource_until_replacement_fits},
    });
}
