#include <ge/scene/scene.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ge::scene {
namespace {

[[nodiscard]] bool same_transform(const math::Transform& first,
                                   const math::Transform& second) noexcept {
    return first.position == second.position && first.rotation == second.rotation &&
           first.scale == second.scale;
}

[[nodiscard]] std::optional<math::Transform> canonical(const math::Transform& value) noexcept {
    if (!math::finite(value)) {
        return std::nullopt;
    }
    const auto norm = math::length(value.rotation);
    if (!std::isfinite(norm) || norm <= math::epsilon) {
        return std::nullopt;
    }
    auto result = value;
    if (!math::almost_equal(norm, 1.0)) {
        result.rotation = math::normalized(result.rotation);
    }
    return math::finite(math::matrix(result)) ? std::optional{result} : std::nullopt;
}

[[nodiscard]] bool valid_name(std::string_view value) noexcept {
    return value.size() <= 1024 && core::valid_utf8(value) &&
           value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool valid_tag(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 && core::valid_utf8(value) &&
           std::none_of(value.begin(), value.end(), [](char character) {
               return static_cast<unsigned char>(character) < 0x20U;
           });
}

[[nodiscard]] bool affine(const math::Mat4& value) noexcept {
    return math::finite(value) && math::almost_equal(value(3, 0), 0.0) &&
           math::almost_equal(value(3, 1), 0.0) && math::almost_equal(value(3, 2), 0.0) &&
           math::almost_equal(value(3, 3), 1.0);
}

[[nodiscard]] math::Vec3 position_of(const math::Mat4& value) noexcept {
    return {value(0, 3), value(1, 3), value(2, 3)};
}

[[nodiscard]] std::optional<math::Aabb> merge_bounds(
    const std::optional<math::Aabb>& first, const std::optional<math::Aabb>& second) noexcept {
    if (!first) {
        return second;
    }
    if (!second) {
        return first;
    }
    return math::merge(*first, *second);
}

[[nodiscard]] bool same_bounds(const std::optional<math::Aabb>& first,
                              const std::optional<math::Aabb>& second) noexcept {
    if (first.has_value() != second.has_value()) {
        return false;
    }
    return !first || (first->min == second->min && first->max == second->max);
}

[[nodiscard]] char hex_digit(unsigned int value) noexcept {
    constexpr std::string_view digits{"0123456789ABCDEF"};
    return digits[value & 15U];
}

[[nodiscard]] int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

[[nodiscard]] std::string encode_segment(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '/' || character == '%' || character == '@' || character == '.' ||
            byte < 0x20U || byte == 0x7FU) {
            result.push_back('%');
            result.push_back(hex_digit(static_cast<unsigned int>(byte) >> 4U));
            result.push_back(hex_digit(byte));
        } else {
            result.push_back(character);
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::string> decode_segment(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (value.size() - index < 3) {
            return std::nullopt;
        }
        const auto high = hex_value(value[index + 1]);
        const auto low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(high * 16 + low));
        index += 2;
    }
    return valid_name(result) ? std::optional{std::move(result)} : std::nullopt;
}

[[nodiscard]] std::optional<double> parse_number(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    double result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] Entity resolve_entity(Scene& scene, std::string_view value) {
    if (value.starts_with('/')) {
        const auto resolved = scene.find_path(value);
        return resolved ? resolved.entity : Entity{};
    }
    if (const auto parsed = ecs::parse_entity(value)) {
        return scene.contains(*parsed) ? *parsed : Entity{};
    }
    const auto named = scene.find_all_named(value, {}, 2);
    return named.size() == 1 ? named.front() : Entity{};
}

[[nodiscard]] core::CommandResult command_result(SceneResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error))};
}

} // namespace

std::string_view describe(SceneError error) noexcept {
    switch (error) {
    case SceneError::None: return "completed";
    case SceneError::InvalidEntity: return "entity is invalid or no longer alive";
    case SceneError::NotInScene: return "entity has not been adopted by the scene";
    case SceneError::InvalidParent: return "parent is not a live scene node";
    case SceneError::Cycle: return "parent change would introduce a hierarchy cycle";
    case SceneError::DepthLimit: return "hierarchy depth exceeds the configured limit";
    case SceneError::Capacity: return "scene or registry entity capacity is exhausted";
    case SceneError::InvalidTransform: return "transform is nonfinite or rotation is degenerate";
    case SceneError::SingularParent: return "parent world matrix is not invertible";
    case SceneError::UnrepresentableTransform: return "requested local matrix cannot be represented as TRS";
    case SceneError::InvalidBounds: return "bounds must have finite ordered endpoints";
    case SceneError::InvalidName: return "name must be valid UTF-8 with at most 1024 bytes";
    case SceneError::InvalidPath: return "path does not resolve to a scene node";
    case SceneError::AmbiguousPath: return "path segment matches multiple siblings";
    case SceneError::ChildrenPresent: return "node still has children";
    case SceneError::DuplicateEntity: return "batch contains the same entity more than once";
    case SceneError::InvalidSnapshot: return "snapshot structure or component data is invalid";
    case SceneError::IdentityConflict: return "persistent identity is already owned";
    case SceneError::RegistryBusy: return "scene mutation is unavailable during registry iteration";
    case SceneError::CloneRejected: return "registry rejected a component clone";
    }
    return "unknown scene error";
}

Scene::Scene(ecs::Registry& shared_registry, SceneLimits configuration)
    : registry_(shared_registry), limits_(configuration) {
    if (limits_.max_nodes > 10000000 || limits_.max_depth == 0 ||
        limits_.max_depth > 10000000 || limits_.max_path_bytes == 0 ||
        limits_.max_path_bytes > 16777216) {
        throw std::invalid_argument("scene limits exceed supported bounds");
    }
    limits_.max_nodes = std::min(limits_.max_nodes,
                                static_cast<std::size_t>(registry_.capacity()));
}

ecs::Registry& Scene::registry() noexcept {
    return registry_;
}

const ecs::Registry& Scene::registry() const noexcept {
    return registry_;
}

const SceneLimits& Scene::limits() const noexcept {
    return limits_;
}

std::size_t Scene::size() const noexcept {
    return nodes_.size();
}

std::uint64_t Scene::revision() const noexcept {
    return revision_;
}

Scene::Node* Scene::node(Entity entity) noexcept {
    const auto found = nodes_.find(entity);
    return found == nodes_.end() ? nullptr : &found->second;
}

const Scene::Node* Scene::node(Entity entity) const noexcept {
    const auto found = nodes_.find(entity);
    return found == nodes_.end() ? nullptr : &found->second;
}

bool Scene::contains(Entity entity) const noexcept {
    return registry_.alive(entity) && registry_.has<ecs::TransformComponent>(entity) &&
           nodes_.contains(entity);
}

std::vector<Entity>& Scene::siblings(Entity parent_entity) {
    return parent_entity ? nodes_.at(parent_entity).children : roots_;
}

const std::vector<Entity>& Scene::siblings(Entity parent_entity) const {
    return parent_entity ? nodes_.at(parent_entity).children : roots_;
}

SceneError Scene::editable(Entity entity) const noexcept {
    if (registry_.iterating()) {
        return SceneError::RegistryBusy;
    }
    if (!registry_.alive(entity)) {
        return SceneError::InvalidEntity;
    }
    return contains(entity) ? SceneError::None : SceneError::NotInScene;
}

SceneError Scene::parent_error(Entity parent_entity) const noexcept {
    return !parent_entity || contains(parent_entity) ? SceneError::None : SceneError::InvalidParent;
}

void Scene::changed(Node* changed_node) noexcept {
    if (revision_ != std::numeric_limits<std::uint64_t>::max()) {
        ++revision_;
    }
    if (changed_node) {
        changed_node->revision = revision_;
    }
}

