#include <ge/render/graph.hpp>
#include <ge/core/runtime.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::render {
namespace {

constexpr auto unused = std::numeric_limits<std::size_t>::max();
std::atomic<std::uint64_t> graph_owners{1};

bool reads(GraphAccess access) noexcept {
    return access == GraphAccess::Read || access == GraphAccess::ReadWrite;
}

bool writes(GraphAccess access) noexcept {
    return access == GraphAccess::Write || access == GraphAccess::ReadWrite;
}

bool multiply(std::size_t first, std::size_t second, std::size_t& result) noexcept {
    if (second != 0 && first > std::numeric_limits<std::size_t>::max() / second) {
        return false;
    }
    result = first * second;
    return true;
}

bool add_bounded(std::size_t& total, std::size_t amount, std::size_t maximum) noexcept {
    if (total > maximum || amount > maximum - total) {
        return false;
    }
    total += amount;
    return true;
}

bool valid_name(std::string_view name, std::size_t maximum) noexcept {
    if (name.empty() || name.size() > maximum) { return false; }
    return std::none_of(name.begin(), name.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value < 33U || value == 127U;
    });
}

std::string bounded_message(std::string_view message) {
    return std::string(message.substr(0, 512));
}

void increment(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) { ++counter; }
}

} // namespace

GraphResource::operator bool() const noexcept {
    return owner != 0 && index != std::numeric_limits<std::uint32_t>::max();
}

GraphPass::operator bool() const noexcept {
    return owner != 0 && index != std::numeric_limits<std::uint32_t>::max();
}

GraphResult::operator bool() const noexcept { return error == GraphError::None; }

std::string_view describe(GraphError error) noexcept {
    switch (error) {
    case GraphError::None: return "none";
    case GraphError::InvalidLimits: return "invalid limits";
    case GraphError::InvalidName: return "invalid name";
    case GraphError::InvalidHandle: return "invalid graph handle";
    case GraphError::InvalidDescriptor: return "invalid resource or pass descriptor";
    case GraphError::DuplicateName: return "duplicate name";
    case GraphError::Capacity: return "graph capacity exhausted";
    case GraphError::Busy: return "graph is executing";
    case GraphError::MissingService: return "required render service is missing";
    case GraphError::MissingDependency: return "enabled pass depends on a disabled pass";
    case GraphError::Cycle: return "pass dependency cycle";
    case GraphError::ResourceConflict: return "unordered resource conflict";
    case GraphError::ImportedAlias: return "imported resource storage overlaps";
    case GraphError::UninitializedRead: return "resource read before initialization";
    case GraphError::InvalidImport: return "invalid imported storage";
    case GraphError::MemoryBudget: return "transient memory budget exhausted";
    case GraphError::WorkLimit: return "resource operation work limit";
    case GraphError::AllocationFailure: return "graph allocation failed";
    case GraphError::UndeclaredAccess: return "callback access violates its declaration";
    case GraphError::CallbackFailure: return "pass callback failed";
    case GraphError::CallbackException: return "pass callback threw";
    case GraphError::NotCompiled: return "graph is not compiled";
    }
    return "unknown graph error";
}

std::string_view describe(GraphResourceKind kind) noexcept {
    switch (kind) {
    case GraphResourceKind::Image: return "image";
    case GraphResourceKind::Buffer: return "buffer";
    case GraphResourceKind::Depth: return "depth";
    }
    return "unknown";
}

std::string_view describe(GraphPassState state) noexcept {
    switch (state) {
    case GraphPassState::Pending: return "pending";
    case GraphPassState::Succeeded: return "succeeded";
    case GraphPassState::Failed: return "failed";
    case GraphPassState::Skipped: return "skipped";
    case GraphPassState::Disabled: return "disabled";
    }
    return "unknown";
}

bool valid(const GraphLimits& limits) noexcept {
    return limits.resources > 0 && limits.resources <= 1024 &&
        limits.passes > 0 && limits.passes <= 512 &&
        limits.uses > 0 && limits.uses <= 65536 &&
        limits.dependencies > 0 && limits.dependencies <= 65536 &&
        limits.transient_slots > 0 && limits.transient_slots <= limits.resources &&
        limits.transient_bytes > 0 && limits.logical_transient_bytes >= limits.transient_bytes &&
        limits.transfer_bytes > 0 && limits.name_bytes > 0 && limits.name_bytes <= 4096 &&
        limits.images.max_dimension > 0 && limits.images.max_dimension <= 16384 &&
        limits.images.max_pixels > 0 && limits.images.max_pixels <= 16777216;
}

struct RenderGraph::Impl {
    struct Resource {
        std::string name;
        GraphResourceKind kind{GraphResourceKind::Image};
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t bytes{};
        Image* image{};
        Framebuffer* framebuffer{};
        std::span<std::byte> buffer;
        bool imported{};
        bool initialized{};
    };
    struct Pass {
        GraphPassDescription description;
        GraphCallback callback;
        GraphPassState state{GraphPassState::Pending};
        std::uint64_t callback_revision{1};
    };
    struct Output {
        std::string name;
        GraphResource resource;
    };
    struct Physical {
        GraphResourceKind kind{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t bytes{};
        std::size_t last_use{};
        std::unique_ptr<Image> image;
        std::vector<std::byte> buffer;
        std::vector<double> depth;
    };
    struct Plan {
        std::vector<GraphPass> schedule;
        std::vector<GraphLifetime> lifetimes;
        std::vector<std::size_t> slots;
        std::vector<Physical> physical;
    };
    explicit Impl(GraphLimits value) : limits(value), owner(graph_owners.fetch_add(1)) {
        if (!valid(limits)) { throw std::invalid_argument("invalid render graph limits"); }
        if (owner == 0) { throw std::overflow_error("render graph owner space exhausted"); }
    }
    GraphLimits limits;
    std::uint64_t owner;
    std::vector<Resource> resources;
    std::vector<Pass> passes;
    std::vector<Output> outputs;
    std::unique_ptr<Plan> plan;
    GraphResult result;
    GraphStatistics statistics;
    std::uint64_t execution{};
    std::size_t current_pass{unused};
    GraphResource access_resource{};
    bool access_failed{};
    bool dirty{true};
    bool running{};
    bool executed{};
    bool frame_success{};

