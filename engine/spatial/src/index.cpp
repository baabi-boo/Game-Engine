#include <ge/spatial/index.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ge::spatial {
namespace {

constexpr double maximum_cost = 1.0e280;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

double cost(const math::Aabb& box) noexcept {
    const double area = math::surface_area(box);
    return std::isfinite(area) ? std::min(area, maximum_cost) : maximum_cost;
}

bool same_bounds(const math::Aabb& first, const math::Aabb& second) noexcept {
    return first.min == second.min && first.max == second.max;
}

double midpoint(const math::Aabb& box, std::size_t axis) noexcept {
    return box.min[axis] * 0.5 + box.max[axis] * 0.5;
}

bool valid_frustum(const math::Frustum& frustum) noexcept {
    return std::all_of(frustum.planes.begin(), frustum.planes.end(),
        [](const math::Plane& plane) { return math::valid(plane); });
}

bool nearer(const RayHit& first, const RayHit& second) noexcept {
    if (first.hit.distance != second.hit.distance) {
        return first.hit.distance < second.hit.distance;
    }
    return first.proxy < second.proxy;
}

bool number(std::string_view text, double& output) noexcept {
    if (text.empty() || text.size() > 128) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::isfinite(output);
}

QueryReport stale_report() noexcept {
    QueryReport result;
    result.stale = true;
    result.truncated = true;
    return result;
}

std::string query_summary(const QueryReport& report) {
    return "written=" + std::to_string(report.written) +
        " visited=" + std::to_string(report.visited) +
        " truncated=" + (report.truncated ? "true" : "false");
}

} // namespace

DynamicAabbTree::DynamicAabbTree(TreeConfig configuration) : config_(configuration) {
    if (config_.capacity > maximum_capacity || !std::isfinite(config_.fat_margin) ||
        config_.fat_margin < 0.0 || !std::isfinite(config_.displacement_multiplier) ||
        config_.displacement_multiplier < 0.0) {
        throw std::invalid_argument("invalid dynamic AABB tree configuration");
    }
    const auto capacity = static_cast<std::size_t>(config_.capacity);
    nodes_.resize(capacity == 0 ? 0 : capacity * 2 - 1);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].next_free = i + 1 < nodes_.size() ? static_cast<Index>(i + 1) : none;
    }
    free_head_ = nodes_.empty() ? none : Index{};
}

DynamicAabbTree::Index DynamicAabbTree::allocate_node() noexcept {
    if (free_head_ == none) {
        return none;
    }
    const Index index = free_head_;
    auto& node = nodes_[index];
    free_head_ = node.next_free;
    const auto generation = node.generation;
    node = Node{};
    node.generation = generation;
    node.height = 0;
    ++live_nodes_;
    return index;
}

void DynamicAabbTree::release_node(Index index) noexcept {
    auto& node = nodes_[index];
    node.entity = {};
    node.layers = 0;
    node.parent = none;
    node.left = none;
    node.right = none;
    node.next_free = none;
    --live_nodes_;
    if (node.generation == std::numeric_limits<std::uint64_t>::max()) {
        node.height = -2;
        ++retired_nodes_;
        return;
    }
    ++node.generation;
    node.height = -1;
    node.next_free = free_head_;
    free_head_ = index;
}

ProxyHandle DynamicAabbTree::handle(Index index) const noexcept {
    return {index + 1, nodes_[index].generation};
}

bool DynamicAabbTree::contains(ProxyHandle proxy) const noexcept {
    if (!proxy || proxy.index > nodes_.size()) {
        return false;
    }
    const auto& node = nodes_[proxy.index - 1];
    return node.height == 0 && node.left == none && node.entity.valid() &&
        node.generation == proxy.generation;
}

std::optional<ProxyInfo> DynamicAabbTree::get(ProxyHandle proxy) const noexcept {
    if (!contains(proxy)) {
        return std::nullopt;
    }
    const auto& node = nodes_[proxy.index - 1];
    return ProxyInfo{proxy, node.entity, node.tight, node.box, node.layers};
}

bool DynamicAabbTree::fatten(const math::Aabb& bounds, math::Vec3 displacement,
                           math::Aabb& output) const noexcept {
    if (!math::valid(bounds) || !math::finite(displacement)) {
        return false;
    }
    auto candidate = math::expanded(bounds, config_.fat_margin);
    const auto prediction = displacement * config_.displacement_multiplier;
    if (!math::valid(candidate) || !math::finite(prediction)) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (prediction[axis] < 0.0) {
            candidate.min[axis] += prediction[axis];
        } else {
            candidate.max[axis] += prediction[axis];
        }
    }
    if (!math::valid(candidate)) {
        return false;
    }
    output = candidate;
    return true;
}

