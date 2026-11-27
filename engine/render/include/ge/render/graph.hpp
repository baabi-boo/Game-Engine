#pragma once

#include <ge/render/image.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }

namespace ge::render {

inline constexpr std::string_view graph_main_color = "main.color";
inline constexpr std::string_view graph_main_depth = "main.depth";
inline constexpr std::string_view graph_scene_pass = "scene.raster";
inline constexpr std::string_view graph_complete_pass = "frame.complete";

struct GraphResource {
    std::uint64_t owner{};
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    [[nodiscard]] explicit operator bool() const noexcept;
    constexpr bool operator==(const GraphResource&) const = default;
};

struct GraphPass {
    std::uint64_t owner{};
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    [[nodiscard]] explicit operator bool() const noexcept;
    constexpr bool operator==(const GraphPass&) const = default;
};

enum class GraphResourceKind : std::uint8_t { Image, Buffer, Depth };
enum class GraphAccess : std::uint8_t { Read, Write, ReadWrite };
enum class GraphFailureMode : std::uint8_t { Abort, ContinueIndependent };
enum class GraphPassState : std::uint8_t { Pending, Succeeded, Failed, Skipped, Disabled };
enum class GraphError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidName,
    InvalidHandle,
    InvalidDescriptor,
    DuplicateName,
    Capacity,
    Busy,
    MissingService,
    MissingDependency,
    Cycle,
    ResourceConflict,
    ImportedAlias,
    UninitializedRead,
    InvalidImport,
    MemoryBudget,
    WorkLimit,
    AllocationFailure,
    UndeclaredAccess,
    CallbackFailure,
    CallbackException,
    NotCompiled
};

[[nodiscard]] std::string_view describe(GraphError error) noexcept;
[[nodiscard]] std::string_view describe(GraphResourceKind kind) noexcept;
[[nodiscard]] std::string_view describe(GraphPassState state) noexcept;

struct GraphLimits {
    std::size_t resources{128};
    std::size_t passes{128};
    std::size_t uses{4096};
    std::size_t dependencies{4096};
    std::size_t transient_slots{128};
    std::size_t transient_bytes{256U * 1024U * 1024U};
    std::size_t logical_transient_bytes{512U * 1024U * 1024U};
    std::size_t transfer_bytes{256U * 1024U * 1024U};
    std::size_t name_bytes{128};
    ImageLimits images{};
};

[[nodiscard]] bool valid(const GraphLimits& limits) noexcept;

struct GraphUse {
    GraphResource resource{};
    GraphAccess access{GraphAccess::Read};
};

struct GraphPassDescription {
    std::string name;
    std::vector<GraphUse> resources;
    std::vector<GraphPass> after;
    int priority{};
    bool enabled{true};
};

struct GraphPassResult {
    bool success{true};
    bool output_changed{};
    std::string message;
};

struct GraphResult {
    GraphError error{GraphError::None};
    GraphPass pass{};
    GraphResource resource{};
    std::string message;
    bool partial_output{};
    bool imported_output_changed{};
    [[nodiscard]] explicit operator bool() const noexcept;
};

struct GraphLifetime {
    GraphResource resource{};
    std::size_t first_use{};
    std::size_t last_use{};
    std::size_t physical_slot{std::numeric_limits<std::size_t>::max()};
    std::size_t bytes{};
    bool imported{};
    bool exported{};
};