    GraphResource resource_handle(std::size_t index) const noexcept {
        return {owner, static_cast<std::uint32_t>(index)};
    }
    GraphPass pass_handle(std::size_t index) const noexcept {
        return {owner, static_cast<std::uint32_t>(index)};
    }
    Resource* resolve(GraphResource handle) noexcept {
        return handle.owner == owner && handle.index < resources.size() ? &resources[handle.index] : nullptr;
    }
    const Resource* resolve(GraphResource handle) const noexcept {
        return handle.owner == owner && handle.index < resources.size() ? &resources[handle.index] : nullptr;
    }
    Pass* resolve(GraphPass handle) noexcept {
        return handle.owner == owner && handle.index < passes.size() ? &passes[handle.index] : nullptr;
    }
    const Pass* resolve(GraphPass handle) const noexcept {
        return handle.owner == owner && handle.index < passes.size() ? &passes[handle.index] : nullptr;
    }
    bool fail(GraphError error, std::string message = {}, GraphPass pass = {}, GraphResource resource = {}) {
        result = {error, pass, resource, std::move(message), false, false};
        return false;
    }
    bool edit() {
        return running ? fail(GraphError::Busy) : true;
    }
    void changed() noexcept {
        dirty = true;
        executed = false;
        frame_success = false;
    }
    bool exported(GraphResource handle) const noexcept {
        return std::any_of(outputs.begin(), outputs.end(), [handle](const Output& value) {
            return value.resource == handle;
        });
    }
    std::size_t dependencies() const noexcept {
        std::size_t count{};
        for (const auto& pass : passes) { count += pass.description.after.size(); }
        return count;
    }
    std::size_t uses() const noexcept {
        std::size_t count{};
        for (const auto& pass : passes) { count += pass.description.resources.size(); }
        return count;
    }
    GraphResource append(Resource resource) {
        if (!edit()) { return {}; }
        if (!valid_name(resource.name, limits.name_bytes)) {
            fail(GraphError::InvalidName);
            return {};
        }
        for (const auto& existing : resources) {
            if (existing.name == resource.name) {
                fail(GraphError::DuplicateName, resource.name);
                return {};
            }
        }
        if (resources.size() >= limits.resources) { fail(GraphError::Capacity); return {}; }
        try {
            resources.push_back(std::move(resource));
            changed();
            result = {};
            return resource_handle(resources.size() - 1);
        } catch (const std::bad_alloc&) { fail(GraphError::AllocationFailure); }
        catch (const std::length_error&) { fail(GraphError::AllocationFailure); }
        return {};
    }
    bool extent(std::uint32_t width, std::uint32_t height, GraphResourceKind kind, std::size_t& bytes) const noexcept {
        std::size_t pixels{};
        return width != 0 && height != 0 && Image::valid_dimensions(width, height, limits.images) &&
            multiply(width, height, pixels) &&
            multiply(pixels, kind == GraphResourceKind::Image ? sizeof(Color) : sizeof(double), bytes);
    }
    GraphResource create(std::string name, GraphResourceKind kind, std::uint32_t width,
                         std::uint32_t height, std::size_t bytes) {
        if (!edit()) { return {}; }
        if ((kind == GraphResourceKind::Buffer && bytes == 0) ||
            (kind != GraphResourceKind::Buffer && !extent(width, height, kind, bytes))) {
            fail(GraphError::InvalidDescriptor);
            return {};
        }
        if (bytes > limits.transient_bytes) { fail(GraphError::MemoryBudget); return {}; }
        Resource resource;
        resource.name = std::move(name);
        resource.kind = kind;
        resource.width = width;
        resource.height = height;
        resource.bytes = bytes;
        return append(std::move(resource));
    }
    Image* image(std::size_t index) noexcept {
        auto& resource = resources[index];
        if (resource.imported) { return resource.image; }
        if (!plan || plan->slots[index] == unused) { return nullptr; }
        return plan->physical[plan->slots[index]].image.get();
    }
    std::span<std::byte> buffer(std::size_t index) noexcept {
        auto& resource = resources[index];
        if (resource.imported) { return resource.buffer; }
        if (!plan || plan->slots[index] == unused) { return {}; }
        return std::span<std::byte>{plan->physical[plan->slots[index]].buffer}.first(resource.bytes);
    }
    std::span<double> depth(std::size_t index) noexcept {
        auto& resource = resources[index];
        if (resource.imported) { return resource.framebuffer->depth(); }
        if (!plan || plan->slots[index] == unused) { return {}; }
        return std::span<double>{plan->physical[plan->slots[index]].depth}.first(resource.bytes / sizeof(double));
    }
    std::size_t bytes(const Resource& resource) const noexcept {
        if (!resource.imported) { return resource.bytes; }
        if (resource.kind == GraphResourceKind::Image) { return resource.image->byte_size(); }
        if (resource.kind == GraphResourceKind::Depth) { return resource.framebuffer->depth().size_bytes(); }
        return resource.buffer.size();
    }
    std::pair<std::uintptr_t, std::size_t> memory(const Resource& resource) const noexcept {
        if (resource.kind == GraphResourceKind::Image) {
            return {reinterpret_cast<std::uintptr_t>(resource.image->pixels().data()), bytes(resource)};
        }
        if (resource.kind == GraphResourceKind::Depth) {
            return {reinterpret_cast<std::uintptr_t>(resource.framebuffer->depth().data()), bytes(resource)};
        }
        return {reinterpret_cast<std::uintptr_t>(resource.buffer.data()), resource.buffer.size()};
    }
    bool validate_imports() {
        for (std::size_t index = 0; index < resources.size(); ++index) {
            const auto& resource = resources[index];
            if (!resource.imported) { continue; }
            const bool valid_image = resource.kind != GraphResourceKind::Image ||
                (resource.image && !resource.image->empty() && Image::valid_dimensions(
                    resource.image->width(), resource.image->height(), limits.images));
            const bool valid_depth = resource.kind != GraphResourceKind::Depth ||
                (resource.framebuffer && resource.framebuffer->valid());
            if (!valid_image || !valid_depth) {
                return fail(GraphError::InvalidImport, resource.name, {}, resource_handle(index));
            }
            const auto [begin, size] = memory(resource);
            if (size > std::numeric_limits<std::uintptr_t>::max() - begin) {
                return fail(GraphError::InvalidImport, resource.name, {}, resource_handle(index));
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (!resources[previous].imported) { continue; }
                const auto [other_begin, other_size] = memory(resources[previous]);
                if (size != 0 && other_size != 0 && begin < other_begin + other_size && other_begin < begin + size) {
                    return fail(GraphError::ImportedAlias, resources[previous].name + " overlaps " + resource.name,
                                {}, resource_handle(index));
                }
            }
        }
        return true;
    }
    Resource* access(GraphResource handle, GraphPass pass, std::uint64_t serial,
                     bool write, GraphResourceKind kind) noexcept {
        auto* resource = resolve(handle);
        if (running && serial == execution && pass.owner == owner && pass.index == current_pass &&
            resource && resource->kind == kind) {
            for (const auto& use : passes[current_pass].description.resources) {
                if (use.resource == handle && (write ? writes(use.access) : reads(use.access)) &&
                    (write || resource->initialized)) { return resource; }
            }
        }
        access_failed = true;
        access_resource = handle;
        return nullptr;
    }
    bool available(GraphResource handle, GraphResourceKind kind, bool partial) const noexcept {
        const auto* resource = resolve(handle);
        return executed && !dirty && !running && (frame_success || partial) && resource &&
            resource->initialized && resource->kind == kind;
    }
};

RenderGraph::RenderGraph(GraphLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
RenderGraph::~RenderGraph() = default;
const GraphLimits& RenderGraph::limits() const noexcept { return impl_->limits; }
const GraphResult& RenderGraph::last_result() const noexcept { return impl_->result; }
const GraphStatistics& RenderGraph::statistics() const noexcept { return impl_->statistics; }
bool RenderGraph::compiled() const noexcept { return impl_->plan && !impl_->dirty; }

bool RenderGraph::clear() {
    if (!impl_->edit()) { return false; }
    const auto owner = graph_owners.fetch_add(1);
    if (owner == 0) { return impl_->fail(GraphError::Capacity); }
    impl_->resources.clear();
    impl_->passes.clear();
    impl_->outputs.clear();
    impl_->plan.reset();
    impl_->owner = owner;
    impl_->statistics = {};
    impl_->result = {};
    impl_->changed();
    return true;
}

GraphResource RenderGraph::import_image(std::string name, Image& image) {
    Impl::Resource resource;
    resource.name = std::move(name);
    resource.kind = GraphResourceKind::Image;
    resource.imported = true;
    resource.image = &image;
    return impl_->append(std::move(resource));
}

GraphResource RenderGraph::import_buffer(std::string name, std::span<std::byte> bytes) {
    Impl::Resource resource;
    resource.name = std::move(name);
    resource.kind = GraphResourceKind::Buffer;
    resource.imported = true;
    resource.buffer = bytes;
    return impl_->append(std::move(resource));
}

GraphResource RenderGraph::import_depth(std::string name, Framebuffer& framebuffer) {
    Impl::Resource resource;
    resource.name = std::move(name);
    resource.kind = GraphResourceKind::Depth;
    resource.imported = true;
    resource.framebuffer = &framebuffer;
    return impl_->append(std::move(resource));
}

GraphResource RenderGraph::create_image(std::string name, std::uint32_t width, std::uint32_t height) {
    return impl_->create(std::move(name), GraphResourceKind::Image, width, height, 0);
}
GraphResource RenderGraph::create_buffer(std::string name, std::size_t bytes) {
    return impl_->create(std::move(name), GraphResourceKind::Buffer, 0, 0, bytes);
}
GraphResource RenderGraph::create_depth(std::string name, std::uint32_t width, std::uint32_t height) {
    return impl_->create(std::move(name), GraphResourceKind::Depth, width, height, 0);
}

bool RenderGraph::rebind_image(GraphResource handle, Image& image) {
    if (!impl_->edit()) { return false; }
    auto* resource = impl_->resolve(handle);
    if (!resource || !resource->imported || resource->kind != GraphResourceKind::Image) {
        return impl_->fail(GraphError::InvalidHandle, {}, {}, handle);
    }
    resource->image = &image;
    impl_->changed();
    return true;
}

bool RenderGraph::rebind_buffer(GraphResource handle, std::span<std::byte> bytes) {
    if (!impl_->edit()) { return false; }
    auto* resource = impl_->resolve(handle);
    if (!resource || !resource->imported || resource->kind != GraphResourceKind::Buffer) {
        return impl_->fail(GraphError::InvalidHandle, {}, {}, handle);
    }
    resource->buffer = bytes;
    impl_->changed();
    return true;
}

bool RenderGraph::rebind_depth(GraphResource handle, Framebuffer& framebuffer) {
    if (!impl_->edit()) { return false; }
    auto* resource = impl_->resolve(handle);
    if (!resource || !resource->imported || resource->kind != GraphResourceKind::Depth) {
        return impl_->fail(GraphError::InvalidHandle, {}, {}, handle);
    }
    resource->framebuffer = &framebuffer;
    impl_->changed();
    return true;
}

bool RenderGraph::resize_transient(GraphResource handle, std::uint32_t width, std::uint32_t height) {
    if (!impl_->edit()) { return false; }
    auto* resource = impl_->resolve(handle);
    if (!resource || resource->imported || resource->kind == GraphResourceKind::Buffer) {
        return impl_->fail(GraphError::InvalidHandle, {}, {}, handle);
    }
    std::size_t bytes{};
    if (!impl_->extent(width, height, resource->kind, bytes)) {
        return impl_->fail(GraphError::InvalidDescriptor, {}, {}, handle);
    }
    if (bytes > impl_->limits.transient_bytes) { return impl_->fail(GraphError::MemoryBudget); }
    resource->width = width;
    resource->height = height;
    resource->bytes = bytes;
    impl_->changed();
    return true;
}

bool RenderGraph::resize_buffer(GraphResource handle, std::size_t bytes) {
    if (!impl_->edit()) { return false; }
    auto* resource = impl_->resolve(handle);
    if (!resource || resource->imported || resource->kind != GraphResourceKind::Buffer) {
        return impl_->fail(GraphError::InvalidHandle, {}, {}, handle);
    }
    if (bytes == 0) { return impl_->fail(GraphError::InvalidDescriptor); }
    if (bytes > impl_->limits.transient_bytes) { return impl_->fail(GraphError::MemoryBudget); }
    resource->bytes = bytes;
    impl_->changed();
    return true;
}

std::optional<GraphResource> RenderGraph::find_resource(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < impl_->resources.size(); ++index) {
        if (impl_->resources[index].name == name) { return impl_->resource_handle(index); }
    }
    return std::nullopt;
}

std::optional<GraphPass> RenderGraph::find_pass(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < impl_->passes.size(); ++index) {
        if (impl_->passes[index].description.name == name) { return impl_->pass_handle(index); }
    }
    return std::nullopt;
}