ProxyHandle DynamicAabbTree::insert(ecs::Entity entity, const math::Aabb& bounds,
                                  std::uint64_t layers, math::Vec3 displacement) {
    math::Aabb fat;
    const std::size_t needed = root_ == none ? 1 : 2;
    if (!entity || proxies_ >= config_.capacity ||
        nodes_.size() - live_nodes_ - retired_nodes_ < needed ||
        !fatten(bounds, displacement, fat)) {
        return {};
    }
    const auto leaf = allocate_node();
    const auto branch = root_ == none ? none : allocate_node();
    auto& node = nodes_[leaf];
    node.box = fat;
    node.tight = bounds;
    node.entity = entity;
    node.layers = layers;
    insert_leaf(leaf, branch);
    ++proxies_;
    increment(insertions_);
    return handle(leaf);
}

void DynamicAabbTree::insert_leaf(Index leaf, Index branch) noexcept {
    if (root_ == none) {
        root_ = leaf;
        nodes_[leaf].parent = none;
        return;
    }
    const auto leaf_box = nodes_[leaf].box;
    Index sibling = root_;
    while (nodes_[sibling].height != 0) {
        const auto& current = nodes_[sibling];
        const double current_cost = cost(current.box);
        const double merged_cost = cost(math::merge(current.box, leaf_box));
        const double inherited = 2.0 * std::max(0.0, merged_cost - current_cost);
        const auto child_cost = [&](Index child) {
            const auto& value = nodes_[child];
            const double combined = cost(math::merge(value.box, leaf_box));
            return inherited + (value.height == 0 ? combined :
                std::max(0.0, combined - cost(value.box)));
        };
        const double left_cost = child_cost(current.left);
        const double right_cost = child_cost(current.right);
        if (2.0 * merged_cost < left_cost && 2.0 * merged_cost < right_cost) {
            break;
        }
        const bool prefer_left = left_cost < right_cost ||
            (left_cost == right_cost && nodes_[current.left].height <= nodes_[current.right].height);
        sibling = prefer_left ? current.left : current.right;
    }
    const auto old_parent = nodes_[sibling].parent;
    auto& parent = nodes_[branch];
    parent.parent = old_parent;
    parent.left = sibling;
    parent.right = leaf;
    parent.entity = {};
    nodes_[sibling].parent = branch;
    nodes_[leaf].parent = branch;
    if (old_parent == none) {
        root_ = branch;
    } else if (nodes_[old_parent].left == sibling) {
        nodes_[old_parent].left = branch;
    } else {
        nodes_[old_parent].right = branch;
    }
    refit_upwards(branch);
}

void DynamicAabbTree::detach_leaf(Index leaf, bool release_branch) noexcept {
    if (root_ == leaf) {
        root_ = none;
        nodes_[leaf].parent = none;
        return;
    }
    const auto parent = nodes_[leaf].parent;
    const auto grandparent = nodes_[parent].parent;
    const auto sibling = nodes_[parent].left == leaf ? nodes_[parent].right : nodes_[parent].left;
    nodes_[sibling].parent = grandparent;
    if (grandparent == none) {
        root_ = sibling;
    } else if (nodes_[grandparent].left == parent) {
        nodes_[grandparent].left = sibling;
    } else {
        nodes_[grandparent].right = sibling;
    }
    nodes_[leaf].parent = none;
    if (release_branch) {
        release_node(parent);
    }
    refit_upwards(grandparent);
}

bool DynamicAabbTree::remove(ProxyHandle proxy) noexcept {
    if (!contains(proxy)) {
        return false;
    }
    const auto index = proxy.index - 1;
    detach_leaf(index, true);
    release_node(index);
    --proxies_;
    increment(removals_);
    return true;
}

ProxyUpdate DynamicAabbTree::update(ProxyHandle proxy, const math::Aabb& bounds,
                                  math::Vec3 displacement, bool force_reinsert) noexcept {
    if (!contains(proxy)) {
        return ProxyUpdate::InvalidHandle;
    }
    math::Aabb fat;
    if (!fatten(bounds, displacement, fat)) {
        return ProxyUpdate::InvalidBounds;
    }
    const auto leaf = proxy.index - 1;
    auto& node = nodes_[leaf];
    if (!force_reinsert && math::contains(node.box, bounds) &&
        cost(node.box) <= std::max(4.0 * cost(fat), 1.0e-24)) {
        if (same_bounds(node.tight, bounds)) {
            return ProxyUpdate::Unchanged;
        }
        node.tight = bounds;
        return ProxyUpdate::Refitted;
    }
    const auto branch = node.parent;
    detach_leaf(leaf, false);
    node.tight = bounds;
    node.box = fat;
    insert_leaf(leaf, branch);
    increment(reinsertions_);
    return ProxyUpdate::Reinserted;
}