struct GraphResourceInfo {
    GraphResource handle{};
    std::string_view name;
    GraphResourceKind kind{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t bytes{};
    bool imported{};
    bool exported{};
    bool initialized{};
};

struct GraphPassInfo {
    GraphPass handle{};
    std::string_view name;
    std::span<const GraphUse> resources;
    std::span<const GraphPass> after;
    int priority{};
    bool enabled{};
    GraphPassState state{GraphPassState::Pending};
};

struct GraphStatistics {
    std::uint64_t executions{};
    std::uint64_t compilations{};
    std::uint64_t frame{};
    std::size_t scheduled_passes{};
    std::size_t completed_passes{};
    std::size_t failed_passes{};
    std::size_t skipped_passes{};
    std::size_t resource_writes{};
    std::size_t physical_slots{};
    std::size_t aliased_resources{};
    std::size_t transient_bytes{};
    std::size_t logical_transient_bytes{};
    bool partial_output{};
};

class RenderGraph;

// Views are valid only inside this synchronous callback. Access must match a
// declared use. Write means complete replacement; ReadWrite requires valid input.
// Callbacks must bound their work, avoid reentry, retain no mutable views, and
// preserve image dimensions/storage. Resize imported owners between executions.
class GraphContext final {
public:
    [[nodiscard]] const core::Frame& frame() const noexcept;
    [[nodiscard]] GraphPass pass() const noexcept;
    [[nodiscard]] const Image* read_image(GraphResource resource) noexcept;
    [[nodiscard]] Image* write_image(GraphResource resource) noexcept;
    [[nodiscard]] std::span<const std::byte> read_buffer(GraphResource resource) noexcept;
    [[nodiscard]] std::span<std::byte> write_buffer(GraphResource resource) noexcept;
    [[nodiscard]] std::span<const double> read_depth(GraphResource resource) noexcept;
    [[nodiscard]] std::span<double> write_depth(GraphResource resource) noexcept;

private:
    friend class RenderGraph;
    GraphContext(RenderGraph& graph, GraphPass pass, const core::Frame& frame,
                 std::uint64_t execution) noexcept;
    RenderGraph* graph_;
    GraphPass pass_;
    const core::Frame* frame_;
    std::uint64_t execution_;
};

using GraphCallback = std::function<GraphPassResult(GraphContext&)>;

// Serial CPU pass DAG. Conflicting users must be ordered by an explicit path;
// independent ready passes sort by priority and then insertion index. Imported
// objects are borrowed and must outlive the graph or their replacement binding.
// Buffer imports must be rebound after reallocating their backing storage.
// Compile invalidates all borrowed outputs and releases the previous pool before
// allocating its replacement, keeping owned payload bytes within the budget.
class RenderGraph final {
public:
    explicit RenderGraph(GraphLimits limits = {});
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    [[nodiscard]] const GraphLimits& limits() const noexcept;
    [[nodiscard]] const GraphResult& last_result() const noexcept;
    [[nodiscard]] const GraphStatistics& statistics() const noexcept;
    [[nodiscard]] bool compiled() const noexcept;
    bool clear();
    [[nodiscard]] GraphResource import_image(std::string name, Image& image);
    [[nodiscard]] GraphResource import_buffer(std::string name, std::span<std::byte> bytes);
    // Reacquires the span each execution, so resizing a framebuffer is safe.
    [[nodiscard]] GraphResource import_depth(std::string name, Framebuffer& framebuffer);
    [[nodiscard]] GraphResource create_image(std::string name, std::uint32_t width,
                                             std::uint32_t height);
    [[nodiscard]] GraphResource create_buffer(std::string name, std::size_t bytes);
    [[nodiscard]] GraphResource create_depth(std::string name, std::uint32_t width,
                                             std::uint32_t height);
    bool rebind_image(GraphResource resource, Image& image);
    bool rebind_buffer(GraphResource resource, std::span<std::byte> bytes);
    bool rebind_depth(GraphResource resource, Framebuffer& framebuffer);
    bool resize_transient(GraphResource resource, std::uint32_t width, std::uint32_t height);
    bool resize_buffer(GraphResource resource, std::size_t bytes);
    [[nodiscard]] std::optional<GraphResource> find_resource(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<GraphPass> find_pass(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<GraphResourceInfo> resource_info(GraphResource resource) const noexcept;
    [[nodiscard]] std::optional<GraphPassInfo> pass_info(GraphPass pass) const noexcept;
    [[nodiscard]] GraphPass add_pass(GraphPassDescription description, GraphCallback callback);
    bool add_dependency(GraphPass pass, GraphPass prerequisite);
    bool replace_callback(GraphPass pass, GraphCallback callback);
    bool set_enabled(GraphPass pass, bool enabled);
    // Insertion inherits anchor predecessors; after-insertion also redirects the
    // anchor's existing direct dependents to the new pass as an atomic edit.
    [[nodiscard]] GraphPass insert_before(GraphPass anchor, GraphPassDescription description,
                                          GraphCallback callback);
    [[nodiscard]] GraphPass insert_after(GraphPass anchor, GraphPassDescription description,
                                         GraphCallback callback);
    bool set_output(std::string name, GraphResource resource);
    bool remove_output(std::string_view name);
    [[nodiscard]] std::optional<GraphResource> output(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const GraphPass> schedule() const noexcept;
    [[nodiscard]] std::span<const GraphLifetime> lifetimes() const noexcept;
    [[nodiscard]] GraphResult compile();
    // Failed callbacks invalidate all declared writes, without rollback. Abort
    // skips the remainder; ContinueIndependent skips failed dependency branches.
    // Exceptions conservatively mark imported writes as potentially modified.
    [[nodiscard]] GraphResult execute(const core::Frame& frame,
                                      GraphFailureMode mode = GraphFailureMode::Abort);
    // Outputs are borrowed until mutation/compile/execute or imported-owner edits.
    // allow_partial exposes only resources still valid after an incomplete frame.
    [[nodiscard]] const Image* output_image(std::string_view name,
                                            bool allow_partial = false) const noexcept;
    [[nodiscard]] std::span<const std::byte> output_buffer(std::string_view name,
                                                           bool allow_partial = false) const noexcept;
    [[nodiscard]] std::span<const double> output_depth(std::string_view name,
                                                      bool allow_partial = false) const noexcept;
    [[nodiscard]] GraphPass add_clear_image(std::string name, GraphResource target, Color color,
                                            std::vector<GraphPass> after = {});
    [[nodiscard]] GraphPass add_clear_depth(std::string name, GraphResource target, double depth,
                                            std::vector<GraphPass> after = {});
    [[nodiscard]] GraphPass add_clear_buffer(std::string name, GraphResource target, std::byte value,
                                             std::vector<GraphPass> after = {});
    [[nodiscard]] GraphPass add_copy(std::string name, GraphResource source, GraphResource target,
                                     std::vector<GraphPass> after = {});
    // Linear premultiplied block averaging; source extent must be an integral
    // multiple of target extent. This is a CPU supersample resolve, not GPU MSAA.
    [[nodiscard]] GraphPass add_resolve(std::string name, GraphResource source, GraphResource target,
                                        std::vector<GraphPass> after = {});
    // Replaces the graph only when construction succeeds. Uses the existing
    // renderer once, imports current ImageService attachments and exports names
    // above. Insert before frame.complete to extend the connected frame pipeline.
    bool install_default(core::Kernel& kernel);

private:
    friend class GraphContext;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Image* access_image(GraphResource resource, GraphPass pass, std::uint64_t execution, bool write) noexcept;
    std::span<std::byte> access_buffer(GraphResource resource, GraphPass pass,
                                      std::uint64_t execution, bool write) noexcept;
    std::span<double> access_depth(GraphResource resource, GraphPass pass,
                                   std::uint64_t execution, bool write) noexcept;
};

void register_graph(core::Kernel& kernel);

} // namespace ge::render