Entity Scene::create(std::string_view entity_name, Entity parent_entity,
                     const math::Transform& local, PersistentId persistent) {
    const auto normalized = canonical(local);
    if (registry_.iterating() || nodes_.size() >= limits_.max_nodes ||
        parent_error(parent_entity) != SceneError::None || !valid_name(entity_name) || !normalized) {
        return {};
    }
    const auto parent_depth = parent_entity ? depth(parent_entity).value_or(limits_.max_depth) : 0;
    if (parent_entity && parent_depth + 1 >= limits_.max_depth) {
        return {};
    }
    const auto entity = registry_.create(persistent, entity_name);
    if (!entity) {
        return {};
    }
    try {
        auto& transform = registry_.emplace<ecs::TransformComponent>(entity);
        transform.local = *normalized;
        transform.dirty = true;
        const auto accepted = adopt(entity, parent_entity);
        if (!accepted) {
            (void)registry_.destroy(entity);
            return {};
        }
    } catch (...) {
        erase_node(entity);
        (void)registry_.destroy(entity);
        throw;
    }
    return entity;
}

SceneResult Scene::adopt(Entity entity, Entity parent_entity, std::size_t index) {
    if (registry_.iterating()) {
        return {SceneError::RegistryBusy};
    }
    if (!registry_.alive(entity)) {
        return {SceneError::InvalidEntity};
    }
    if (const auto error = parent_error(parent_entity); error != SceneError::None) {
        return {error};
    }
    if (entity == parent_entity) {
        return {SceneError::Cycle};
    }
    if (nodes_.contains(entity)) {
        return reparent(entity, parent_entity, ReparentMode::KeepLocal, index);
    }
    if (nodes_.size() >= limits_.max_nodes) {
        return {SceneError::Capacity};
    }
    if (parent_entity && depth(parent_entity).value_or(limits_.max_depth) + 1 >= limits_.max_depth) {
        return {SceneError::DepthLimit};
    }
    auto& destination = siblings(parent_entity);
    destination.reserve(destination.size() + 1);
    const bool had_transform = registry_.has<ecs::TransformComponent>(entity);
    auto& transform = registry_.emplace<ecs::TransformComponent>(entity);
    const auto normalized = canonical(transform.local);
    if (!normalized) {
        return {SceneError::InvalidTransform};
    }
    try {
        Node entry;
        entry.parent = parent_entity;
        entry.observed_local = *normalized;
        const auto inserted = nodes_.emplace(entity, std::move(entry));
        const auto position = std::min(index, destination.size());
        destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(position), entity);
        transform.local = *normalized;
        transform.dirty = true;
        changed(&inserted.first->second);
    } catch (...) {
        nodes_.erase(entity);
        if (!had_transform) {
            (void)registry_.remove<ecs::TransformComponent>(entity);
        }
        throw;
    }
    return {SceneError::None, 1};
}

Entity Scene::parent(Entity entity) const noexcept {
    const auto* entry = node(entity);
    return contains(entity) && entry && contains(entry->parent) ? entry->parent : Entity{};
}

std::vector<Entity> Scene::children(Entity entity) const {
    std::vector<Entity> result;
    const auto* entry = node(entity);
    if (!entry || !contains(entity)) {
        return result;
    }
    result.reserve(entry->children.size());
    for (const auto child : entry->children) {
        if (contains(child)) {
            result.push_back(child);
        }
    }
    return result;
}

std::vector<Entity> Scene::roots() const {
    std::vector<Entity> result;
    result.reserve(roots_.size());
    for (const auto entity : roots_) {
        if (contains(entity)) {
            result.push_back(entity);
        }
    }
    return result;
}

std::vector<Entity> Scene::ancestors(Entity entity) const {
    std::vector<Entity> result;
    auto current = parent(entity);
    while (current && result.size() < limits_.max_depth) {
        result.push_back(current);
        current = parent(current);
    }
    return result;
}

std::vector<Entity> Scene::descendants(Entity entity, bool include_self) const {
    std::vector<Entity> result;
    if (!contains(entity)) {
        return result;
    }
    std::vector<Entity> pending{entity};
    while (!pending.empty() && result.size() < limits_.max_nodes) {
        const auto current = pending.back();
        pending.pop_back();
        const auto* entry = node(current);
        if (!entry || !contains(current)) {
            continue;
        }
        if (current != entity || include_self) {
            result.push_back(current);
        }
        for (auto child = entry->children.rbegin(); child != entry->children.rend(); ++child) {
            pending.push_back(*child);
        }
    }
    return result;
}

std::vector<Entity> Scene::ordered_entities() const {
    std::vector<Entity> result;
    result.reserve(nodes_.size());
    std::vector<Entity> pending(roots_.rbegin(), roots_.rend());
    while (!pending.empty() && result.size() < limits_.max_nodes) {
        const auto current = pending.back();
        pending.pop_back();
        const auto* entry = node(current);
        if (!entry || !contains(current)) {
            continue;
        }
        result.push_back(current);
        for (auto child = entry->children.rbegin(); child != entry->children.rend(); ++child) {
            pending.push_back(*child);
        }
    }
    return result;
}

std::optional<std::size_t> Scene::depth(Entity entity) const noexcept {
    if (!contains(entity)) {
        return std::nullopt;
    }
    std::size_t result{};
    auto current = node(entity)->parent;
    while (current) {
        const auto* entry = node(current);
        if (!entry || !contains(current) || result >= limits_.max_depth) {
            return std::nullopt;
        }
        ++result;
        current = entry->parent;
    }
    return result;
}

std::optional<std::size_t> Scene::sibling_index(Entity entity) const noexcept {
    const auto* entry = node(entity);
    if (!entry || !contains(entity)) {
        return std::nullopt;
    }
    const auto* parent_node = node(entry->parent);
    const auto& values = parent_node ? parent_node->children : roots_;
    const auto position = std::find(values.begin(), values.end(), entity);
    return position == values.end() ? std::nullopt :
        std::optional{static_cast<std::size_t>(position - values.begin())};
}

bool Scene::is_ancestor(Entity ancestor_entity, Entity descendant_entity) const noexcept {
    if (!contains(ancestor_entity) || !contains(descendant_entity)) {
        return false;
    }
    auto current = parent(descendant_entity);
    std::size_t steps{};
    while (current && steps < limits_.max_depth) {
        if (current == ancestor_entity) {
            return true;
        }
        current = parent(current);
        ++steps;
    }
    return false;
}

Entity Scene::common_ancestor(Entity first, Entity second) const noexcept {
    const auto first_depth = depth(first);
    const auto second_depth = depth(second);
    if (!first_depth || !second_depth) {
        return {};
    }
    auto first_level = *first_depth;
    auto second_level = *second_depth;
    while (first_level > second_level) {
        first = parent(first);
        --first_level;
    }
    while (second_level > first_level) {
        second = parent(second);
        --second_level;
    }
    while (first && second && first != second) {
        first = parent(first);
        second = parent(second);
    }
    return first == second ? first : Entity{};
}

std::size_t Scene::subtree_height(Entity entity) const {
    std::size_t height{};
    std::vector<std::pair<Entity, std::size_t>> pending{{entity, 0}};
    while (!pending.empty()) {
        const auto [current, level] = pending.back();
        pending.pop_back();
        const auto* entry = node(current);
        if (!entry || !contains(current)) {
            continue;
        }
        height = std::max(height, level);
        for (const auto child : entry->children) {
            pending.emplace_back(child, level + 1);
        }
    }
    return height;
}

void Scene::dirty_subtree(Entity entity) {
    auto* entry = node(entity);
    if (!entry) {
        return;
    }
    entry->dirty = true;
    if (auto* transform = registry_.get<ecs::TransformComponent>(entity)) {
        transform->dirty = true;
    }
    std::vector<Entity> pending(entry->children.begin(), entry->children.end());
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        auto* child_node = node(current);
        if (!child_node) {
            continue;
        }
        child_node->dirty = true;
        if (auto* transform = registry_.get<ecs::TransformComponent>(current)) {
            transform->dirty = true;
        }
        pending.insert(pending.end(), child_node->children.begin(), child_node->children.end());
    }
}