bool DynamicAabbTree::set_layers(ProxyHandle proxy, std::uint64_t layers) noexcept {
    if (!contains(proxy)) {
        return false;
    }
    auto& node = nodes_[proxy.index - 1];
    if (node.layers != layers) {
        node.layers = layers;
        refit_upwards(node.parent);
    }
    return true;
}

void DynamicAabbTree::refit_node(Index index) noexcept {
    auto& node = nodes_[index];
    if (node.left == none) {
        node.height = 0;
        return;
    }
    const auto& left = nodes_[node.left];
    const auto& right = nodes_[node.right];
    node.box = math::merge(left.box, right.box);
    node.layers = left.layers | right.layers;
    node.height = 1 + std::max(left.height, right.height);
}

DynamicAabbTree::Index DynamicAabbTree::rotate(Index index) noexcept {
    auto& root = nodes_[index];
    if (root.height < 2) {
        return index;
    }
    const Index left_index = root.left;
    const Index right_index = root.right;
    const auto difference = nodes_[right_index].height - nodes_[left_index].height;
    if (difference >= -1 && difference <= 1) {
        return index;
    }
    const auto old_parent = root.parent;
    const bool promote_right = difference > 1;
    const Index promoted_index = promote_right ? right_index : left_index;
    auto& promoted = nodes_[promoted_index];
    const Index first = promoted.left;
    const Index second = promoted.right;
    const bool first_taller = nodes_[first].height > nodes_[second].height;
    const Index retained = first_taller ? first : second;
    const Index transferred = first_taller ? second : first;
    promoted.parent = old_parent;
    root.parent = promoted_index;
    if (old_parent == none) {
        root_ = promoted_index;
    } else if (nodes_[old_parent].left == index) {
        nodes_[old_parent].left = promoted_index;
    } else {
        nodes_[old_parent].right = promoted_index;
    }
    if (promote_right) {
        promoted.left = index;
        promoted.right = retained;
        root.right = transferred;
    } else {
        promoted.right = index;
        promoted.left = retained;
        root.left = transferred;
    }
    nodes_[retained].parent = promoted_index;
    nodes_[transferred].parent = index;
    refit_node(index);
    refit_node(promoted_index);
    increment(rotations_);
    return promoted_index;
}

void DynamicAabbTree::refit_upwards(Index index) noexcept {
    while (index != none) {
        refit_node(index);
        const auto balanced = rotate(index);
        index = nodes_[balanced].parent;
    }
}

void DynamicAabbTree::clear() noexcept {
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].height >= 0) {
            release_node(static_cast<Index>(index));
        }
    }
    root_ = none;
    proxies_ = 0;
}

std::size_t DynamicAabbTree::size() const noexcept {
    return proxies_;
}

const TreeConfig& DynamicAabbTree::config() const noexcept {
    return config_;
}

std::optional<math::Aabb> DynamicAabbTree::bounds() const noexcept {
    return root_ == none ? std::nullopt : std::optional{nodes_[root_].box};
}

TreeStatistics DynamicAabbTree::statistics() const noexcept {
    TreeStatistics result;
    result.proxies = proxies_;
    result.nodes = live_nodes_;
    result.capacity = config_.capacity;
    result.free_nodes = nodes_.size() - live_nodes_ - retired_nodes_;
    result.retired_nodes = retired_nodes_;
    result.memory_bytes = nodes_.capacity() * sizeof(Node);
    result.height = root_ == none ? 0 : nodes_[root_].height;
    result.insertions = insertions_;
    result.removals = removals_;
    result.reinsertions = reinsertions_;
    result.rotations = rotations_;
    result.rebuilds = rebuilds_;
    double summed_cost{};
    for (const auto& node : nodes_) {
        if (node.height < 0) {
            continue;
        }
        summed_cost += cost(node.box);
        if (node.height > 0) {
            const auto balance = std::abs(nodes_[node.left].height - nodes_[node.right].height);
            result.maximum_balance = std::max(result.maximum_balance, balance);
        }
    }
    if (root_ != none && cost(nodes_[root_].box) > 0.0) {
        result.surface_area_ratio = summed_cost / cost(nodes_[root_].box);
    }
    return result;
}

DynamicAabbTree::Index DynamicAabbTree::next_after(Index index) const noexcept {
    while (nodes_[index].parent != none) {
        const auto parent = nodes_[index].parent;
        if (nodes_[parent].left == index) {
            return nodes_[parent].right;
        }
        index = parent;
    }
    return none;
}

