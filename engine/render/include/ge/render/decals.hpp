#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/render/camera.hpp>
#include <ge/render/graph.hpp>
#include <ge/render/material.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::render {

inline constexpr std::string_view graph_decals_pass = "scene.decals";
inline constexpr std::uint64_t graph_decals_callback_owner = 0x4745444543414c31ULL;

enum class DecalBlend : std::uint8_t { Alpha, Multiply, Add, Screen };
enum class DecalError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidSettings,
    InvalidProjector,
    InvalidEntity,
    InvalidHandle,
    InvalidImage,
    InvalidView,
    AmbiguousDepth,
    MissingTexture,
    MissingMaterial,
    MissingService,
    Capacity,
    WorkLimit,
    ScratchLimit,
    SceneRejected,
    AllocationFailure,
    GraphMissing,
    GraphConflict,
    GraphRejected,
    Busy,
    Stopped
};

[[nodiscard]] std::string_view describe(DecalError error) noexcept;
[[nodiscard]] std::string_view describe(DecalBlend mode) noexcept;

struct DecalHandle {
    std::uint64_t owner{};
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept;
    constexpr bool operator==(const DecalHandle&) const = default;
};

// Exact Scene affine matrices transform this centred local box. Projection is
// along local -Z; +X is texture right and +Y is texture up. UVs are top-left.
// A material supplies only albedo, alpha-mask and emissive color. Normal maps,
// roughness and metalness do not modify the already-lit framebuffer.
struct DecalProjector {
    math::Vec3 half_extent{0.5, 0.5, 0.5};
    Color tint{Color::white()};
    MaterialHandle material{};
    MaterialTexture texture{};
    DecalBlend blend{DecalBlend::Alpha};
    math::Vec3 edge_fade{0.05, 0.05, 0.05};
    double opacity{1.0};
    double alpha_cutoff{};
    double facing_start{};
    double facing_full{0.25};
    double fade_in_seconds{};
    double lifetime_seconds{};
    double fade_out_seconds{};
    double maximum_distance{1.0e12};
    double distance_fade{};
    std::int32_t order{};
    std::uint64_t view_mask{~std::uint64_t{0}};
    bool enabled{true};
    bool facing_fade{true};
    bool two_sided{};
};

// Cloning this component does not clone service ownership. Call attach on the
// clone to assign a fresh projector generation; raw component edits are validated.
struct DecalComponent {
    DecalHandle handle{};
    DecalProjector projector{};
};

struct DecalLimits {
    std::size_t projectors{1024};
    std::size_t views{8};
    std::size_t jobs{8192};
    std::size_t image_pixels{4194304};
    std::size_t candidate_pixels{16777216};
    std::size_t sample_visits{536870912};
    std::size_t scratch_bytes{256U * 1024U * 1024U};
    double maximum_delta_seconds{0.25};
};

struct DecalSettings {
    bool enabled{true};
    double clear_depth_epsilon{1.0e-10};
    double normal_absolute_threshold{0.05};
    double normal_relative_threshold{0.02};
    double output_ceiling{65504.0};
};

// Describes depth ownership, not a request to clear anything. Every view must
// have cleared and written its own non-overlapping region this frame. Callers
// must supply the exact matrices used by rasterization, after late camera edits.
struct DecalView {
    CameraView camera{};
    Rect region{};
    double clear_depth{1.0};
    bool depth_cleared{true};
};

struct DecalStatistics {
    std::uint64_t attempts{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::size_t active_projectors{};
    std::size_t prepared_projectors{};
    std::size_t jobs{};
    std::size_t candidate_pixels{};
    std::size_t visited_pixels{};
    std::size_t blended_pixels{};
    std::size_t outside_volume{};
    std::size_t background_pixels{};
    std::size_t normal_rejections{};
    std::size_t texture_samples{};
    std::size_t depth_samples{};
    std::size_t scratch_bytes{};
    std::size_t expired{};
    bool image_changed{};
    DecalError error{DecalError::None};
};

struct DecalResult {
    DecalError error{DecalError::None};
    bool image_changed{};
    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] bool valid(const DecalLimits& limits) noexcept;
[[nodiscard]] bool valid(const DecalSettings& settings) noexcept;
[[nodiscard]] bool valid(const DecalProjector& projector) noexcept;
[[nodiscard]] Color blend_decal(Color destination, Color source, double opacity,
                                 DecalBlend mode, double ceiling = 65504.0) noexcept;

// Serial CPU color decals. The whole image is staged before publication; depth,
// destination alpha, imported storage and dimensions remain intact on success.
// Rejections preserve destination pixels. Depth normals are approximate and
// rejected at discontinuities. No receiver entity/material IDs exist in depth.
// World positions are read from Scene each invocation, so WorldPartition origin
// shifts need no duplicated world-space cache. Destruction touches no borrowers.
class DecalService final {
public:
    explicit DecalService(scene::Scene& scene, DecalLimits limits = {});
    ~DecalService();
    DecalService(const DecalService&) = delete;
    DecalService& operator=(const DecalService&) = delete;
    DecalService(DecalService&&) = delete;
    DecalService& operator=(DecalService&&) = delete;
    [[nodiscard]] DecalHandle create(std::string_view name = "Decal",
                                     const math::Transform& transform = {},
                                     const DecalProjector& projector = {});
    [[nodiscard]] DecalHandle attach(ecs::Entity entity, const DecalProjector& projector = {});
    // Erase removes only the owned component. Expired projectors retain their
    // handle and capacity until erased, and restart makes them eligible again.
    bool erase(DecalHandle handle);
    std::size_t clear();
    bool replace(DecalHandle handle, const DecalProjector& projector);
    bool restart(DecalHandle handle) noexcept;
    [[nodiscard]] ecs::Entity entity(DecalHandle handle) const noexcept;
    [[nodiscard]] DecalHandle handle(ecs::Entity entity) const noexcept;
    [[nodiscard]] const DecalComponent* component(DecalHandle handle) const noexcept;
    [[nodiscard]] std::vector<DecalHandle> handles() const;
    [[nodiscard]] std::optional<math::Aabb> world_bounds(DecalHandle handle);
    [[nodiscard]] double age(DecalHandle handle) const noexcept;
    bool configure(const DecalSettings& settings) noexcept;
    [[nodiscard]] const DecalSettings& settings() const noexcept;
    [[nodiscard]] const DecalLimits& limits() const noexcept;
    [[nodiscard]] const DecalStatistics& statistics() const noexcept;
    [[nodiscard]] DecalError last_error() const noexcept;
    [[nodiscard]] DecalError attachment_error() const noexcept;
    void advance(double delta_seconds);
    [[nodiscard]] DecalResult process(Image& color, std::span<const double> depth,
                                      std::span<const DecalView> views,
                                      const TextureLibrary& textures,
                                      const MaterialLibrary* materials = nullptr) noexcept;
    // Keeps scene.raster < scene.decals < image.postprocess, or frame.complete
    // when postprocess is absent. Graph reset is recoverable; ownership theft is
    // rejected. Weak callbacks become no-ops after stop or owner destruction.
    // The default raster callback requires adjacency in the enabled schedule;
    // Visibility-owned rasterization requires its current rendered-frame stamp.
    [[nodiscard]] DecalError install_graph(core::Kernel& kernel);
    [[nodiscard]] bool attached(const RenderGraph& graph) const noexcept;
    bool detach(RenderGraph& graph);
    void stop() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_decals(core::Kernel& kernel);

} // namespace ge::render