SceneResult Scene::mark_dirty(Entity entity) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    dirty_subtree(entity);
    changed(node(entity));
    return {SceneError::None, 1};
}

void Scene::mark_all_dirty() noexcept {
    for (auto& [entity, entry] : nodes_) {
        entry.dirty = true;
        if (auto* transform = registry_.get<ecs::TransformComponent>(entity)) {
            transform->dirty = true;
        }
    }
    changed();
}

SceneError Scene::local_from_world(Entity parent_entity, const math::Mat4& world,
                                   math::Transform& output) const noexcept {
    if (!affine(world)) {
        return SceneError::InvalidTransform;
    }
    auto local_matrix = world;
    if (parent_entity) {
        const auto* parent_node = node(parent_entity);
        if (!parent_node || !parent_node->world_valid || parent_node->dirty) {
            return SceneError::InvalidParent;
        }
        const auto inverse_parent = math::inverse(parent_node->world);
        if (!inverse_parent) {
            return SceneError::SingularParent;
        }
        local_matrix = *inverse_parent * world;
    }
    const auto decomposed = math::decompose(local_matrix);
    if (!decomposed || !canonical(*decomposed)) {
        return SceneError::UnrepresentableTransform;
    }
    output = *decomposed;
    return SceneError::None;
}

SceneResult Scene::reparent(Entity entity, Entity parent_entity, ReparentMode mode,
                            std::size_t index) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    if (const auto error = parent_error(parent_entity); error != SceneError::None) {
        return {error};
    }
    if (entity == parent_entity || is_ancestor(entity, parent_entity)) {
        return {SceneError::Cycle};
    }
    auto* entry = node(entity);
    if (entry->parent == parent_entity) {
        return reorder(entity, index);
    }
    const auto new_depth = parent_entity ? depth(parent_entity).value_or(limits_.max_depth) + 1 : 0;
    if (new_depth >= limits_.max_depth || subtree_height(entity) >= limits_.max_depth - new_depth) {
        return {SceneError::DepthLimit};
    }
    auto replacement = registry_.require<ecs::TransformComponent>(entity).local;
    if (mode == ReparentMode::KeepWorld) {
        const auto synchronized = update();
        if (synchronized.error != SceneError::None) {
            return {synchronized.error};
        }
        entry = node(entity);
        if (!entry || !entry->world_valid) {
            return {SceneError::InvalidTransform};
        }
        if (const auto error = local_from_world(parent_entity, entry->world, replacement);
            error != SceneError::None) {
            return {error};
        }
    }
    auto& destination = siblings(parent_entity);
    destination.reserve(destination.size() + 1);
    auto& previous = siblings(entry->parent);
    (void)std::erase(previous, entity);
    const auto position = std::min(index, destination.size());
    destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(position), entity);
    entry->parent = parent_entity;
    auto& transform = registry_.require<ecs::TransformComponent>(entity);
    transform.local = replacement;
    (void)registry_.mark_changed<ecs::TransformComponent>(entity);
    dirty_subtree(entity);
    changed(entry);
    return {SceneError::None, 1};
}

SceneResult Scene::detach(Entity entity, ReparentMode mode, std::size_t index) {
    return reparent(entity, {}, mode, index);
}

SceneResult Scene::reorder(Entity entity, std::size_t index) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    auto* entry = node(entity);
    auto& values = siblings(entry->parent);
    const auto found = std::find(values.begin(), values.end(), entity);
    if (found == values.end()) {
        return {SceneError::NotInScene};
    }
    const auto old_index = static_cast<std::size_t>(found - values.begin());
    const auto new_index = std::min(index, values.size() - 1);
    if (old_index == new_index) {
        return {};
    }
    values.erase(found);
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(new_index), entity);
    changed(entry);
    return {SceneError::None, 1};
}

void Scene::erase_node(Entity entity) noexcept {
    const auto found = nodes_.find(entity);
    if (found == nodes_.end()) {
        return;
    }
    if (auto* parent_node = node(found->second.parent)) {
        (void)std::erase(parent_node->children, entity);
    } else {
        (void)std::erase(roots_, entity);
    }
    nodes_.erase(found);
    changed();
}

void Scene::rollback_entities(std::span<const Entity> entities) noexcept {
    for (auto current = entities.rbegin(); current != entities.rend(); ++current) {
        erase_node(*current);
        try {
            (void)registry_.destroy(*current);
        } catch (...) {
            // Structural access was checked before creation; retain noexcept cleanup.
        }
    }
}

SceneResult Scene::destroy(Entity entity, DestroyPolicy policy, ReparentMode child_mode) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    const auto synchronized = update();
    if (synchronized.error != SceneError::None) {
        return {synchronized.error};
    }
    auto* entry = node(entity);
    if (policy == DestroyPolicy::RejectChildren && !entry->children.empty()) {
        return {SceneError::ChildrenPresent};
    }
    if (policy == DestroyPolicy::Subtree) {
        const auto selected = descendants(entity, true);
        for (auto current = selected.rbegin(); current != selected.rend(); ++current) {
            erase_node(*current);
            (void)registry_.destroy(*current);
        }
        return {SceneError::None, selected.size()};
    }
    const auto destination_parent = policy == DestroyPolicy::PromoteChildren ? entry->parent : Entity{};
    const auto child_entities = entry->children;
    std::vector<math::Transform> replacements;
    replacements.reserve(child_entities.size());
    for (const auto child : child_entities) {
        auto replacement = registry_.require<ecs::TransformComponent>(child).local;
        if (child_mode == ReparentMode::KeepWorld) {
            const auto* child_node = node(child);
            if (!child_node->world_valid) {
                return {SceneError::InvalidTransform};
            }
            if (const auto error = local_from_world(destination_parent, child_node->world, replacement);
                error != SceneError::None) {
                return {error};
            }
        }
        replacements.push_back(replacement);
    }
    auto& destination = siblings(destination_parent);
    destination.reserve(destination.size() + child_entities.size());
    auto insertion = destination.size();
    if (destination_parent == entry->parent) {
        insertion = sibling_index(entity).value_or(destination.size());
    }
    for (const auto child : child_entities) {
        dirty_subtree(child);
    }
    for (std::size_t index = 0; index < child_entities.size(); ++index) {
        const auto child = child_entities[index];
        auto* child_node = node(child);
        child_node->parent = destination_parent;
        destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(insertion), child);
        ++insertion;
        auto& transform = registry_.require<ecs::TransformComponent>(child);
        transform.local = replacements[index];
        (void)registry_.mark_changed<ecs::TransformComponent>(child);
        changed(child_node);
    }
    entry->children.clear();
    erase_node(entity);
    (void)registry_.destroy(entity);
    return {SceneError::None, 1};
}

SceneResult Scene::destroy_all() {
    if (registry_.iterating()) {
        return {SceneError::RegistryBusy};
    }
    (void)update();
    const auto selected = ordered_entities();
    for (auto current = selected.rbegin(); current != selected.rend(); ++current) {
        erase_node(*current);
        (void)registry_.destroy(*current);
    }
    return {SceneError::None, selected.size()};
}

SceneResult Scene::rename(Entity entity, std::string_view entity_name) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    if (!valid_name(entity_name)) {
        return {SceneError::InvalidName};
    }
    if (registry_.name(entity) == entity_name) {
        return {};
    }
    (void)registry_.set_name(entity, entity_name);
    changed(node(entity));
    return {SceneError::None, 1};
}