bool DynamicAabbTree::accepts(const Node& node, const QueryFilter& filter) const noexcept {
    return (node.layers & filter.layers) != 0 &&
        (node.height != 0 || !filter.exclude || node.entity != filter.exclude);
}

template<class Predicate>
QueryReport DynamicAabbTree::query_impl(Predicate&& predicate, std::span<QueryHit> output,
                                      const QueryFilter& filter) const {
    QueryReport result;
    const auto budget = std::min(config_.traversal_limit, filter.max_visits);
    Index current = root_;
    while (current != none && result.visited < budget) {
        ++result.visited;
        const auto& node = nodes_[current];
        if (!accepts(node, filter) || !predicate(node.box)) {
            current = next_after(current);
            continue;
        }
        if (node.height != 0) {
            current = node.left;
            continue;
        }
        const auto& selected = filter.fat_bounds ? node.box : node.tight;
        if (predicate(selected)) {
            ++result.matched;
            if (result.written == output.size()) {
                result.truncated = true;
                return result;
            }
            output[result.written++] = {handle(current), node.entity, selected};
        }
        current = next_after(current);
    }
    result.traversal_exhausted = current != none;
    result.truncated = result.traversal_exhausted;
    return result;
}

QueryReport DynamicAabbTree::query(const math::Aabb& bounds, std::span<QueryHit> output,
                                 QueryFilter filter) const {
    if (!math::valid(bounds)) {
        QueryReport result;
        result.invalid_input = true;
        return result;
    }
    return query_impl([&bounds](const math::Aabb& box) { return math::overlaps(bounds, box); },
        output, filter);
}

QueryReport DynamicAabbTree::query_frustum(const math::Frustum& frustum,
                                         std::span<QueryHit> output, QueryFilter filter) const {
    if (!valid_frustum(frustum)) {
        QueryReport result;
        result.invalid_input = true;
        return result;
    }
    return query_impl([&frustum](const math::Aabb& box) { return math::intersects(frustum, box); },
        output, filter);
}

QueryReport DynamicAabbTree::raycast(const math::Ray& ray, double maximum,
                                   std::span<RayHit> output, QueryFilter filter,
                                   double minimum) const {
    QueryReport result;
    if (!math::valid(ray) || !std::isfinite(minimum) || std::isnan(maximum) ||
        maximum < minimum) {
        result.invalid_input = true;
        return result;
    }
    const auto budget = std::min(config_.traversal_limit, filter.max_visits);
    Index current = root_;
    while (current != none && result.visited < budget) {
        ++result.visited;
        const auto& node = nodes_[current];
        if (!accepts(node, filter) || !math::ray_interval(ray, node.box, minimum, maximum)) {
            current = next_after(current);
            continue;
        }
        if (node.height != 0) {
            current = node.left;
            continue;
        }
        const auto& selected = filter.fat_bounds ? node.box : node.tight;
        if (const auto interval = math::ray_interval(ray, selected, minimum, maximum)) {
            const auto surface = math::intersect(ray, selected, minimum, maximum);
            auto hit = surface.value_or(math::RayHit{});
            if (!surface || surface->distance != interval->enter) {
                hit.distance = interval->enter;
                hit.point = ray.point(interval->enter);
                hit.normal = {};
                hit.inside = math::contains(selected, ray.point(minimum));
            }
            if (!std::isfinite(hit.distance) || !math::finite(hit.point)) {
                result.invalid_input = true;
                current = next_after(current);
                continue;
            }
            ++result.matched;
            const RayHit candidate{handle(current), node.entity, hit};
            if (output.empty()) {
                result.truncated = true;
                return result;
            }
            if (result.written < output.size()) {
                output[result.written++] = candidate;
                auto prefix = output.first(result.written);
                std::push_heap(prefix.begin(), prefix.end(), nearer);
            } else if (nearer(candidate, output.front())) {
                std::pop_heap(output.begin(), output.end(), nearer);
                output.back() = candidate;
                std::push_heap(output.begin(), output.end(), nearer);
            }
        }
        current = next_after(current);
    }
    auto prefix = output.first(result.written);
    std::sort_heap(prefix.begin(), prefix.end(), nearer);
    result.traversal_exhausted = current != none;
    result.truncated = result.traversal_exhausted || result.matched > result.written;
    return result;
}

