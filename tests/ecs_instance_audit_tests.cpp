#include <ge/ecs/registry.hpp>

#include "test_support.hpp"

#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

struct Payload {
    int value{};
};

struct FragilePayload {
    inline static bool fail_next_allocation{};
    inline static bool fail_next_copy{};
    int value;

    explicit FragilePayload(int initial) : value(initial) {
        if (initial < 0) {
            throw std::runtime_error("construction failed");
        }
    }

    FragilePayload(const FragilePayload& source) : value(source.value) {
        if (std::exchange(fail_next_copy, false)) {
            throw std::runtime_error("copy failed");
        }
    }

    static void* operator new(std::size_t bytes) {
        if (std::exchange(fail_next_allocation, false)) {
            throw std::bad_alloc();
        }
        return ::operator new(bytes);
    }

    static void operator delete(void* pointer) noexcept {
        ::operator delete(pointer);
    }
};

void edits_preserve_identity_and_replacements_retire_it() {
    ge::ecs::Registry registry(4);
    const auto entity = registry.create();
    CHECK(registry.component_instance<Payload>(entity) == 0);
    CHECK(registry.component_instance<Payload>(ge::ecs::null_entity) == 0);
    CHECK(registry.statistics().component_types == 0);
    registry.emplace<Payload>(entity, Payload{11});
    const auto first = registry.component_instance<Payload>(entity);
    CHECK(first != 0);
    const auto& read_only = registry;
    CHECK(read_only.component_instance<const volatile Payload>(entity) == first);

    const auto before = registry.revision();
    registry.emplace<Payload>(entity, Payload{99});
    CHECK(registry.component_instance<Payload>(entity) == first);
    CHECK(registry.revision() == before);
    CHECK(registry.patch<Payload>(entity, [](Payload& value) { value.value = 17; }));
    CHECK(registry.mark_changed<const Payload>(entity));
    CHECK(registry.revision() > before);
    CHECK(registry.component_instance<Payload>(entity) == first);
    CHECK(registry.require<Payload>(entity).value == 17);

    registry.emplace_or_replace<const Payload>(entity, Payload{23});
    const auto replacement = registry.component_instance<Payload>(entity);
    CHECK(replacement > first);
    CHECK(registry.remove<Payload>(entity));
    CHECK(registry.component_instance<Payload>(entity) == 0);
    registry.emplace<Payload>(entity, Payload{29});
    CHECK(registry.component_instance<Payload>(entity) > replacement);
}

void clones_and_recycled_entities_receive_fresh_instances() {
    ge::ecs::Registry registry(2);
    const auto source = registry.create();
    registry.emplace<Payload>(source, Payload{31});
    const auto original = registry.component_instance<Payload>(source);
    const auto clone = registry.clone(source);
    CHECK(clone);
    const auto copied = registry.component_instance<Payload>(clone);
    CHECK(copied > original);
    CHECK(registry.component_instance<Payload>(source) == original);
    CHECK(registry.require<Payload>(clone).value == 31);

    CHECK(registry.destroy(source));
    const auto recycled = registry.create();
    CHECK(recycled.index == source.index);
    CHECK(recycled.generation != source.generation);
    registry.emplace<Payload>(recycled, Payload{37});
    const auto reused_slot = registry.component_instance<Payload>(recycled);
    CHECK(reused_slot > copied);
    CHECK(registry.component_instance<Payload>(source) == 0);

    registry.clear();
    CHECK(registry.component_instance<Payload>(clone) == 0);
    const auto after_clear = registry.create();
    registry.emplace_or_replace<Payload>(after_clear, Payload{41});
    CHECK(registry.component_instance<Payload>(after_clear) > reused_slot);
}

void failed_construction_and_allocation_preserve_the_installed_instance() {
    ge::ecs::Registry registry(3);
    const auto entity = registry.create();
    auto* original = &registry.emplace<FragilePayload>(entity, 43);
    const auto identity = registry.component_instance<FragilePayload>(entity);
    const auto before = registry.revision();
    CHECK_THROWS_AS(registry.emplace_or_replace<FragilePayload>(entity, -1), std::runtime_error);
    FragilePayload::fail_next_allocation = true;
    CHECK_THROWS_AS(registry.emplace_or_replace<FragilePayload>(entity, 47), std::bad_alloc);
    CHECK(registry.get<FragilePayload>(entity) == original);
    CHECK(original->value == 43);
    CHECK(registry.component_instance<FragilePayload>(entity) == identity);
    CHECK(registry.revision() == before);

    const auto absent = registry.create();
    const auto before_insert = registry.revision();
    CHECK_THROWS_AS(registry.emplace<FragilePayload>(absent, -1), std::runtime_error);
    FragilePayload::fail_next_allocation = true;
    CHECK_THROWS_AS(registry.emplace<FragilePayload>(absent, 53), std::bad_alloc);
    CHECK(!registry.has<FragilePayload>(absent));
    CHECK(registry.component_instance<FragilePayload>(absent) == 0);
    CHECK(registry.component_instance<FragilePayload>(entity) == identity);
    CHECK(registry.revision() == before_insert);

    FragilePayload::fail_next_copy = true;
    CHECK_THROWS_AS(registry.clone(entity), std::runtime_error);
    CHECK(registry.size() == 2);
    CHECK(registry.get<FragilePayload>(entity) == original);
    CHECK(registry.component_instance<FragilePayload>(entity) == identity);
    registry.emplace_or_replace<FragilePayload>(entity, 59);
    CHECK(registry.component_instance<FragilePayload>(entity) > identity);
    CHECK(registry.require<FragilePayload>(entity).value == 59);
}

} // namespace

int main() {
    return ge::test::run("ecs_instance_audit", {
        {"edits_and_replacements", edits_preserve_identity_and_replacements_retire_it},
        {"clone_and_entity_reuse", clones_and_recycled_entities_receive_fresh_instances},
        {"failed_publication", failed_construction_and_allocation_preserve_the_installed_instance},
    });
}