std::vector<Entity> Scene::find_all_named(std::string_view entity_name, Entity within,
                                         std::size_t limit) const {
    std::vector<Entity> result;
    if (limit == 0) {
        return result;
    }
    const auto selected = within ? descendants(within, true) : ordered_entities();
    for (const auto entity : selected) {
        if (registry_.name(entity) == entity_name) {
            result.push_back(entity);
            if (result.size() >= limit) {
                break;
            }
        }
    }
    return result;
}

std::optional<std::string> Scene::path(Entity entity) const {
    if (!contains(entity)) {
        return std::nullopt;
    }
    std::vector<std::string> segments;
    std::size_t total{};
    auto current = entity;
    while (current && segments.size() < limits_.max_depth) {
        const auto* entry = node(current);
        if (!entry || (entry->parent && !contains(entry->parent))) {
            return std::nullopt;
        }
        const auto current_name = registry_.name(current);
        std::size_t matches{};
        for (const auto sibling : siblings(entry->parent)) {
            if (contains(sibling) && registry_.name(sibling) == current_name) {
                ++matches;
            }
        }
        auto segment = current_name.empty() || matches > 1
            ? std::string("@") + ecs::format_entity(current) : encode_segment(current_name);
        if (total >= limits_.max_path_bytes || segment.size() > limits_.max_path_bytes - total - 1) {
            return std::nullopt;
        }
        total += segment.size() + 1;
        segments.push_back(std::move(segment));
        current = entry->parent;
    }
    if (current) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(total);
    for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment) {
        result.push_back('/');
        result.append(*segment);
    }
    return result;
}