QueryReport DynamicAabbTree::collect_pairs(std::span<OverlapPair> output, QueryFilter filter,
                                         bool include_same_entity) const {
    QueryReport result;
    if (root_ == none || proxies_ < 2) {
        return result;
    }
    struct Work { Index first; Index second; };
    const auto budget = std::min(config_.traversal_limit, filter.max_visits);
    const auto depth_bound = static_cast<std::size_t>(nodes_[root_].height) * 4 + 4;
    const auto stack_limit = std::min({live_nodes_, budget, depth_bound});
    if (stack_limit == 0) {
        result.traversal_exhausted = true;
        result.truncated = true;
        return result;
    }
    std::vector<Work> pending;
    pending.reserve(stack_limit);
    pending.push_back({root_, root_});
    while (!pending.empty() && result.visited < budget) {
        const auto work = pending.back();
        pending.pop_back();
        ++result.visited;
        const auto& first = nodes_[work.first];
        const auto& second = nodes_[work.second];
        if (!accepts(first, filter) || !accepts(second, filter) ||
            !math::overlaps(first.box, second.box)) {
            continue;
        }
        if (work.first == work.second) {
            if (first.height == 0) {
                continue;
            }
            if (stack_limit - pending.size() < 3) {
                result.traversal_exhausted = true;
                break;
            }
            pending.push_back({first.right, first.right});
            pending.push_back({first.left, first.right});
            pending.push_back({first.left, first.left});
            continue;
        }
        if (first.height == 0 && second.height == 0) {
            if ((!include_same_entity && first.entity == second.entity) ||
                (!filter.fat_bounds && !math::overlaps(first.tight, second.tight))) {
                continue;
            }
            ++result.matched;
            if (result.written == output.size()) {
                result.truncated = true;
                return result;
            }
            const bool ordered = work.first < work.second;
            output[result.written++] = ordered ?
                OverlapPair{handle(work.first), handle(work.second), first.entity, second.entity} :
                OverlapPair{handle(work.second), handle(work.first), second.entity, first.entity};
            continue;
        }
        if (stack_limit - pending.size() < 2) {
            result.traversal_exhausted = true;
            break;
        }
        if (second.height == 0 || (first.height != 0 && cost(first.box) >= cost(second.box))) {
            pending.push_back({first.right, work.second});
            pending.push_back({first.left, work.second});
        } else {
            pending.push_back({work.first, second.right});
            pending.push_back({work.first, second.left});
        }
    }
    result.traversal_exhausted = result.traversal_exhausted || !pending.empty();
    result.truncated = result.traversal_exhausted;
    return result;
}

void DynamicAabbTree::refit(bool tighten) {
    if (root_ == none) {
        return;
    }
    std::vector<Index> order;
    order.reserve(live_nodes_);
    order.push_back(root_);
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto& node = nodes_[order[position]];
        if (node.height > 0) {
            order.push_back(node.left);
            order.push_back(node.right);
        }
    }
    for (auto current = order.rbegin(); current != order.rend(); ++current) {
        auto& node = nodes_[*current];
        if (tighten && node.height == 0) {
            math::Aabb fat;
            if (fatten(node.tight, {}, fat)) {
                node.box = fat;
            }
        }
        refit_node(*current);
    }
}

DynamicAabbTree::Index DynamicAabbTree::build_subtree(std::span<Index> leaves,
                                                    std::span<Index> branches,
                                                    std::size_t& branch_cursor,
                                                    Index parent) noexcept {
    if (leaves.size() == 1) {
        nodes_[leaves.front()].parent = parent;
        return leaves.front();
    }
    math::Aabb centers;
    for (const auto leaf : leaves) {
        const auto& box = nodes_[leaf].box;
        centers = math::include(centers, {midpoint(box, 0), midpoint(box, 1), midpoint(box, 2)});
    }
    const auto extent = math::extent(centers);
    std::size_t axis = extent.y > extent.x ? 1 : 0;
    if (extent.z > extent[axis]) {
        axis = 2;
    }
    const std::size_t split = leaves.size() / 2;
    auto middle = leaves.begin() + static_cast<std::ptrdiff_t>(split);
    std::nth_element(leaves.begin(), middle, leaves.end(), [this, axis](Index a, Index b) {
        const double first = midpoint(nodes_[a].box, axis);
        const double second = midpoint(nodes_[b].box, axis);
        return first < second || (first == second && a < b);
    });
    const auto branch = branches[branch_cursor++];
    auto& node = nodes_[branch];
    node.parent = parent;
    node.left = build_subtree(leaves.first(split), branches, branch_cursor, branch);
    node.right = build_subtree(leaves.subspan(split), branches, branch_cursor, branch);
    refit_node(branch);
    return branch;
}