std::optional<GraphResourceInfo> RenderGraph::resource_info(GraphResource handle) const noexcept {
    const auto* resource = impl_->resolve(handle);
    if (!resource) { return std::nullopt; }
    auto width = resource->width;
    auto height = resource->height;
    if (resource->imported && resource->kind == GraphResourceKind::Image) {
        width = resource->image->width();
        height = resource->image->height();
    } else if (resource->imported && resource->kind == GraphResourceKind::Depth) {
        width = resource->framebuffer->width();
        height = resource->framebuffer->height();
    }
    return GraphResourceInfo{handle, resource->name, resource->kind, width, height,
        impl_->bytes(*resource), resource->imported, impl_->exported(handle),
        impl_->executed && !impl_->dirty && resource->initialized};
}

std::optional<GraphPassInfo> RenderGraph::pass_info(GraphPass handle) const noexcept {
    const auto* pass = impl_->resolve(handle);
    if (!pass) { return std::nullopt; }
    return GraphPassInfo{handle, pass->description.name, pass->description.resources,
        pass->description.after, pass->description.priority, pass->description.enabled, pass->state,
        pass->description.callback_owner, pass->callback_revision};
}

GraphPass RenderGraph::add_pass(GraphPassDescription description, GraphCallback callback) {
    if (!impl_->edit()) { return {}; }
    if (!valid_name(description.name, impl_->limits.name_bytes)) {
        impl_->fail(GraphError::InvalidName);
        return {};
    }
    if (find_pass(description.name)) { impl_->fail(GraphError::DuplicateName, description.name); return {}; }
    auto use_count = impl_->uses();
    auto dependency_count = impl_->dependencies();
    if (impl_->passes.size() >= impl_->limits.passes ||
        !add_bounded(use_count, description.resources.size(), impl_->limits.uses) ||
        !add_bounded(dependency_count, description.after.size(), impl_->limits.dependencies)) {
        impl_->fail(GraphError::Capacity);
        return {};
    }
    if (!callback) { impl_->fail(GraphError::InvalidDescriptor); return {}; }
    for (std::size_t index = 0; index < description.resources.size(); ++index) {
        const auto& use = description.resources[index];
        if (!impl_->resolve(use.resource) || (!reads(use.access) && !writes(use.access))) {
            impl_->fail(GraphError::InvalidHandle, {}, {}, use.resource);
            return {};
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (description.resources[prior].resource == use.resource) {
                impl_->fail(GraphError::InvalidDescriptor, "declare each resource once using ReadWrite when needed");
                return {};
            }
        }
    }
    for (std::size_t index = 0; index < description.after.size(); ++index) {
        if (!impl_->resolve(description.after[index])) {
            impl_->fail(GraphError::InvalidHandle, {}, description.after[index]);
            return {};
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (description.after[prior] == description.after[index]) {
                impl_->fail(GraphError::InvalidDescriptor, "duplicate pass dependency");
                return {};
            }
        }
    }
    try {
        impl_->passes.push_back({std::move(description), std::move(callback), GraphPassState::Pending});
        impl_->changed();
        impl_->result = {};
        return impl_->pass_handle(impl_->passes.size() - 1);
    } catch (const std::bad_alloc&) { impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { impl_->fail(GraphError::AllocationFailure); }
    return {};
}

bool RenderGraph::add_dependency(GraphPass handle, GraphPass prerequisite) {
    if (!impl_->edit()) { return false; }
    auto* pass = impl_->resolve(handle);
    if (!pass || !impl_->resolve(prerequisite)) { return impl_->fail(GraphError::InvalidHandle); }
    if (handle == prerequisite) { return impl_->fail(GraphError::Cycle, {}, handle); }
    auto& after = pass->description.after;
    if (std::find(after.begin(), after.end(), prerequisite) != after.end()) { return true; }
    if (impl_->dependencies() >= impl_->limits.dependencies) { return impl_->fail(GraphError::Capacity); }
    try {
        after.push_back(prerequisite);
        impl_->changed();
        return true;
    } catch (const std::bad_alloc&) { return impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->fail(GraphError::AllocationFailure); }
}

bool RenderGraph::replace_callback(GraphPass handle, GraphCallback callback) {
    if (!impl_->edit()) { return false; }
    auto* pass = impl_->resolve(handle);
    if (!pass) { return impl_->fail(GraphError::InvalidHandle); }
    if (!callback) { return impl_->fail(GraphError::InvalidDescriptor); }
    if (pass->callback_revision == std::numeric_limits<std::uint64_t>::max()) {
        return impl_->fail(GraphError::Capacity, "callback revision space exhausted", handle);
    }
    pass->callback = std::move(callback);
    pass->description.callback_owner = 0;
    ++pass->callback_revision;
    impl_->changed();
    impl_->result = {};
    return true;
}

bool RenderGraph::replace_callback_if(GraphPass handle, std::uint64_t expected_owner,
                                      std::uint64_t expected_revision, std::uint64_t new_owner,
                                      GraphCallback callback) {
    if (!impl_->edit()) { return false; }
    auto* pass = impl_->resolve(handle);
    if (!pass) { return impl_->fail(GraphError::InvalidHandle); }
    if (!callback) { return impl_->fail(GraphError::InvalidDescriptor); }
    if (expected_revision == 0 || pass->description.callback_owner != expected_owner ||
        pass->callback_revision != expected_revision) {
        return impl_->fail(GraphError::ResourceConflict, "callback ownership stamp changed", handle);
    }
    if (pass->callback_revision == std::numeric_limits<std::uint64_t>::max()) {
        return impl_->fail(GraphError::Capacity, "callback revision space exhausted", handle);
    }
    pass->callback = std::move(callback);
    pass->description.callback_owner = new_owner;
    ++pass->callback_revision;
    impl_->changed();
    impl_->result = {};
    return true;
}

bool RenderGraph::set_enabled(GraphPass handle, bool enabled) {
    if (!impl_->edit()) { return false; }
    auto* pass = impl_->resolve(handle);
    if (!pass) { return impl_->fail(GraphError::InvalidHandle); }
    if (pass->description.enabled != enabled) {
        pass->description.enabled = enabled;
        impl_->changed();
    }
    return true;
}

GraphPass RenderGraph::insert_before(GraphPass anchor, GraphPassDescription description, GraphCallback callback) {
    if (!impl_->edit()) { return {}; }
    auto* existing = impl_->resolve(anchor);
    if (!existing) { impl_->fail(GraphError::InvalidHandle); return {}; }
    try {
        if (std::find(description.after.begin(), description.after.end(), anchor) != description.after.end()) {
            impl_->fail(GraphError::Cycle);
            return {};
        }
        for (const auto predecessor : existing->description.after) {
            if (std::find(description.after.begin(), description.after.end(), predecessor) == description.after.end()) {
                description.after.push_back(predecessor);
            }
        }
        auto dependency_count = impl_->dependencies();
        if (!add_bounded(dependency_count, description.after.size(), impl_->limits.dependencies) ||
            !add_bounded(dependency_count, 1, impl_->limits.dependencies)) {
            impl_->fail(GraphError::Capacity);
            return {};
        }
        existing->description.after.reserve(existing->description.after.size() + 1);
        const auto inserted = add_pass(std::move(description), std::move(callback));
        if (inserted) { impl_->passes[anchor.index].description.after.push_back(inserted); }
        return inserted;
    } catch (const std::bad_alloc&) { impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { impl_->fail(GraphError::AllocationFailure); }
    return {};
}

GraphPass RenderGraph::insert_after(GraphPass anchor, GraphPassDescription description, GraphCallback callback) {
    if (!impl_->edit()) { return {}; }
    if (!impl_->resolve(anchor)) { impl_->fail(GraphError::InvalidHandle); return {}; }
    try {
        if (std::find(description.after.begin(), description.after.end(), anchor) == description.after.end()) {
            description.after.push_back(anchor);
        }
        const auto inserted = add_pass(std::move(description), std::move(callback));
        if (!inserted) { return {}; }
        for (std::size_t index = 0; index < inserted.index; ++index) {
            for (auto& predecessor : impl_->passes[index].description.after) {
                if (predecessor == anchor) { predecessor = inserted; }
            }
        }
        return inserted;
    } catch (const std::bad_alloc&) { impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { impl_->fail(GraphError::AllocationFailure); }
    return {};
}

bool RenderGraph::set_output(std::string name, GraphResource resource) {
    if (!impl_->edit()) { return false; }
    if (!impl_->resolve(resource)) { return impl_->fail(GraphError::InvalidHandle); }
    if (!valid_name(name, impl_->limits.name_bytes)) { return impl_->fail(GraphError::InvalidName); }
    for (auto& entry : impl_->outputs) {
        if (entry.name == name) {
            entry.resource = resource;
            impl_->changed();
            return true;
        }
    }
    if (impl_->outputs.size() >= impl_->limits.resources) { return impl_->fail(GraphError::Capacity); }
    try {
        impl_->outputs.push_back({std::move(name), resource});
        impl_->changed();
        return true;
    } catch (const std::bad_alloc&) { return impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->fail(GraphError::AllocationFailure); }
}

bool RenderGraph::remove_output(std::string_view name) {
    if (!impl_->edit()) { return false; }
    const auto found = std::find_if(impl_->outputs.begin(), impl_->outputs.end(), [name](const Impl::Output& entry) {
        return entry.name == name;
    });
    if (found == impl_->outputs.end()) { return impl_->fail(GraphError::InvalidName); }
    impl_->outputs.erase(found);
    impl_->changed();
    return true;
}

std::optional<GraphResource> RenderGraph::output(std::string_view name) const noexcept {
    for (const auto& entry : impl_->outputs) {
        if (entry.name == name) { return entry.resource; }
    }
    return std::nullopt;
}

std::span<const GraphPass> RenderGraph::schedule() const noexcept {
    return compiled() ? std::span<const GraphPass>{impl_->plan->schedule} : std::span<const GraphPass>{};
}
std::span<const GraphLifetime> RenderGraph::lifetimes() const noexcept {
    return compiled() ? std::span<const GraphLifetime>{impl_->plan->lifetimes} : std::span<const GraphLifetime>{};
}

GraphResult RenderGraph::compile() {
    if (!impl_->edit()) { return impl_->result; }
    impl_->plan.reset();
    impl_->changed();
    increment(impl_->statistics.compilations);
    impl_->statistics.scheduled_passes = 0;
    impl_->statistics.physical_slots = 0;
    impl_->statistics.aliased_resources = 0;
    impl_->statistics.transient_bytes = 0;
    impl_->statistics.logical_transient_bytes = 0;
    try {
        if (!impl_->validate_imports()) { return impl_->result; }
        auto plan = std::make_unique<Impl::Plan>();
        const auto count = impl_->passes.size();
        std::vector<std::uint8_t> reach(count * count, 0);
        std::vector<std::size_t> indegree(count, 0);
        std::vector<std::uint8_t> selected(count, 0);
        std::size_t enabled{};
        for (std::size_t index = 0; index < count; ++index) {
            auto& pass = impl_->passes[index];
            pass.state = pass.description.enabled ? GraphPassState::Pending : GraphPassState::Disabled;
            if (!pass.description.enabled) { continue; }
            ++enabled;
            for (const auto prerequisite : pass.description.after) {
                if (!impl_->passes[prerequisite.index].description.enabled) {
                    impl_->fail(GraphError::MissingDependency, pass.description.name, impl_->pass_handle(index));
                    return impl_->result;
                }
                reach[static_cast<std::size_t>(prerequisite.index) * count + index] = 1;
                ++indegree[index];
            }
        }
        plan->schedule.reserve(enabled);
        while (plan->schedule.size() < enabled) {
            std::size_t next = unused;
            for (std::size_t index = 0; index < count; ++index) {
                if (!impl_->passes[index].description.enabled || selected[index] != 0 || indegree[index] != 0) { continue; }
                if (next == unused || impl_->passes[index].description.priority < impl_->passes[next].description.priority) {
                    next = index;
                }
            }
            if (next == unused) {
                impl_->fail(GraphError::Cycle, "no ready pass remains");
                return impl_->result;
            }
            selected[next] = 1;
            plan->schedule.push_back(impl_->pass_handle(next));
            for (std::size_t target = 0; target < count; ++target) {
                if (reach[next * count + target] != 0) { --indegree[target]; }
            }
        }
        for (auto iterator = plan->schedule.rbegin(); iterator != plan->schedule.rend(); ++iterator) {
            const auto source = static_cast<std::size_t>(iterator->index);
            for (std::size_t successor = 0; successor < count; ++successor) {
                if (reach[source * count + successor] == 0) { continue; }
                for (std::size_t target = 0; target < count; ++target) {
                    if (reach[successor * count + target] != 0) { reach[source * count + target] = 1; }
                }
            }
        }
        plan->slots.assign(impl_->resources.size(), unused);
        std::size_t logical_bytes{};
        for (std::size_t index = 0; index < impl_->resources.size(); ++index) {
            auto& resource = impl_->resources[index];
            const auto handle = impl_->resource_handle(index);
            GraphLifetime lifetime{handle, unused, 0, unused, impl_->bytes(resource), resource.imported, impl_->exported(handle)};
            bool initialized = resource.imported;
            std::vector<std::pair<GraphPass, GraphAccess>> previous;
            for (std::size_t position = 0; position < plan->schedule.size(); ++position) {
                const auto current = plan->schedule[position];
                for (const auto& use : impl_->passes[current.index].description.resources) {
                    if (use.resource != handle) { continue; }
                    for (const auto& [earlier, access] : previous) {
                        if ((writes(access) || writes(use.access)) &&
                            reach[static_cast<std::size_t>(earlier.index) * count + current.index] == 0) {
                            impl_->fail(GraphError::ResourceConflict,
                                impl_->passes[earlier.index].description.name + " / " + impl_->passes[current.index].description.name,
                                current, handle);
                            return impl_->result;
                        }
                    }
                    if (reads(use.access) && !initialized) {
                        impl_->fail(GraphError::UninitializedRead, resource.name, current, handle);
                        return impl_->result;
                    }
                    initialized = initialized || writes(use.access);
                    previous.emplace_back(current, use.access);
                    lifetime.first_use = std::min(lifetime.first_use, position);
                    lifetime.last_use = position;
                }
            }
            if (lifetime.exported && !initialized) {
                impl_->fail(GraphError::UninitializedRead, "export has no producer", {}, handle);
                return impl_->result;
            }
            if (lifetime.first_use == unused && !lifetime.exported) { continue; }
            if (lifetime.first_use == unused) { lifetime.first_use = 0; }
            if (lifetime.exported) { lifetime.last_use = plan->schedule.size(); }
            if (!resource.imported && !add_bounded(logical_bytes, lifetime.bytes, impl_->limits.logical_transient_bytes)) {
                impl_->fail(GraphError::MemoryBudget, "logical transient byte budget", {}, handle);
                return impl_->result;
            }
            plan->lifetimes.push_back(lifetime);
            resource.initialized = false;
        }
        std::vector<std::size_t> allocation_order;
        for (std::size_t index = 0; index < plan->lifetimes.size(); ++index) {
            if (!plan->lifetimes[index].imported) { allocation_order.push_back(index); }
        }
        std::sort(allocation_order.begin(), allocation_order.end(), [&plan](std::size_t left, std::size_t right) {
            const auto& first = plan->lifetimes[left];
            const auto& second = plan->lifetimes[right];
            return first.first_use != second.first_use ? first.first_use < second.first_use : first.resource.index < second.resource.index;
        });
        std::size_t resident_bytes{};
        std::size_t aliases{};
        for (const auto lifetime_index : allocation_order) {
            auto& lifetime = plan->lifetimes[lifetime_index];
            const auto& resource = impl_->resources[lifetime.resource.index];
            std::size_t selected_slot = unused;
            for (std::size_t index = 0; index < plan->physical.size(); ++index) {
                const auto& slot = plan->physical[index];
                if (slot.kind != resource.kind || slot.last_use >= lifetime.first_use || slot.bytes < lifetime.bytes) { continue; }
                if (resource.kind != GraphResourceKind::Buffer && (slot.width != resource.width || slot.height != resource.height)) { continue; }
                if (selected_slot == unused || slot.bytes < plan->physical[selected_slot].bytes) { selected_slot = index; }
            }
            if (selected_slot == unused) {
                if (plan->physical.size() >= impl_->limits.transient_slots ||
                    !add_bounded(resident_bytes, lifetime.bytes, impl_->limits.transient_bytes)) {
                    impl_->fail(GraphError::MemoryBudget, resource.name, {}, lifetime.resource);
                    return impl_->result;
                }
                Impl::Physical slot;
                slot.kind = resource.kind;
                slot.width = resource.width;
                slot.height = resource.height;
                slot.bytes = lifetime.bytes;
                if (slot.kind == GraphResourceKind::Image) {
                    slot.image = std::make_unique<Image>(impl_->limits.images);
                    if (!slot.image->resize(slot.width, slot.height)) {
                        impl_->fail(GraphError::AllocationFailure, resource.name);
                        return impl_->result;
                    }
                } else if (slot.kind == GraphResourceKind::Depth) {
                    slot.depth.resize(slot.bytes / sizeof(double));
                } else {
                    slot.buffer.resize(slot.bytes);
                }
                selected_slot = plan->physical.size();
                plan->physical.push_back(std::move(slot));
            } else { ++aliases; }
            plan->physical[selected_slot].last_use = lifetime.last_use;
            plan->slots[lifetime.resource.index] = selected_slot;
            lifetime.physical_slot = selected_slot;
        }
        impl_->statistics.scheduled_passes = plan->schedule.size();
        impl_->statistics.physical_slots = plan->physical.size();
        impl_->statistics.aliased_resources = aliases;
        impl_->statistics.transient_bytes = resident_bytes;
        impl_->statistics.logical_transient_bytes = logical_bytes;
        impl_->plan = std::move(plan);
        impl_->dirty = false;
        impl_->result = {};
    } catch (const std::bad_alloc&) { impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { impl_->fail(GraphError::AllocationFailure); }
    return impl_->result;
}

GraphContext::GraphContext(RenderGraph& graph, GraphPass pass, const core::Frame& frame,
                           std::uint64_t execution) noexcept
    : graph_(&graph), pass_(pass), frame_(&frame), execution_(execution) {}
const core::Frame& GraphContext::frame() const noexcept { return *frame_; }
GraphPass GraphContext::pass() const noexcept { return pass_; }
const Image* GraphContext::read_image(GraphResource resource) noexcept {
    return graph_->access_image(resource, pass_, execution_, false);
}
Image* GraphContext::write_image(GraphResource resource) noexcept {
    return graph_->access_image(resource, pass_, execution_, true);
}
std::span<const std::byte> GraphContext::read_buffer(GraphResource resource) noexcept {
    return graph_->access_buffer(resource, pass_, execution_, false);
}
std::span<std::byte> GraphContext::write_buffer(GraphResource resource) noexcept {
    return graph_->access_buffer(resource, pass_, execution_, true);
}
std::span<const double> GraphContext::read_depth(GraphResource resource) noexcept {
    return graph_->access_depth(resource, pass_, execution_, false);
}
std::span<double> GraphContext::write_depth(GraphResource resource) noexcept {
    return graph_->access_depth(resource, pass_, execution_, true);
}
Image* RenderGraph::access_image(GraphResource resource, GraphPass pass, std::uint64_t execution, bool write) noexcept {
    return impl_->access(resource, pass, execution, write, GraphResourceKind::Image) ? impl_->image(resource.index) : nullptr;
}
std::span<std::byte> RenderGraph::access_buffer(GraphResource resource, GraphPass pass,
                                              std::uint64_t execution, bool write) noexcept {
    return impl_->access(resource, pass, execution, write, GraphResourceKind::Buffer) ? impl_->buffer(resource.index) : std::span<std::byte>{};
}
std::span<double> RenderGraph::access_depth(GraphResource resource, GraphPass pass,
                                           std::uint64_t execution, bool write) noexcept {
    return impl_->access(resource, pass, execution, write, GraphResourceKind::Depth) ? impl_->depth(resource.index) : std::span<double>{};
}

GraphResult RenderGraph::execute(const core::Frame& frame, GraphFailureMode mode) {
    if (!impl_->edit()) { return impl_->result; }
    if (mode != GraphFailureMode::Abort && mode != GraphFailureMode::ContinueIndependent) {
        impl_->fail(GraphError::InvalidDescriptor);
        return impl_->result;
    }
    impl_->executed = false;
    impl_->frame_success = false;
    auto& statistics = impl_->statistics;
    increment(statistics.executions);
    statistics.frame = frame.tick;
    statistics.completed_passes = 0;
    statistics.failed_passes = 0;
    statistics.skipped_passes = 0;
    statistics.resource_writes = 0;
    statistics.partial_output = false;
    if (!compiled() && !compile()) { return impl_->result; }
    if (!impl_->validate_imports()) { return impl_->result; }
    if (impl_->execution == std::numeric_limits<std::uint64_t>::max()) {
        impl_->fail(GraphError::Capacity, "execution token space exhausted");
        return impl_->result;
    }
    ++impl_->execution;
    for (auto& resource : impl_->resources) {
        resource.initialized = resource.imported;
        if (resource.imported) {
            resource.bytes = impl_->bytes(resource);
            if (resource.kind == GraphResourceKind::Image) {
                resource.width = resource.image->width();
                resource.height = resource.image->height();
            } else if (resource.kind == GraphResourceKind::Depth) {
                resource.width = resource.framebuffer->width();
                resource.height = resource.framebuffer->height();
            }
        }
    }
    for (auto& pass : impl_->passes) {
        pass.state = pass.description.enabled ? GraphPassState::Pending : GraphPassState::Disabled;
    }
    struct ExecutionScope {
        Impl& state;
        ~ExecutionScope() { state.running = false; state.current_pass = unused; }
    } scope{*impl_};
    impl_->running = true;
    GraphResult failure;
    bool abort_remaining{};
    bool any_changed{};
    bool imported_changed{};
    for (const auto handle : impl_->plan->schedule) {
        auto& pass = impl_->passes[handle.index];
        bool skip = abort_remaining;
        for (const auto prerequisite : pass.description.after) {
            skip = skip || impl_->passes[prerequisite.index].state != GraphPassState::Succeeded;
        }
        for (const auto& use : pass.description.resources) {
            if (reads(use.access) && !impl_->resources[use.resource.index].initialized) { skip = true; }
        }
        if (skip) {
            pass.state = GraphPassState::Skipped;
            ++statistics.skipped_passes;
            for (const auto& use : pass.description.resources) {
                if (writes(use.access)) { impl_->resources[use.resource.index].initialized = false; }
            }
            if (failure.error == GraphError::None) {
                failure = {GraphError::UninitializedRead, handle, {}, "pass input is unavailable", false, false};
            }
            continue;
        }
        impl_->current_pass = handle.index;
        impl_->access_failed = false;
        impl_->access_resource = {};
        GraphContext context(*this, handle, frame, impl_->execution);
        GraphPassResult response;
        GraphError error = GraphError::None;
        try {
            response = pass.callback(context);
            if (response.message.size() > 512) { response.message.resize(512); }
            if (!response.success) { error = GraphError::CallbackFailure; }
        } catch (const std::bad_alloc&) {
            response = {false, true, "callback allocation failed"};
            error = GraphError::AllocationFailure;
        } catch (const std::exception& exception) {
            response = {false, true, bounded_message(exception.what())};
            error = GraphError::CallbackException;
        } catch (...) {
            response = {false, true, "unknown callback exception"};
            error = GraphError::CallbackException;
        }
        if (impl_->access_failed) {
            error = GraphError::UndeclaredAccess;
            response = {false, true, "callback accessed an undeclared or unavailable resource"};
        }
        bool storage_changed{};
        for (std::size_t index = 0; index < impl_->resources.size(); ++index) {
            const auto& resource = impl_->resources[index];
            if (resource.kind == GraphResourceKind::Image) {
                const auto* image = impl_->image(index);
                storage_changed = storage_changed || (image &&
                    (image->width() != resource.width || image->height() != resource.height));
            } else if (resource.imported && resource.kind == GraphResourceKind::Depth) {
                storage_changed = storage_changed || !resource.framebuffer->valid() ||
                    resource.framebuffer->width() != resource.width || resource.framebuffer->height() != resource.height ||
                    resource.framebuffer->depth().size_bytes() != resource.bytes;
            }
        }
        if (storage_changed) {
            response = {false, true, "callbacks cannot resize attachment storage"};
            error = GraphError::InvalidDescriptor;
            impl_->dirty = true;
            abort_remaining = true;
        }
        const bool succeeded = error == GraphError::None;
        for (const auto& use : pass.description.resources) {
            if (!writes(use.access)) { continue; }
            auto& resource = impl_->resources[use.resource.index];
            resource.initialized = succeeded;
            ++statistics.resource_writes;
            any_changed = any_changed || response.output_changed;
            imported_changed = imported_changed || (resource.imported && response.output_changed);
        }
        pass.state = succeeded ? GraphPassState::Succeeded : GraphPassState::Failed;
        if (succeeded) { ++statistics.completed_passes; }
        else {
            ++statistics.failed_passes;
            if (failure.error == GraphError::None) {
                failure = {error, handle, impl_->access_resource, std::move(response.message), false, false};
            }
            abort_remaining = abort_remaining || mode == GraphFailureMode::Abort;
        }
    }
    impl_->frame_success = failure.error == GraphError::None;
    impl_->executed = true;
    failure.partial_output = !impl_->frame_success && any_changed;
    failure.imported_output_changed = imported_changed;
    statistics.partial_output = failure.partial_output;
    impl_->result = std::move(failure);
    return impl_->result;
}

const Image* RenderGraph::output_image(std::string_view name, bool allow_partial) const noexcept {
    const auto handle = output(name);
    return handle && impl_->available(*handle, GraphResourceKind::Image, allow_partial) ? impl_->image(handle->index) : nullptr;
}
std::span<const std::byte> RenderGraph::output_buffer(std::string_view name, bool allow_partial) const noexcept {
    const auto handle = output(name);
    return handle && impl_->available(*handle, GraphResourceKind::Buffer, allow_partial) ?
        std::span<const std::byte>{impl_->buffer(handle->index)} : std::span<const std::byte>{};
}
std::span<const double> RenderGraph::output_depth(std::string_view name, bool allow_partial) const noexcept {
    const auto handle = output(name);
    return handle && impl_->available(*handle, GraphResourceKind::Depth, allow_partial) ?
        std::span<const double>{impl_->depth(handle->index)} : std::span<const double>{};
}

GraphPass RenderGraph::add_clear_image(std::string name, GraphResource target, Color color, std::vector<GraphPass> after) {
    const auto* resource = impl_->resolve(target);
    if (!resource || resource->kind != GraphResourceKind::Image || !finite(color)) {
        impl_->fail(GraphError::InvalidDescriptor);
        return {};
    }
    const auto maximum = impl_->limits.transfer_bytes;
    return add_pass({std::move(name), {{target, GraphAccess::Write}}, std::move(after), 0, true},
        [target, color, maximum](GraphContext& context) -> GraphPassResult {
            auto* image = context.write_image(target);
            if (!image || image->byte_size() > maximum) { return {false, false, "image clear byte limit"}; }
            image->clear(color);
            return {true, !image->empty(), {}};
        });
}

GraphPass RenderGraph::add_clear_depth(std::string name, GraphResource target, double depth, std::vector<GraphPass> after) {
    const auto* resource = impl_->resolve(target);
    if (!resource || resource->kind != GraphResourceKind::Depth || !std::isfinite(depth) || depth < 0.0 || depth > 1.0) {
        impl_->fail(GraphError::InvalidDescriptor);
        return {};
    }
    const auto maximum = impl_->limits.transfer_bytes;
    return add_pass({std::move(name), {{target, GraphAccess::Write}}, std::move(after), 0, true},
        [target, depth, maximum](GraphContext& context) -> GraphPassResult {
            auto values = context.write_depth(target);
            if (values.size_bytes() > maximum) { return {false, false, "depth clear byte limit"}; }
            std::fill(values.begin(), values.end(), depth);
            return {true, !values.empty(), {}};
        });
}

GraphPass RenderGraph::add_clear_buffer(std::string name, GraphResource target, std::byte value, std::vector<GraphPass> after) {
    const auto* resource = impl_->resolve(target);
    if (!resource || resource->kind != GraphResourceKind::Buffer) {
        impl_->fail(GraphError::InvalidDescriptor);
        return {};
    }
    const auto maximum = impl_->limits.transfer_bytes;
    return add_pass({std::move(name), {{target, GraphAccess::Write}}, std::move(after), 0, true},
        [target, value, maximum](GraphContext& context) -> GraphPassResult {
            auto bytes = context.write_buffer(target);
            if (bytes.size() > maximum) { return {false, false, "buffer clear byte limit"}; }
            std::fill(bytes.begin(), bytes.end(), value);
            return {true, !bytes.empty(), {}};
        });
}

GraphPass RenderGraph::add_copy(std::string name, GraphResource source, GraphResource target, std::vector<GraphPass> after) {
    const auto* from = impl_->resolve(source);
    const auto* to = impl_->resolve(target);
    if (!from || !to || source == target || from->kind != to->kind) {
        impl_->fail(GraphError::InvalidDescriptor);
        return {};
    }
    const auto kind = from->kind;
    const auto maximum = impl_->limits.transfer_bytes;
    return add_pass({std::move(name), {{source, GraphAccess::Read}, {target, GraphAccess::Write}}, std::move(after), 0, true},
        [source, target, kind, maximum](GraphContext& context) -> GraphPassResult {
            if (kind == GraphResourceKind::Image) {
                const auto* input = context.read_image(source);
                auto* destination_image = context.write_image(target);
                if (!input || !destination_image || input->width() != destination_image->width() || input->height() != destination_image->height()) {
                    return {false, false, "image copy requires equal extents"};
                }
                if (input->byte_size() > maximum) { return {false, false, "image copy byte limit"}; }
                std::copy(input->pixels().begin(), input->pixels().end(), destination_image->pixels().begin());
                return {true, !input->empty(), {}};
            }
            if (kind == GraphResourceKind::Depth) {
                const auto input = context.read_depth(source);
                const auto destination_depth = context.write_depth(target);
                if (input.size() != destination_depth.size() || input.size_bytes() > maximum) {
                    return {false, false, "depth copy extent or byte limit"};
                }
                std::copy(input.begin(), input.end(), destination_depth.begin());
                return {true, !input.empty(), {}};
            }
            const auto input = context.read_buffer(source);
            const auto destination_buffer = context.write_buffer(target);
            if (input.size() != destination_buffer.size() || input.size() > maximum) {
                return {false, false, "buffer copy size or byte limit"};
            }
            if (!input.empty()) { std::memmove(destination_buffer.data(), input.data(), input.size()); }
            return {true, !input.empty(), {}};
        });
}

GraphPass RenderGraph::add_resolve(std::string name, GraphResource source, GraphResource target, std::vector<GraphPass> after) {
    const auto* from = impl_->resolve(source);
    const auto* to = impl_->resolve(target);
    if (!from || !to || source == target || from->kind != GraphResourceKind::Image || to->kind != GraphResourceKind::Image) {
        impl_->fail(GraphError::InvalidDescriptor);
        return {};
    }
    const auto maximum = impl_->limits.transfer_bytes;
    return add_pass({std::move(name), {{source, GraphAccess::Read}, {target, GraphAccess::Write}}, std::move(after), 0, true},
        [source, target, maximum](GraphContext& context) -> GraphPassResult {
            const auto* input = context.read_image(source);
            auto* destination = context.write_image(target);
            if (!input || !destination || input->empty() || destination->empty() ||
                input->width() % destination->width() != 0 || input->height() % destination->height() != 0) {
                return {false, false, "resolve requires integral positive source-to-target scale"};
            }
            if (input->byte_size() > maximum) { return {false, false, "resolve byte limit"}; }
            const auto scale_x = input->width() / destination->width();
            const auto scale_y = input->height() / destination->height();
            const auto weight = 1.0 / (static_cast<double>(scale_x) * static_cast<double>(scale_y));
            for (std::uint32_t y = 0; y < destination->height(); ++y) {
                auto output_row = destination->row(y);
                for (std::uint32_t x = 0; x < destination->width(); ++x) {
                    Color sum = Color::transparent();
                    for (std::uint32_t offset_y = 0; offset_y < scale_y; ++offset_y) {
                        const auto input_row = input->row(y * scale_y + offset_y);
                        for (std::uint32_t offset_x = 0; offset_x < scale_x; ++offset_x) {
                            sum = sum + premultiplied(input_row[x * scale_x + offset_x]) * weight;
                        }
                    }
                    output_row[x] = sanitize(unpremultiplied(sum));
                }
            }
            return {true, true, {}};
        });
}

namespace {

void publish_raster_metrics(core::Kernel& kernel, const SoftwareRenderer& renderer, const RasterResult& result) {
    const auto& statistics = renderer.statistics();
    kernel.metric("render.triangles", static_cast<double>(statistics.input_triangles));
    kernel.metric("render.fragments", static_cast<double>(statistics.fragments_written));
    kernel.metric("render.fragment_visits", static_cast<double>(statistics.fragment_visits));
    kernel.metric("render.draws", static_cast<double>(statistics.draws_rendered));
    kernel.metric("render.work_limited", statistics.work_limited ? 1.0 : 0.0);
    kernel.metric("render.error", static_cast<double>(result.error));
}

} // namespace

bool RenderGraph::install_default(core::Kernel& kernel) {
    if (!impl_->edit()) { return false; }
    auto* images = kernel.services().find<ImageService>();
    if (!images || !kernel.services().find<SoftwareRenderer>() || !kernel.services().find<scene::Scene>() ||
        !kernel.services().find<geometry::MeshLibrary>() || !kernel.services().find<CameraService>() ||
        !kernel.services().find<MaterialService>()) {
        return impl_->fail(GraphError::MissingService);
    }
    try {
        RenderGraph candidate(impl_->limits);
        const auto color = candidate.import_image(std::string(graph_main_color), images->framebuffer().color());
        const auto depth = candidate.import_depth(std::string(graph_main_depth), images->framebuffer());
        auto* host = &kernel;
        const auto scene_pass = candidate.add_pass({std::string(graph_scene_pass),
            {{color, GraphAccess::ReadWrite}, {depth, GraphAccess::ReadWrite}}, {}, 0, true,
            graph_default_scene_callback_owner},
            [host, color, depth](GraphContext& context) -> GraphPassResult {
                auto* target = host->services().find<ImageService>();
                auto* renderer = host->services().find<SoftwareRenderer>();
                auto* current_scene = host->services().find<scene::Scene>();
                const auto* meshes = host->services().find<geometry::MeshLibrary>();
                const auto* cameras = host->services().find<CameraService>();
                const auto* materials = host->services().find<MaterialService>();
                if (!target || !renderer || !current_scene || !meshes || !cameras || !materials) {
                    return {false, false, "default graph service unavailable"};
                }
                auto* color_image = context.write_image(color);
                const auto depth_values = context.write_depth(depth);
                if (color_image != &target->framebuffer().color() ||
                    depth_values.data() != target->framebuffer().depth().data() ||
                    depth_values.size() != target->framebuffer().depth().size()) {
                    return {false, false, "default graph attachments were rebound away from the shared framebuffer"};
                }
                const auto result = renderer->render_scene(*current_scene, *meshes, *cameras, *materials, *target);
                publish_raster_metrics(*host, *renderer, result);
                return {static_cast<bool>(result), result.framebuffer_changed, std::string(describe(result.error))};
            });
        const auto complete = candidate.add_pass({std::string(graph_complete_pass),
            {{color, GraphAccess::Read}, {depth, GraphAccess::Read}}, {scene_pass}, 0, true},
            [images, color, depth](GraphContext& context) -> GraphPassResult {
                const auto* image = context.read_image(color);
                const auto depth_values = context.read_depth(depth);
                if (!image || !images->framebuffer().valid() || image->empty() ||
                    (images->framebuffer().has_depth() && depth_values.size() != image->size())) {
                    return {false, false, "frame output attachments are inconsistent"};
                }
                return {};
            });
        if (!color || !depth || !scene_pass || !complete ||
            !candidate.set_output(std::string(graph_main_color), color) ||
            !candidate.set_output(std::string(graph_main_depth), depth) || !candidate.compile()) {
            impl_->result = candidate.last_result();
            return false;
        }
        impl_.swap(candidate.impl_);
        return true;
    } catch (const std::bad_alloc&) { return impl_->fail(GraphError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->fail(GraphError::AllocationFailure); }
}

namespace {

struct GraphDriver {
    bool installed{};
    bool owns_frame{};
    bool enabled{true};
    GraphFailureMode failure_mode{GraphFailureMode::Abort};
    GraphError reported_error{GraphError::None};
};

bool acquire_frame(core::Kernel& kernel, RenderGraph& graph, GraphDriver& driver) {
    if (driver.owns_frame) { return true; }
    if (!driver.installed) { driver.installed = graph.install_default(kernel); }
    if (!driver.installed) {
        if (driver.reported_error != graph.last_result().error) {
            driver.reported_error = graph.last_result().error;
            kernel.diagnose(core::Diagnostic::Severity::Warning, "render.graph", std::string(describe(driver.reported_error)));
        }
        return false;
    }
    driver.owns_frame = kernel.set_system_enabled("render.rasterizer", false);
    return driver.owns_frame;
}

std::string result_text(const RenderGraph& graph) {
    const auto& result = graph.last_result();
    const auto& statistics = graph.statistics();
    std::ostringstream stream;
    stream << describe(result.error) << " frame=" << statistics.frame
           << " completed=" << statistics.completed_passes << " failed=" << statistics.failed_passes
           << " skipped=" << statistics.skipped_passes << " slots=" << statistics.physical_slots
           << " resident_bytes=" << statistics.transient_bytes << " aliases=" << statistics.aliased_resources
           << " partial=" << result.partial_output << " imported_changed=" << result.imported_output_changed;
    if (const auto pass = graph.pass_info(result.pass)) { stream << " pass=" << pass->name; }
    if (const auto resource = graph.resource_info(result.resource)) { stream << " resource=" << resource->name; }
    if (!result.message.empty()) { stream << " message=" << result.message; }
    return stream.str();
}

std::optional<bool> parse_enabled(std::string_view value) noexcept {
    if (value == "on" || value == "true" || value == "1") { return true; }
    if (value == "off" || value == "false" || value == "0") { return false; }
    return std::nullopt;
}

core::CommandResult plan_command(core::Kernel& kernel, core::CommandArgs arguments) {
    if (!arguments.empty()) { return {false, "usage: render.graph.plan"}; }
    auto& graph = kernel.services().get<RenderGraph>();
    if (!graph.compiled()) { return {false, "compile the graph before inspecting its physical plan"}; }
    std::ostringstream stream;
    for (const auto handle : graph.schedule()) {
        const auto pass = graph.pass_info(handle);
        stream << "pass " << pass->name << " priority=" << pass->priority << " state=" << describe(pass->state);
        for (const auto& use : pass->resources) {
            const auto resource = graph.resource_info(use.resource);
            stream << ' ' << resource->name << ':' << (use.access == GraphAccess::Read ? "read" :
                use.access == GraphAccess::Write ? "write" : "readwrite");
        }
        stream << '\n';
    }
    for (const auto& lifetime : graph.lifetimes()) {
        const auto resource = graph.resource_info(lifetime.resource);
        stream << "resource " << resource->name << ' ' << describe(resource->kind)
               << " first=" << lifetime.first_use << " last=" << lifetime.last_use << " bytes=" << lifetime.bytes;
        if (lifetime.imported) { stream << " imported"; }
        else { stream << " slot=" << lifetime.physical_slot; }
        stream << " exported=" << lifetime.exported << " valid=" << resource->initialized << '\n';
    }
    return {true, stream.str()};
}

} // namespace

void register_graph(core::Kernel& kernel) {
    if (!kernel.register_module({"render.graph", "CPU render pass DAG and bounded transient resource reuse", 1})) { return; }
    auto& graph = kernel.services().emplace<RenderGraph>();
    auto& driver = kernel.services().emplace<GraphDriver>();
    driver.enabled = kernel.config().boolean("render.graph.enabled", true);
    const bool registered = kernel.add_system({"render.graph.frame", core::Phase::Render, -50,
        {"render.lighting", "camera.update", "spatial.sync"}}, [](core::Kernel& host, const core::Frame& frame) {
            auto& current = host.services().get<RenderGraph>();
            auto& state = host.services().get<GraphDriver>();
            if (!acquire_frame(host, current, state)) { return; }
            host.metric("render.graph.enabled", state.enabled ? 1.0 : 0.0);
            if (!state.enabled) { return; }
            const auto result = current.execute(frame, state.failure_mode);
            const auto& statistics = current.statistics();
            host.metric("render.graph.error", static_cast<double>(result.error));
            host.metric("render.graph.passes", static_cast<double>(statistics.completed_passes));
            host.metric("render.graph.failed", static_cast<double>(statistics.failed_passes));
            host.metric("render.graph.skipped", static_cast<double>(statistics.skipped_passes));
            host.metric("render.graph.transient_bytes", static_cast<double>(statistics.transient_bytes));
            host.metric("render.graph.aliases", static_cast<double>(statistics.aliased_resources));
            host.metric("render.graph.partial", result.partial_output ? 1.0 : 0.0);
        });
    if (!registered) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "render.graph", "frame system registration failed");
        return;
    }
    (void)acquire_frame(kernel, graph, driver);
    (void)kernel.add_command("render.graph.stats", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.graph.stats"}; }
        return {true, result_text(host.services().get<RenderGraph>())};
    }, "Inspect graph completion, failure and transient allocation statistics");
    (void)kernel.add_command("render.graph.plan", plan_command, "Inspect deterministic pass order, resource uses and allocation lifetimes");
    (void)kernel.add_command("render.graph.compile", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.graph.compile"}; }
        auto& current = host.services().get<RenderGraph>();
        const auto result = current.compile();
        return {static_cast<bool>(result), result_text(current)};
    }, "Validate dependencies and conflicts and allocate bounded transient storage");
    (void)kernel.add_command("render.graph.run", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.graph.run"}; }
        auto& current = host.services().get<RenderGraph>();
        auto& state = host.services().get<GraphDriver>();
        if (!acquire_frame(host, current, state)) { return {false, result_text(current)}; }
        const auto result = current.execute(host.frame(), state.failure_mode);
        return {static_cast<bool>(result), result_text(current)};
    }, "Explicitly execute the graph using current camera and lighting snapshots");
    (void)kernel.add_command("render.graph.enabled", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        auto& state = host.services().get<GraphDriver>();
        if (arguments.size() > 1) { return {false, "usage: render.graph.enabled [on|off]"}; }
        if (!arguments.empty()) {
            const auto enabled = parse_enabled(arguments[0]);
            if (!enabled) { return {false, "expected on or off"}; }
            state.enabled = *enabled;
        }
        return {true, state.enabled ? "graph frame execution enabled" : "graph frame execution paused"};
    }, "Pause or resume the graph's scheduled render frame");
    (void)kernel.add_command("render.graph.policy", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        auto& state = host.services().get<GraphDriver>();
        if (arguments.size() > 1) { return {false, "usage: render.graph.policy [abort|independent]"}; }
        if (!arguments.empty()) {
            if (arguments[0] == "abort") { state.failure_mode = GraphFailureMode::Abort; }
            else if (arguments[0] == "independent") { state.failure_mode = GraphFailureMode::ContinueIndependent; }
            else { return {false, "expected abort or independent"}; }
        }
        return {true, state.failure_mode == GraphFailureMode::Abort ? "abort after failure" : "continue independent passes"};
    }, "Choose whether failures stop the frame or only dependent pass branches");
    (void)kernel.add_command("render.graph.reset", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.graph.reset"}; }
        auto& current = host.services().get<RenderGraph>();
        auto& state = host.services().get<GraphDriver>();
        if (!current.install_default(host)) { return {false, result_text(current)}; }
        state.installed = true;
        const bool acquired = acquire_frame(host, current, state);
        return {acquired, acquired ? "default shared-framebuffer graph installed" : "rasterizer schedule handover unavailable"};
    }, "Replace graph extensions with the default scene render and output validation passes");
}

} // namespace ge::render