PathResult Scene::find_path(std::string_view value, Entity relative_to) const {
    if (value.size() > limits_.max_path_bytes || (relative_to && !contains(relative_to))) {
        return {SceneError::InvalidPath, {}};
    }
    auto current = value.starts_with('/') ? Entity{} : relative_to;
    std::size_t offset = value.starts_with('/') ? 1 : 0;
    std::size_t segments{};
    if (value.empty()) {
        return {SceneError::None, current};
    }
    while (offset < value.size()) {
        if (++segments > limits_.max_depth) {
            return {SceneError::DepthLimit, {}};
        }
        const auto separator = value.find('/', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        const auto segment = value.substr(offset, end - offset);
        offset = end == value.size() ? end : end + 1;
        if (segment.empty()) {
            return {SceneError::InvalidPath, {}};
        }
        if (segment == ".") {
            continue;
        }
        if (segment == "..") {
            current = parent(current);
            continue;
        }
        Entity match{};
        if (segment.starts_with('@')) {
            const auto candidate = ecs::parse_entity(segment.substr(1));
            if (candidate && contains(*candidate) && node(*candidate)->parent == current) {
                match = *candidate;
            }
        } else {
            const auto decoded = decode_segment(segment);
            if (!decoded) {
                return {SceneError::InvalidPath, {}};
            }
            for (const auto candidate : siblings(current)) {
                if (contains(candidate) && registry_.name(candidate) == *decoded) {
                    if (match) {
                        return {SceneError::AmbiguousPath, {}};
                    }
                    match = candidate;
                }
            }
        }
        if (!match) {
            return {SceneError::InvalidPath, {}};
        }
        current = match;
    }
    return {SceneError::None, current};
}

SceneResult Scene::apply_local_transforms(std::span<const LocalTransformEdit> edits) {
    if (registry_.iterating()) {
        return {SceneError::RegistryBusy};
    }
    if (edits.size() > limits_.max_nodes) {
        return {SceneError::Capacity};
    }
    std::vector<LocalTransformEdit> prepared;
    prepared.reserve(edits.size());
    std::unordered_set<Entity, ecs::EntityHash> selected;
    selected.reserve(edits.size());
    for (const auto& edit : edits) {
        if (const auto error = editable(edit.entity); error != SceneError::None) {
            return {error};
        }
        if (!selected.insert(edit.entity).second) {
            return {SceneError::DuplicateEntity};
        }
        const auto normalized = canonical(edit.transform);
        if (!normalized) {
            return {SceneError::InvalidTransform};
        }
        if (!same_transform(registry_.require<ecs::TransformComponent>(edit.entity).local, *normalized)) {
            prepared.push_back({edit.entity, *normalized});
        }
    }
    for (const auto& edit : prepared) {
        dirty_subtree(edit.entity);
    }
    for (const auto& edit : prepared) {
        registry_.require<ecs::TransformComponent>(edit.entity).local = edit.transform;
        (void)registry_.mark_changed<ecs::TransformComponent>(edit.entity);
        changed(node(edit.entity));
    }
    return {SceneError::None, prepared.size()};
}

SceneResult Scene::set_local_transform(Entity entity, const math::Transform& value) {
    const std::array<LocalTransformEdit, 1> edits{{{entity, value}}};
    return apply_local_transforms(edits);
}

SceneResult Scene::apply_world_matrices(std::span<const WorldMatrixEdit> edits) {
    if (registry_.iterating()) {
        return {SceneError::RegistryBusy};
    }
    if (edits.size() > limits_.max_nodes) {
        return {SceneError::Capacity};
    }
    (void)update();
    std::unordered_map<Entity, math::Mat4, ecs::EntityHash> desired;
    desired.reserve(edits.size());
    for (const auto& edit : edits) {
        if (const auto error = editable(edit.entity); error != SceneError::None) {
            return {error};
        }
        if (!affine(edit.matrix)) {
            return {SceneError::InvalidTransform};
        }
        if (!desired.emplace(edit.entity, edit.matrix).second) {
            return {SceneError::DuplicateEntity};
        }
    }
    std::unordered_map<Entity, math::Mat4, ecs::EntityHash> planned_world;
    std::vector<LocalTransformEdit> prepared;
    prepared.reserve(edits.size());
    for (const auto entity : ordered_entities()) {
        const auto* entry = node(entity);
        const auto desired_value = desired.find(entity);
        const auto planned_parent = planned_world.find(entry->parent);
        if (desired_value == desired.end() && planned_parent == planned_world.end()) {
            continue;
        }
        math::Mat4 parent_world;
        if (planned_parent != planned_world.end()) {
            parent_world = planned_parent->second;
        } else if (entry->parent) {
            const auto* parent_node = node(entry->parent);
            if (!parent_node->world_valid) {
                return {SceneError::InvalidParent};
            }
            parent_world = parent_node->world;
        }
        auto local = registry_.require<ecs::TransformComponent>(entity).local;
        if (desired_value != desired.end()) {
            const auto inverse_parent = math::inverse(parent_world);
            if (!inverse_parent) {
                return {SceneError::SingularParent};
            }
            const auto decomposed = math::decompose(*inverse_parent * desired_value->second);
            if (!decomposed) {
                return {SceneError::UnrepresentableTransform};
            }
            local = *decomposed;
            prepared.push_back({entity, local});
        }
        const auto normalized = canonical(local);
        if (!normalized) {
            return {SceneError::InvalidTransform};
        }
        const auto next_world = parent_world * math::matrix(*normalized);
        if (!affine(next_world)) {
            return {SceneError::InvalidTransform};
        }
        planned_world.emplace(entity, next_world);
    }
    return apply_local_transforms(prepared);
}

SceneResult Scene::set_world_matrix(Entity entity, const math::Mat4& value) {
    const std::array<WorldMatrixEdit, 1> edits{{{entity, value}}};
    return apply_world_matrices(edits);
}

SceneResult Scene::set_world_transform(Entity entity, const math::Transform& value) {
    const auto normalized = canonical(value);
    return normalized ? set_world_matrix(entity, math::matrix(*normalized)) :
        SceneResult{SceneError::InvalidTransform};
}

std::optional<math::Transform> Scene::local_transform(Entity entity) const {
    const auto* transform = registry_.get<ecs::TransformComponent>(entity);
    return contains(entity) && transform ? std::optional{transform->local} : std::nullopt;
}

SceneResult Scene::set_local_position(Entity entity, math::Vec3 value) {
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->position = value;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::set_local_rotation(Entity entity, math::Quat value) {
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->rotation = value;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::set_local_scale(Entity entity, math::Vec3 value) {
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->scale = value;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::set_world_position(Entity entity, math::Vec3 value) {
    if (!math::finite(value)) {
        return {SceneError::InvalidTransform};
    }
    auto matrix = world_matrix(entity);
    if (!matrix) {
        return {SceneError::InvalidTransform};
    }
    (*matrix)(0, 3) = value.x;
    (*matrix)(1, 3) = value.y;
    (*matrix)(2, 3) = value.z;
    return set_world_matrix(entity, *matrix);
}

SceneResult Scene::translate(Entity entity, math::Vec3 offset, TransformSpace space) {
    if (!math::finite(offset)) {
        return {SceneError::InvalidTransform};
    }
    if (space == TransformSpace::World) {
        const auto matrix = world_matrix(entity);
        return matrix ? set_world_position(entity, position_of(*matrix) + offset) :
            SceneResult{SceneError::InvalidTransform};
    }
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->position += space == TransformSpace::Local ?
        math::rotate(transform->rotation, offset) : offset;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::rotate(Entity entity, math::Quat rotation, TransformSpace space) {
    const auto magnitude = math::length(rotation);
    if (!math::finite(rotation) || !std::isfinite(magnitude) || magnitude <= math::epsilon) {
        return {SceneError::InvalidTransform};
    }
    rotation = math::normalized(rotation);
    if (space == TransformSpace::World) {
        const auto matrix = world_matrix(entity);
        if (!matrix) {
            return {SceneError::InvalidTransform};
        }
        const auto position = position_of(*matrix);
        const auto delta = math::translation_matrix(position) *
            math::trs_matrix({}, rotation, {1.0, 1.0, 1.0}) * math::translation_matrix(-position);
        return set_world_matrix(entity, delta * *matrix);
    }
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->rotation = space == TransformSpace::Local ? transform->rotation * rotation :
        rotation * transform->rotation;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::scale(Entity entity, math::Vec3 factor) {
    auto transform = local_transform(entity);
    if (!transform) {
        return {SceneError::NotInScene};
    }
    transform->scale = transform->scale * factor;
    return set_local_transform(entity, *transform);
}

SceneResult Scene::look_at(Entity entity, math::Vec3 target, math::Vec3 up) {
    auto transform = world_transform(entity);
    if (!transform || !world_transform_exact(entity)) {
        return {SceneError::UnrepresentableTransform};
    }
    const auto direction = target - transform->position;
    if (!math::finite(target) || !math::finite(up) || !math::finite(direction) ||
        math::length_squared(direction) <= math::epsilon ||
        math::length_squared(math::cross(direction, up)) <= math::epsilon) {
        return {SceneError::InvalidTransform};
    }
    transform->rotation = math::look_rotation(direction, up);
    return set_world_transform(entity, *transform);
}

SceneResult Scene::set_enabled_recursive(Entity entity, bool enabled) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    const auto selected = descendants(entity, true);
    for (const auto current : selected) {
        (void)registry_.set_enabled(current, enabled);
        changed(node(current));
    }
    return {SceneError::None, selected.size()};
}

std::size_t Scene::prune_external_changes() {
    std::vector<Entity> removed;
    for (const auto& [entity, entry] : nodes_) {
        (void)entry;
        if (!registry_.alive(entity) || !registry_.has<ecs::TransformComponent>(entity)) {
            removed.push_back(entity);
        }
    }
    std::sort(removed.begin(), removed.end());
    if (removed.empty()) {
        return 0;
    }
    roots_.reserve(nodes_.size());
    for (const auto entity : removed) {
        const auto* entry = node(entity);
        for (const auto child : entry->children) {
            if (auto* child_node = node(child)) {
                child_node->parent = {};
                child_node->dirty = true;
                roots_.push_back(child);
                changed(child_node);
            }
        }
        erase_node(entity);
    }
    return removed.size();
}

SceneUpdate Scene::update() {
    SceneUpdate result;
    if (registry_.iterating()) {
        result.error = SceneError::RegistryBusy;
        result.revision = revision_;
        return result;
    }
    result.removed = prune_external_changes();
    if (limits_.adopt_registry_transforms) {
        for (const auto entity : registry_.view<ecs::TransformComponent>(true)) {
            if (!nodes_.contains(entity)) {
                const auto accepted = adopt(entity);
                if (accepted) {
                    ++result.adopted;
                } else {
                    ++result.skipped;
                }
            }
        }
    }
    const auto order = ordered_entities();
    for (const auto entity : order) {
        auto& entry = *node(entity);
        auto& transform = registry_.require<ecs::TransformComponent>(entity);
        const auto* parent_node = node(entry.parent);
        const auto normalized = canonical(transform.local);
        const bool enabled = registry_.enabled(entity) &&
            (!parent_node || parent_node->effective_enabled);
        if (entry.effective_enabled != enabled) {
            entry.effective_enabled = enabled;
            changed(&entry);
        }
        const bool input_changed = !same_transform(transform.local, entry.observed_local);
        const bool refresh = entry.dirty || transform.dirty || input_changed || !entry.world_valid;
        if (refresh) {
            for (const auto child : entry.children) {
                node(child)->dirty = true;
            }
        }
        bool valid_world = normalized.has_value() && (!parent_node || parent_node->world_valid);
        math::Mat4 next_world = entry.world;
        if (valid_world && refresh) {
            next_world = parent_node ? parent_node->world * math::matrix(*normalized) :
                math::matrix(*normalized);
            valid_world = affine(next_world);
        }
        math::Transform projected;
        if (valid_world) {
            if (const auto decomposed = math::decompose(next_world)) {
                projected = *decomposed;
            } else if (!parent_node) {
                projected = *normalized;
            } else {
                const auto& parent_transform = registry_.require<ecs::TransformComponent>(entry.parent).world;
                projected.position = position_of(next_world);
                projected.rotation = math::normalized(parent_transform.rotation * normalized->rotation);
                projected.scale = parent_transform.scale * normalized->scale;
            }
            valid_world = canonical(projected).has_value();
        }
        if (!valid_world) {
            if (entry.world_valid) {
                changed(&entry);
            }
            entry.world_valid = false;
            entry.world_exact = false;
            entry.dirty = true;
            transform.dirty = true;
            ++result.invalid;
        } else {
            const bool output_changed = !same_transform(transform.world, projected);
            entry.world = next_world;
            entry.world_exact = math::almost_equal(math::matrix(projected), next_world, 1.0e-8);
            entry.world_valid = true;
            entry.dirty = false;
            entry.observed_local = *normalized;
            transform.local = *normalized;
            transform.world = projected;
            transform.dirty = false;
            if (refresh || output_changed) {
                ++result.transformed;
                changed(&entry);
                (void)registry_.mark_changed<ecs::TransformComponent>(entity);
            }
            if (!entry.world_exact) {
                ++result.non_trs;
            }
        }
        std::optional<math::Aabb> own_bounds;
        if (auto* bounds = registry_.get<ecs::BoundsComponent>(entity)) {
            if (entry.world_valid && math::valid(bounds->local)) {
                const auto transformed = math::transform_bounds(entry.world, bounds->local);
                if (math::valid(transformed)) {
                    own_bounds = transformed;
                }
            }
            const auto published = own_bounds.value_or(math::Aabb{});
            if (bounds->world.min != published.min || bounds->world.max != published.max) {
                bounds->world = published;
                (void)registry_.mark_changed<ecs::BoundsComponent>(entity);
            }
        }
        if (!same_bounds(entry.own_bounds, own_bounds)) {
            entry.own_bounds = own_bounds;
            ++result.bounds_updated;
            changed(&entry);
        }
    }
    for (auto current = order.rbegin(); current != order.rend(); ++current) {
        auto& entry = *node(*current);
        auto accumulated = entry.own_bounds;
        for (const auto child : entry.children) {
            accumulated = merge_bounds(accumulated, node(child)->subtree_bounds);
        }
        if (!same_bounds(entry.subtree_bounds, accumulated)) {
            entry.subtree_bounds = accumulated;
            changed(&entry);
        }
    }
    result.revision = revision_;
    return result;
}

const math::Mat4* Scene::cached_world_matrix(Entity entity) const noexcept {
    const auto* entry = node(entity);
    if (!entry || !contains(entity) || !entry->world_valid) {
        return nullptr;
    }
    auto current = entity;
    std::size_t levels{};
    while (current && levels < limits_.max_depth) {
        const auto* current_node = node(current);
        const auto* transform = registry_.get<ecs::TransformComponent>(current);
        if (!current_node || !transform || current_node->dirty || transform->dirty ||
            !same_transform(current_node->observed_local, transform->local)) {
            return nullptr;
        }
        current = current_node->parent;
        ++levels;
    }
    return current ? nullptr : &entry->world;
}

std::optional<math::Mat4> Scene::world_matrix(Entity entity) {
    const auto synchronized = update();
    if (synchronized.error != SceneError::None) {
        return std::nullopt;
    }
    const auto* value = cached_world_matrix(entity);
    return value ? std::optional{*value} : std::nullopt;
}

std::optional<math::Transform> Scene::world_transform(Entity entity) {
    if (!world_matrix(entity)) {
        return std::nullopt;
    }
    return registry_.require<ecs::TransformComponent>(entity).world;
}

bool Scene::world_transform_exact(Entity entity) {
    return world_matrix(entity).has_value() && node(entity)->world_exact;
}

bool Scene::effective_enabled(Entity entity) {
    if (update().error != SceneError::None) {
        return false;
    }
    const auto* entry = node(entity);
    return contains(entity) && entry && entry->effective_enabled;
}

std::optional<math::Vec3> Scene::local_to_world(Entity entity, math::Vec3 point) {
    const auto matrix = world_matrix(entity);
    if (!matrix || !math::finite(point)) {
        return std::nullopt;
    }
    const auto transformed = math::transform_point(*matrix, point);
    return math::finite(transformed) ? std::optional{transformed} : std::nullopt;
}

std::optional<math::Vec3> Scene::world_to_local(Entity entity, math::Vec3 point) {
    const auto matrix = world_matrix(entity);
    if (!matrix || !math::finite(point)) {
        return std::nullopt;
    }
    const auto inverted = math::inverse(*matrix);
    if (!inverted) {
        return std::nullopt;
    }
    const auto transformed = math::transform_point(*inverted, point);
    return math::finite(transformed) ? std::optional{transformed} : std::nullopt;
}

std::optional<math::Vec3> Scene::transform_direction(Entity entity, math::Vec3 direction) {
    const auto matrix = world_matrix(entity);
    if (!matrix || !math::finite(direction)) {
        return std::nullopt;
    }
    const auto transformed = math::transform_vector(*matrix, direction);
    return math::finite(transformed) ? std::optional{transformed} : std::nullopt;
}

SceneResult Scene::set_local_bounds(Entity entity, const math::Aabb& bounds) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    if (!math::valid(bounds)) {
        return {SceneError::InvalidBounds};
    }
    registry_.emplace<ecs::BoundsComponent>(entity).local = bounds;
    (void)registry_.mark_changed<ecs::BoundsComponent>(entity);
    changed(node(entity));
    return {SceneError::None, 1};
}

SceneResult Scene::clear_bounds(Entity entity) {
    if (const auto error = editable(entity); error != SceneError::None) {
        return {error};
    }
    if (registry_.remove<ecs::BoundsComponent>(entity)) {
        changed(node(entity));
        return {SceneError::None, 1};
    }
    return {};
}

std::optional<math::Aabb> Scene::world_bounds(Entity entity) {
    if (update().error != SceneError::None || !contains(entity)) {
        return std::nullopt;
    }
    return node(entity)->own_bounds;
}

std::optional<math::Aabb> Scene::subtree_bounds(Entity entity) {
    if (update().error != SceneError::None || !contains(entity)) {
        return std::nullopt;
    }
    return node(entity)->subtree_bounds;
}

std::optional<math::Aabb> Scene::aggregate_bounds() {
    if (update().error != SceneError::None) {
        return std::nullopt;
    }
    std::optional<math::Aabb> result;
    for (const auto entity : roots_) {
        result = merge_bounds(result, node(entity)->subtree_bounds);
    }
    return result;
}

std::vector<Entity> Scene::query_bounds(const math::Aabb& bounds, bool include_disabled,
                                        std::size_t limit) {
    std::vector<Entity> result;
    if (!math::valid(bounds) || limit == 0 || update().error != SceneError::None) {
        return result;
    }
    for (const auto entity : ordered_entities()) {
        const auto* entry = node(entity);
        if (entry->own_bounds && (include_disabled || entry->effective_enabled) &&
            math::overlaps(bounds, *entry->own_bounds)) {
            result.push_back(entity);
            if (result.size() >= limit) {
                break;
            }
        }
    }
    return result;
}

std::vector<Entity> Scene::query_frustum(const math::Frustum& frustum, bool include_disabled,
                                         std::size_t limit) {
    std::vector<Entity> result;
    if (limit == 0 || std::any_of(frustum.planes.begin(), frustum.planes.end(),
        [](const math::Plane& plane) { return !math::valid(plane); }) ||
        update().error != SceneError::None) {
        return result;
    }
    for (const auto entity : ordered_entities()) {
        const auto* entry = node(entity);
        if (entry->own_bounds && (include_disabled || entry->effective_enabled) &&
            math::intersects(frustum, *entry->own_bounds)) {
            result.push_back(entity);
            if (result.size() >= limit) {
                break;
            }
        }
    }
    return result;
}

std::vector<BoundsHit> Scene::raycast_bounds(const math::Ray& ray, double maximum,
                                            bool include_disabled, std::size_t limit) {
    std::vector<BoundsHit> result;
    if (!math::valid(ray) || std::isnan(maximum) || maximum < 0.0 || limit == 0 ||
        update().error != SceneError::None) {
        return result;
    }
    for (const auto entity : ordered_entities()) {
        const auto* entry = node(entity);
        if (!entry->own_bounds || (!include_disabled && !entry->effective_enabled)) {
            continue;
        }
        if (const auto hit = math::intersect(ray, *entry->own_bounds, 0.0, maximum)) {
            result.push_back({entity, *hit});
        }
    }
    std::sort(result.begin(), result.end(), [](const BoundsHit& first, const BoundsHit& second) {
        return first.hit.distance == second.hit.distance ? first.entity < second.entity :
            first.hit.distance < second.hit.distance;
    });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

std::optional<NodeInfo> Scene::inspect(Entity entity) {
    if (update().error != SceneError::None || !contains(entity)) {
        return std::nullopt;
    }
    const auto& entry = *node(entity);
    return NodeInfo{entity, entry.parent, entry.children, depth(entity).value_or(0),
                    sibling_index(entity).value_or(0), entry.world, entry.own_bounds,
                    entry.subtree_bounds, entry.effective_enabled, entry.world_exact,
                    entry.world_valid, entry.revision};
}

SceneStatistics Scene::statistics() const {
    SceneStatistics result;
    result.nodes = nodes_.size();
    result.roots = roots_.size();
    result.revision = revision_;
    for (const auto& [entity, entry] : nodes_) {
        result.deepest_level = std::max(result.deepest_level, depth(entity).value_or(0));
        result.dirty_nodes += entry.dirty ? 1U : 0U;
        result.non_trs_nodes += entry.world_valid && !entry.world_exact ? 1U : 0U;
        result.invalid_nodes += entry.world_valid ? 0U : 1U;
        result.bounded_nodes += entry.own_bounds ? 1U : 0U;
    }
    return result;
}

CloneResult Scene::clone_subtree(Entity source, Entity parent_entity, ReparentMode mode,
                                 std::string_view root_name) {
    CloneResult result;
    if (registry_.iterating()) {
        result.error = SceneError::RegistryBusy;
        return result;
    }
    (void)update();
    if (const auto error = editable(source); error != SceneError::None) {
        result.error = error;
        return result;
    }
    if (const auto error = parent_error(parent_entity); error != SceneError::None) {
        result.error = error;
        return result;
    }
    if (!valid_name(root_name)) {
        result.error = SceneError::InvalidName;
        return result;
    }
    const auto selected = descendants(source, true);
    if (selected.size() > limits_.max_nodes - nodes_.size()) {
        result.error = SceneError::Capacity;
        return result;
    }
    const auto new_depth = parent_entity ? depth(parent_entity).value_or(limits_.max_depth) + 1 : 0;
    if (new_depth >= limits_.max_depth || subtree_height(source) >= limits_.max_depth - new_depth) {
        result.error = SceneError::DepthLimit;
        return result;
    }
    auto root_local = registry_.require<ecs::TransformComponent>(source).local;
    if (mode == ReparentMode::KeepWorld) {
        if (!node(source)->world_valid) {
            result.error = SceneError::InvalidTransform;
            return result;
        }
        result.error = local_from_world(parent_entity, node(source)->world, root_local);
        if (result.error != SceneError::None) {
            return result;
        }
    }
    std::vector<Entity> created;
    created.reserve(selected.size());
    result.entities.reserve(selected.size());
    std::unordered_map<Entity, Entity, ecs::EntityHash> remap;
    remap.reserve(selected.size());
    try {
        for (const auto original : selected) {
            const auto copy = registry_.clone(original, {}, original == source ? root_name : std::string_view{});
            if (!copy) {
                result.error = SceneError::CloneRejected;
                break;
            }
            created.push_back(copy);
            const auto destination_parent = original == source ? parent_entity : remap.at(node(original)->parent);
            if (original == source) {
                registry_.require<ecs::TransformComponent>(copy).local = root_local;
            }
            const auto accepted = adopt(copy, destination_parent);
            if (!accepted) {
                result.error = accepted.error;
                break;
            }
            remap.emplace(original, copy);
            result.entities.push_back({original, copy});
        }
    } catch (...) {
        rollback_entities(created);
        throw;
    }
    if (result.error != SceneError::None) {
        rollback_entities(created);
        result.entities.clear();
        return result;
    }
    result.root = created.front();
    return result;
}

std::optional<SceneSnapshot> Scene::snapshot(Entity subtree) {
    if (update().error != SceneError::None || (subtree && !contains(subtree))) {
        return std::nullopt;
    }
    SceneSnapshot result;
    result.revision = revision_;
    const auto selected = subtree ? descendants(subtree, true) : ordered_entities();
    result.nodes.reserve(selected.size());
    std::unordered_map<Entity, std::size_t, ecs::EntityHash> indices;
    indices.reserve(selected.size());
    for (const auto entity : selected) {
        const auto local = canonical(registry_.require<ecs::TransformComponent>(entity).local);
        if (!local) {
            return std::nullopt;
        }
        SnapshotNode saved;
        saved.source = entity;
        saved.persistent = registry_.persistent_id(entity);
        saved.name = registry_.name(entity);
        saved.local = *local;
        saved.enabled = registry_.enabled(entity);
        saved.layers = registry_.layers(entity);
        saved.tags = registry_.tags(entity);
        if (const auto parent_index = indices.find(node(entity)->parent); parent_index != indices.end()) {
            saved.parent_index = parent_index->second;
        }
        if (const auto* bounds = registry_.get<ecs::BoundsComponent>(entity); bounds && math::valid(bounds->local)) {
            saved.bounds = bounds->local;
        }
        indices.emplace(entity, result.nodes.size());
        result.nodes.push_back(std::move(saved));
    }
    return result;
}

CloneResult Scene::import_snapshot(const SceneSnapshot& saved, Entity parent_entity,
                                   bool preserve_persistent) {
    CloneResult result;
    if (registry_.iterating()) {
        result.error = SceneError::RegistryBusy;
        return result;
    }
    (void)update();
    if (const auto error = parent_error(parent_entity); error != SceneError::None) {
        result.error = error;
        return result;
    }
    if (saved.version != 1) {
        result.error = SceneError::InvalidSnapshot;
        return result;
    }
    if (saved.nodes.size() > limits_.max_nodes - nodes_.size()) {
        result.error = SceneError::Capacity;
        return result;
    }
    const auto root_depth = parent_entity ? depth(parent_entity).value_or(limits_.max_depth) + 1 : 0;
    std::vector<std::size_t> levels;
    std::vector<math::Transform> locals;
    levels.reserve(saved.nodes.size());
    locals.reserve(saved.nodes.size());
    std::unordered_set<Entity, ecs::EntityHash> sources;
    std::unordered_set<std::uint64_t> identities;
    for (std::size_t index = 0; index < saved.nodes.size(); ++index) {
        const auto& entry = saved.nodes[index];
        if ((entry.parent_index != no_parent && entry.parent_index >= index) ||
            !valid_name(entry.name) || entry.tags.size() > 64 ||
            (entry.bounds && !math::valid(*entry.bounds))) {
            result.error = SceneError::InvalidSnapshot;
            return result;
        }
        const auto normalized = canonical(entry.local);
        if (!normalized) {
            result.error = SceneError::InvalidTransform;
            return result;
        }
        if (entry.source && !sources.insert(entry.source).second) {
            result.error = SceneError::DuplicateEntity;
            return result;
        }
        if (preserve_persistent && entry.persistent &&
            (registry_.find(entry.persistent) || !identities.insert(entry.persistent.value).second)) {
            result.error = SceneError::IdentityConflict;
            return result;
        }
        std::unordered_set<std::string_view> tags;
        for (const auto& tag : entry.tags) {
            if (!valid_tag(tag) || !tags.insert(tag).second) {
                result.error = SceneError::InvalidSnapshot;
                return result;
            }
        }
        const auto level = entry.parent_index == no_parent ? root_depth : levels[entry.parent_index] + 1;
        if (level >= limits_.max_depth) {
            result.error = SceneError::DepthLimit;
            return result;
        }
        levels.push_back(level);
        locals.push_back(*normalized);
    }
    std::vector<Entity> created;
    created.reserve(saved.nodes.size());
    result.entities.reserve(saved.nodes.size());
    try {
        for (std::size_t index = 0; index < saved.nodes.size(); ++index) {
            const auto& entry = saved.nodes[index];
            const auto destination_parent = entry.parent_index == no_parent ? parent_entity :
                created[entry.parent_index];
            const auto entity = create(entry.name, destination_parent, locals[index],
                                        preserve_persistent ? entry.persistent : PersistentId{});
            if (!entity) {
                result.error = SceneError::Capacity;
                break;
            }
            created.push_back(entity);
            (void)registry_.set_enabled(entity, entry.enabled);
            (void)registry_.set_layers(entity, entry.layers);
            for (const auto& tag : entry.tags) {
                if (!registry_.add_tag(entity, tag)) {
                    result.error = SceneError::InvalidSnapshot;
                    break;
                }
            }
            if (result.error != SceneError::None) {
                break;
            }
            if (entry.bounds) {
                (void)set_local_bounds(entity, *entry.bounds);
            }
            result.entities.push_back({entry.source, entity});
        }
    } catch (...) {
        rollback_entities(created);
        throw;
    }
    if (result.error != SceneError::None) {
        rollback_entities(created);
        result.entities.clear();
        return result;
    }
    result.root = created.empty() ? Entity{} : created.front();
    return result;
}

void register_scene(core::Kernel& kernel) {
    auto* entities = kernel.services().find<ecs::Registry>();
    if (!entities) {
        throw std::logic_error("scene registration requires the shared ECS Registry service");
    }
    if (kernel.services().find<Scene>()) {
        return;
    }
    SceneLimits configured;
    configured.max_nodes = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("scene.capacity", 65536), 0, 10000000));
    configured.max_depth = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("scene.max_depth", 4096), 1, 10000000));
    configured.adopt_registry_transforms = kernel.config().boolean("scene.auto_adopt", true);
    (void)kernel.services().emplace<Scene>(*entities, configured);
    (void)kernel.register_module({"scene.scene", "Ordered ECS hierarchy, exact affine transforms and bounds", 1});
    (void)kernel.add_system({"scene.transforms.pre", core::Phase::PreSimulation, -9000, {"ecs.deferred"}},
        [](core::Kernel& host, const core::Frame&) {
            (void)host.services().get<Scene>().update();
        });
    (void)kernel.add_system({"scene.transforms.post", core::Phase::PostSimulation, 9000, {}},
        [previous_invalid = std::size_t{}, previous_skipped = std::size_t{}]
        (core::Kernel& host, const core::Frame&) mutable {
            auto& scene = host.services().get<Scene>();
            const auto updated = scene.update();
            host.metric("scene.nodes", static_cast<double>(scene.size()));
            host.metric("scene.transformed", static_cast<double>(updated.transformed));
            host.metric("scene.bounds_updated", static_cast<double>(updated.bounds_updated));
            host.metric("scene.non_trs", static_cast<double>(updated.non_trs));
            host.metric("scene.invalid", static_cast<double>(updated.invalid));
            host.metric("scene.skipped", static_cast<double>(updated.skipped));
            if (updated.invalid != previous_invalid || updated.skipped != previous_skipped) {
                if (updated.invalid != 0 || updated.skipped != 0) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "scene",
                        std::to_string(updated.invalid) + " invalid transforms; " +
                        std::to_string(updated.skipped) + " registry nodes could not be adopted");
                }
                previous_invalid = updated.invalid;
                previous_skipped = updated.skipped;
            }
        });
    (void)kernel.add_command("scene.create", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: scene.create name [parent]"};
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        Entity parent_entity{};
        if (args.size() == 2 && args[1] != "/" && args[1] != "-") {
            parent_entity = resolve_entity(scene, args[1]);
            if (!parent_entity) {
                return core::CommandResult{false, "parent name, path or handle did not resolve uniquely"};
            }
        }
        const auto entity = scene.create(args[0], parent_entity);
        return core::CommandResult{static_cast<bool>(entity), entity ? ecs::format_entity(entity) :
            "creation rejected: invalid name, exhausted capacity or hierarchy depth"};
    }, "Create a named scene entity under an optional parent; / selects a root");
    (void)kernel.add_command("scene.reparent", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) {
            return core::CommandResult{false, "usage: scene.reparent entity parent|/ [world|local]"};
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        const auto entity = resolve_entity(scene, args[0]);
        const auto parent_entity = args[1] == "/" || args[1] == "-" ? Entity{} :
            resolve_entity(scene, args[1]);
        if (!entity || (!parent_entity && args[1] != "/" && args[1] != "-")) {
            return core::CommandResult{false, "entity or parent did not resolve uniquely"};
        }
        ReparentMode mode = ReparentMode::KeepWorld;
        if (args.size() == 3) {
            if (args[2] != "local" && args[2] != "world") {
                return core::CommandResult{false, "preservation mode must be world or local"};
            }
            mode = args[2] == "local" ? ReparentMode::KeepLocal : ReparentMode::KeepWorld;
        }
        return command_result(scene.reparent(entity, parent_entity, mode));
    }, "Reparent without cycles; keep world by default, rejecting lossy TRS conversion");
    (void)kernel.add_command("scene.move", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 4 || args.size() > 5) {
            return core::CommandResult{false, "usage: scene.move entity dx dy dz [parent|local|world]"};
        }
        const auto x = parse_number(args[1]);
        const auto y = parse_number(args[2]);
        const auto z = parse_number(args[3]);
        if (!x || !y || !z) {
            return core::CommandResult{false, "movement requires three finite numbers"};
        }
        TransformSpace space = TransformSpace::Parent;
        if (args.size() == 5) {
            if (args[4] == "local") {
                space = TransformSpace::Local;
            } else if (args[4] == "world") {
                space = TransformSpace::World;
            } else if (args[4] != "parent") {
                return core::CommandResult{false, "space must be parent, local or world"};
            }
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        return command_result(scene.translate(resolve_entity(scene, args[0]), {*x, *y, *z}, space));
    }, "Translate a scene node by a finite offset in parent, local rotation or world axes");
    (void)kernel.add_command("scene.list", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 1) {
            return core::CommandResult{false, "usage: scene.list [subtree]"};
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        Entity subtree{};
        if (!args.empty() && args[0] != "/") {
            subtree = resolve_entity(scene, args[0]);
            if (!subtree) {
                return core::CommandResult{false, "subtree did not resolve uniquely"};
            }
        }
        const auto selected = subtree ? scene.descendants(subtree, true) : scene.ordered_entities();
        std::ostringstream output;
        const auto count = std::min<std::size_t>(selected.size(), 256);
        output << selected.size() << " scene nodes\n";
        for (std::size_t index = 0; index < count; ++index) {
            const auto entity = selected[index];
            const auto* matrix = scene.cached_world_matrix(entity);
            output << ecs::format_entity(entity) << ' ' << scene.path(entity).value_or("<path too long>");
            if (matrix) {
                const auto position = position_of(*matrix);
                output << " world=(" << position.x << ',' << position.y << ',' << position.z << ')';
            } else {
                output << " invalid transform";
            }
            output << '\n';
        }
        if (count < selected.size()) {
            output << "output limited to " << count << " nodes";
        }
        return core::CommandResult{true, output.str()};
    }, "List up to 256 ordered scene nodes, canonical paths and exact world positions");
    (void)kernel.add_command("scene.destroy", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: scene.destroy entity [subtree|detach|promote|leaf]"};
        }
        DestroyPolicy policy = DestroyPolicy::Subtree;
        if (args.size() == 2) {
            if (args[1] == "detach") {
                policy = DestroyPolicy::DetachChildren;
            } else if (args[1] == "promote") {
                policy = DestroyPolicy::PromoteChildren;
            } else if (args[1] == "leaf") {
                policy = DestroyPolicy::RejectChildren;
            } else if (args[1] != "subtree") {
                return core::CommandResult{false, "unknown destruction policy"};
            }
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        return command_result(scene.destroy(resolve_entity(scene, args[0]), policy));
    }, "Destroy a subtree, reject non-leaves, or preserve surviving children's world transforms");
    (void)kernel.add_command("scene.clone", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: scene.clone source [parent]"};
        }
        auto& scene = host.services().get<Scene>();
        (void)scene.update();
        Entity parent_entity{};
        if (args.size() == 2 && args[1] != "/") {
            parent_entity = resolve_entity(scene, args[1]);
            if (!parent_entity) {
                return core::CommandResult{false, "parent did not resolve uniquely"};
            }
        }
        const auto result = scene.clone_subtree(resolve_entity(scene, args[0]), parent_entity);
        return core::CommandResult{static_cast<bool>(result), result ?
            ecs::format_entity(result.root) + " cloned " + std::to_string(result.entities.size()) + " nodes" :
            std::string(describe(result.error))};
    }, "Clone a subtree through Registry component cloning with fresh transient handles");
}

} // namespace ge::scene