void DynamicAabbTree::rebuild(bool tighten) {
    if (root_ == none) {
        return;
    }
    std::vector<Index> leaves;
    std::vector<Index> branches;
    leaves.reserve(proxies_);
    branches.reserve(proxies_ - 1);
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const auto height = nodes_[index].height;
        if (height == 0) {
            leaves.push_back(static_cast<Index>(index));
        } else if (height > 0) {
            branches.push_back(static_cast<Index>(index));
        }
    }
    if (tighten) {
        for (const auto leaf : leaves) {
            math::Aabb fat;
            if (fatten(nodes_[leaf].tight, {}, fat)) {
                nodes_[leaf].box = fat;
            }
        }
    }
    std::size_t cursor{};
    root_ = build_subtree(leaves, branches, cursor, none);
    increment(rebuilds_);
}

SpatialIndex::SpatialIndex(ecs::Registry& registry, scene::Scene& scene,
                           IndexConfig configuration)
    : registry_(registry), scene_(scene), config_(configuration), tree_(configuration.tree) {
    if (&scene.registry() != &registry || config_.change_limit == 0 ||
        config_.change_limit > 1048576) {
        throw std::invalid_argument("spatial index requires one shared registry and bounded history");
    }
    entries_.reserve(std::min(static_cast<std::size_t>(config_.tree.capacity),
                             static_cast<std::size_t>(registry_.capacity())));
}

bool SpatialIndex::eligible(ecs::Entity entity) const noexcept {
    if (!registry_.alive(entity) || !registry_.has<ecs::BoundsComponent>(entity) ||
        !scene_.contains(entity)) {
        return false;
    }
    if (config_.include_disabled) {
        return true;
    }
    auto current = entity;
    std::size_t depth{};
    while (current && depth < scene_.limits().max_depth) {
        if (!registry_.enabled(current)) {
            return false;
        }
        current = scene_.parent(current);
        ++depth;
    }
    return !current;
}

void SpatialIndex::erase(ecs::Entity entity, SyncReport& report) noexcept {
    const auto found = entries_.find(entity);
    if (found != entries_.end()) {
        (void)tree_.remove(found->second.proxy);
        entries_.erase(found);
        ++report.removed;
    }
}

void SpatialIndex::synchronize_entity(ecs::Entity entity, SyncReport& report) {
    ++report.processed;
    if (!eligible(entity)) {
        erase(entity, report);
        return;
    }
    auto* bounds = registry_.get<ecs::BoundsComponent>(entity);
    const auto* world = scene_.cached_world_matrix(entity);
    const auto transformed = world ? math::transform_bounds(*world, bounds->local) : math::Aabb{};
    if (!math::valid(transformed) ||
        !math::valid(math::expanded(transformed, config_.tree.fat_margin))) {
        erase(entity, report);
        ++report.rejected;
        return;
    }
    if (!same_bounds(bounds->world, transformed)) {
        bounds->world = transformed;
        (void)registry_.mark_changed<ecs::BoundsComponent>(entity);
    }
    const auto found = entries_.find(entity);
    if (found == entries_.end()) {
        const auto proxy_handle = tree_.insert(entity, transformed, registry_.layers(entity));
        if (!proxy_handle) {
            ++report.capacity_failures;
            return;
        }
        try {
            entries_.emplace(entity, Entry{proxy_handle, registry_.entity_revision(entity)});
        } catch (...) {
            (void)tree_.remove(proxy_handle);
            throw;
        }
        ++report.added;
        return;
    }
    const auto previous = tree_.get(found->second.proxy);
    const auto displacement = previous ? math::Vec3{
        midpoint(transformed, 0) - midpoint(previous->bounds, 0),
        midpoint(transformed, 1) - midpoint(previous->bounds, 1),
        midpoint(transformed, 2) - midpoint(previous->bounds, 2)} : math::Vec3{};
    auto changed = tree_.update(found->second.proxy, transformed, displacement);
    if (changed == ProxyUpdate::InvalidBounds) {
        changed = tree_.update(found->second.proxy, transformed);
    }
    if (changed == ProxyUpdate::InvalidBounds || changed == ProxyUpdate::InvalidHandle) {
        erase(entity, report);
        ++report.rejected;
        return;
    }
    const auto layers = registry_.layers(entity);
    const bool layers_changed = previous && previous->layers != layers;
    (void)tree_.set_layers(found->second.proxy, layers);
    found->second.entity_revision = registry_.entity_revision(entity);
    if (changed == ProxyUpdate::Unchanged && !layers_changed) {
        ++report.unchanged;
    } else {
        ++report.updated;
    }
}

SyncReport SpatialIndex::sync(bool force_full_scan) {
    SyncReport report;
    const auto updated = scene_.update();
    if (updated.error != scene::SceneError::None) {
        report.scene_busy = true;
        report.registry_revision = registry_.revision();
        report.scene_revision = scene_.revision();
        return report;
    }
    const auto scene_revision = scene_.revision();
    const auto registry_revision = registry_.revision();
    report.full_scan = force_full_scan || !initialized_ || retry_full_scan_ ||
        scene_revision != last_.scene_revision ||
        scene_revision == std::numeric_limits<std::uint64_t>::max() ||
        registry_revision == std::numeric_limits<std::uint64_t>::max();
    std::vector<ecs::Entity> changed;
    if (!report.full_scan && registry_revision != last_.registry_revision) {
        const auto batch = registry_.changes_since(last_.registry_revision, config_.change_limit);
        report.history_lost = batch.history_lost;
        report.full_scan = batch.history_lost || batch.newest_revision != registry_revision;
        if (!report.full_scan) {
            changed.reserve(batch.events.size());
            for (const auto& event : batch.events) {
                changed.push_back(event.entity);
            }
            std::sort(changed.begin(), changed.end());
            changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        }
    }
    if (report.full_scan) {
        changed = registry_.view<ecs::BoundsComponent>(true);
        for (auto current = entries_.begin(); current != entries_.end();) {
            if (!eligible(current->first)) {
                (void)tree_.remove(current->second.proxy);
                current = entries_.erase(current);
                ++report.removed;
            } else {
                ++current;
            }
        }
    }
    for (const auto entity : changed) {
        const auto found = entries_.find(entity);
        if (!report.full_scan && found != entries_.end() &&
            found->second.entity_revision == registry_.entity_revision(entity)) {
            ++report.unchanged;
            continue;
        }
        synchronize_entity(entity, report);
    }
    report.registry_revision = registry_.revision();
    report.scene_revision = scene_.revision();
    initialized_ = true;
    retry_full_scan_ = report.capacity_failures != 0;
    last_ = report;
    return report;
}

const SyncReport& SpatialIndex::last_sync() const noexcept {
    return last_;
}

const DynamicAabbTree& SpatialIndex::tree() const noexcept {
    return tree_;
}

std::optional<ProxyHandle> SpatialIndex::proxy(ecs::Entity entity) const noexcept {
    const auto found = entries_.find(entity);
    if (!eligible(entity) || found == entries_.end() || !tree_.contains(found->second.proxy)) {
        return std::nullopt;
    }
    return found->second.proxy;
}

std::optional<math::Aabb> SpatialIndex::world_bounds(ecs::Entity entity) const noexcept {
    const auto proxy_handle = proxy(entity);
    const auto value = proxy_handle ? tree_.get(*proxy_handle) : std::nullopt;
    return value ? std::optional{value->bounds} : std::nullopt;
}

QueryReport SpatialIndex::query(const math::Aabb& bounds, std::span<QueryHit> output,
                               QueryFilter filter) {
    if (sync().scene_busy) {
        return stale_report();
    }
    auto result = tree_.query(bounds, output, filter);
    result.truncated = result.truncated || retry_full_scan_;
    return result;
}

QueryReport SpatialIndex::query_frustum(const math::Frustum& frustum,
                                       std::span<QueryHit> output, QueryFilter filter) {
    if (sync().scene_busy) {
        return stale_report();
    }
    auto result = tree_.query_frustum(frustum, output, filter);
    result.truncated = result.truncated || retry_full_scan_;
    return result;
}

QueryReport SpatialIndex::raycast(const math::Ray& ray, double maximum,
                                 std::span<RayHit> output, QueryFilter filter, double minimum) {
    if (sync().scene_busy) {
        return stale_report();
    }
    auto result = tree_.raycast(ray, maximum, output, filter, minimum);
    result.truncated = result.truncated || retry_full_scan_;
    return result;
}

QueryReport SpatialIndex::collect_pairs(std::span<OverlapPair> output, QueryFilter filter) {
    if (sync().scene_busy) {
        return stale_report();
    }
    auto result = tree_.collect_pairs(output, filter, false);
    result.truncated = result.truncated || retry_full_scan_;
    return result;
}

void SpatialIndex::rebuild(bool tighten) {
    if (!sync().scene_busy) {
        tree_.rebuild(tighten);
    }
}

void SpatialIndex::clear() noexcept {
    tree_.clear();
    entries_.clear();
    initialized_ = false;
    retry_full_scan_ = false;
    last_ = {};
}

void register_index(core::Kernel& kernel) {
    if (kernel.services().find<SpatialIndex>()) {
        return;
    }
    auto* registry = kernel.services().find<ecs::Registry>();
    auto* scene_service = kernel.services().find<scene::Scene>();
    if (!registry || !scene_service) {
        throw std::logic_error("spatial registration requires ECS and scene services");
    }
    IndexConfig configured;
    configured.tree.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        kernel.config().integer("spatial.capacity", 65536), 0,
        static_cast<std::int64_t>(DynamicAabbTree::maximum_capacity)));
    configured.tree.fat_margin = kernel.config().number("spatial.fat_margin", 0.1);
    configured.tree.displacement_multiplier = kernel.config().number("spatial.prediction", 2.0);
    configured.tree.traversal_limit = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("spatial.visit_limit", 4194304), 1, 16777216));
    configured.include_disabled = kernel.config().boolean("spatial.include_disabled", false);
    (void)kernel.services().emplace<SpatialIndex>(*registry, *scene_service, configured);
    (void)kernel.register_module({"spatial.index", "Generation-checked dynamic AABB tree and scene queries", 1});
    (void)kernel.add_system({"spatial.sync", core::Phase::Visibility, -10000, {"scene.transforms.post"}},
        [](core::Kernel& host, const core::Frame&) {
            auto& index = host.services().get<SpatialIndex>();
            const auto synchronized = index.sync();
            host.metric("spatial.proxies", static_cast<double>(index.tree().size()));
            host.metric("spatial.updated", static_cast<double>(synchronized.updated));
            host.metric("spatial.rejected", static_cast<double>(synchronized.rejected));
            host.metric("spatial.capacity_failures", static_cast<double>(synchronized.capacity_failures));
        });
    (void)kernel.add_command("spatial.query", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 6) {
            return core::CommandResult{false, "usage: spatial.query min_x min_y min_z max_x max_y max_z"};
        }
        math::Aabb bounds;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!number(args[axis], bounds.min[axis]) || !number(args[axis + 3], bounds.max[axis])) {
                return core::CommandResult{false, "bounds require six finite numbers"};
            }
        }
        std::array<QueryHit, 128> hits{};
        const auto report = host.services().get<SpatialIndex>().query(bounds, hits);
        if (report.invalid_input || report.stale) {
            return core::CommandResult{false, "invalid bounds or scene synchronization unavailable"};
        }
        std::ostringstream message;
        message << query_summary(report);
        for (std::size_t index = 0; index < report.written; ++index) {
            const auto entity = hits[index].entity;
            message << '\n' << entity.index << ':' << entity.generation;
        }
        return core::CommandResult{true, message.str()};
    }, "Query scene world AABBs; reports at most 128 generation-qualified entities");
    (void)kernel.add_command("spatial.ray", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 7) {
            return core::CommandResult{false, "usage: spatial.ray ox oy oz dx dy dz maximum"};
        }
        math::Ray ray;
        double maximum{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!number(args[axis], ray.origin[axis]) || !number(args[axis + 3], ray.direction[axis])) {
                return core::CommandResult{false, "ray requires finite origin and direction"};
            }
        }
        if (!number(args[6], maximum) || maximum < 0.0) {
            return core::CommandResult{false, "maximum ray parameter must be finite and nonnegative"};
        }
        std::array<RayHit, 128> hits{};
        const auto report = host.services().get<SpatialIndex>().raycast(ray, maximum, hits);
        if (report.invalid_input || report.stale) {
            return core::CommandResult{false, "invalid ray or scene synchronization unavailable"};
        }
        std::ostringstream message;
        message << query_summary(report);
        for (std::size_t index = 0; index < report.written; ++index) {
            message << '\n' << hits[index].entity.index << ':' << hits[index].entity.generation
                    << " parameter=" << hits[index].hit.distance;
        }
        return core::CommandResult{true, message.str()};
    }, "Raycast scene bounds; returns the nearest 128 visited hits in parameter order");
    (void)kernel.add_command("spatial.stats", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: spatial.stats"};
        }
        auto& index = host.services().get<SpatialIndex>();
        const auto synchronized = index.sync();
        const auto stats = index.tree().statistics();
        std::ostringstream message;
        message << "proxies=" << stats.proxies << " nodes=" << stats.nodes
                << " capacity=" << stats.capacity << " height=" << stats.height
                << " balance=" << stats.maximum_balance << " area_ratio=" << stats.surface_area_ratio
                << " memory_bytes=" << stats.memory_bytes << " retired=" << stats.retired_nodes
                << " rotations=" << stats.rotations << " reinsertions=" << stats.reinsertions
                << " rejected=" << synchronized.rejected << " capacity_failures=" << synchronized.capacity_failures;
        return core::CommandResult{!synchronized.scene_busy, message.str()};
    }, "Inspect current tree structure, allocation bounds and scene synchronization");
}

} // namespace ge::spatial
