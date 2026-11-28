#pragma once

#include <ge/geometry/mesh.hpp>
#include <ge/render/camera.hpp>
#include <ge/render/material.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::render {

enum class CullMode : std::uint8_t { None, Back, Front };
enum class FillMode : std::uint8_t { Solid, Wireframe };
enum class RasterError : std::uint8_t {
    None,
    InvalidConfiguration,
    InvalidFramebuffer,
    InvalidView,
    InvalidDraw,
    MissingService,
    QueueCapacity,
    SceneRejected,
    AllocationFailure,
    WorkLimit
};

[[nodiscard]] std::string_view describe(RasterError error) noexcept;

struct RasterizerLimits {
    std::size_t max_draws{4096};
    std::size_t max_scene_entities{65536};
    std::size_t max_hierarchy_depth{256};
    std::size_t max_views{8};
    std::size_t max_input_triangles{262144};
    std::size_t max_raster_triangles{1048576};
    std::size_t max_fragment_visits{16777216};
    std::size_t max_light_visits{33554432};
    std::size_t max_lights_per_fragment{256};
    std::size_t max_framebuffer_pixels{16777216};
};

struct RasterizerSettings {
    bool enabled{true};
    bool render_all_views{};
    bool allow_stale_views{true};
    bool clear_color{true};
    bool clear_depth{true};
    bool sort_transparency{true};
    bool front_counter_clockwise{true};
    bool blended_depth_write{};
    CullMode cull{CullMode::Back};
    FillMode fill{FillMode::Solid};
    DepthState depth{};
    Color background{0.015, 0.02, 0.03, 1.0};
    double background_depth{1.0};
    double depth_bias{};
    double slope_depth_bias{};
    double wire_width{1.0};
    double texture_lod_bias{};
    std::optional<Rect> scissor;
};

// Borrowed mesh/material/texture/light data must remain immutable and alive until
// a synchronous draw finishes or a queued submission is rendered/cleared. A zero
// index_count selects the remainder of the shared mesh's indexed triangle list.
struct RasterDraw {
    const geometry::MeshData* mesh{};
    const PbrMaterial* material{};
    math::Mat4 model{};
    std::size_t first_index{};
    std::size_t index_count{};
    ge::RuntimeHandle entity{};
    std::uint64_t visibility_mask{~std::uint64_t{0}};
    std::uint64_t light_mask{~std::uint64_t{0}};
    std::optional<Rect> scissor;
    std::optional<DepthState> depth;
    std::optional<CullMode> cull;
    std::optional<FillMode> fill;
};

// Every borrowed draw and lighting resource must outlive the synchronous batch.
struct RasterViewSubmission {
    CameraView view{};
    std::span<const RasterDraw> draws{};
    ShadingContext lighting{};
};

// Synchronous callbacks borrow their user state for the duration of rendering.
// They must not mutate shared resources or reenter the renderer and must bound
// their own work. The supplied light span/max_lights already obey frame limits.
using FragmentShader = ShadingResult (*)(const RasterDraw&, SurfaceSample&,
                                        ShadingContext&, const TextureLibrary*,
                                        const void*) noexcept;

struct RasterStatistics {
    std::uint64_t frame{};
    std::size_t views{};
    std::size_t stale_views{};
    std::size_t entities_examined{};
    std::size_t entities_skipped{};
    // Submitted commands are counted once; rendered commands include each view.
    std::size_t draws_submitted{};
    std::size_t draws_rendered{};
    std::size_t draws_rejected{};
    std::size_t input_triangles{};
    std::size_t triangles_clipped{};
    std::size_t triangles_culled{};
    std::size_t triangles_degenerate{};
    std::size_t triangles_invalid{};
    std::size_t raster_triangles{};
    std::size_t vertices_transformed{};
    std::size_t fragment_visits{};
    std::size_t fragments_covered{};
    std::size_t fragments_depth_rejected{};
    std::size_t fragments_discarded{};
    std::size_t fragments_invalid{};
    std::size_t fragments_shaded{};
    std::size_t fragments_written{};
    std::size_t blended_fragments{};
    std::size_t light_visits{};
    std::size_t lights_evaluated{};
    std::size_t missing_texture_samples{};
    std::size_t cleared_pixels{};
    bool work_limited{};
};

struct RasterResult {
    RasterError error{RasterError::None};
    bool framebuffer_changed{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == RasterError::None;
    }
};

[[nodiscard]] bool valid(const RasterizerLimits& limits) noexcept;
[[nodiscard]] bool valid(const RasterizerSettings& settings) noexcept;

// Serial CPU renderer writing the shared Framebuffer directly. Rejected settings,
// views, draw batches and allocation failures during preparation preserve pixels.
// Work exhaustion during drawing returns WorkLimit with an explicitly partial
// frame; statistics distinguish visited, covered, shaded and written fragments.
// Pixel centres are x+0.5,y+0.5 and coverage uses an exact fixed-point top-left rule.
class SoftwareRenderer final {
public:
    explicit SoftwareRenderer(RasterizerLimits limits = {});
    ~SoftwareRenderer();
    SoftwareRenderer(const SoftwareRenderer&) = delete;
    SoftwareRenderer& operator=(const SoftwareRenderer&) = delete;
    SoftwareRenderer(SoftwareRenderer&&) = delete;
    SoftwareRenderer& operator=(SoftwareRenderer&&) = delete;

    bool configure(const RasterizerSettings& settings) noexcept;
    [[nodiscard]] const RasterizerSettings& settings() const noexcept;
    [[nodiscard]] const RasterizerLimits& limits() const noexcept;
    [[nodiscard]] const RasterStatistics& statistics() const noexcept;
    [[nodiscard]] RasterError last_error() const noexcept;
    // Scene views use this shadow provider. Immediate draws use their supplied
    // ShadingContext; the fragment shader override applies to both entry points.
    void set_shadow_provider(ShadowFactor provider, const void* user = nullptr) noexcept;
    // Release a borrowed callback only if both its function and user still match.
    // A replacement installed by another owner is preserved.
    bool clear_shadow_provider(ShadowFactor provider, const void* user = nullptr) noexcept;
    void set_fragment_shader(FragmentShader shader, const void* user = nullptr) noexcept;
    bool submit(const RasterDraw& draw);
    void clear_queue() noexcept;
    [[nodiscard]] std::size_t queued_draws() const noexcept;
    [[nodiscard]] std::span<const RasterDraw> submissions() const noexcept;

    // Immediate draws keep existing pixels/depth unless clear=true is requested.
    // A batch is validated completely before clearing or writing its first pixel.
    [[nodiscard]] RasterResult draw(Framebuffer& target, const CameraView& view,
                                     const RasterDraw& submission,
                                     const ShadingContext& lighting = {},
                                     const TextureLibrary* textures = nullptr);
    [[nodiscard]] RasterResult draw(Framebuffer& target, const CameraView& view,
                                     std::span<const RasterDraw> submissions,
                                     const ShadingContext& lighting = {},
                                     const TextureLibrary* textures = nullptr,
                                     bool clear = false);
    // All views preflight before any clear/write and share one work budget.
    // Immediate batches use their supplied lighting/shadow context; the existing
    // fragment-shader override applies to every view. The manual queue is untouched.
    [[nodiscard]] RasterResult draw_views(Framebuffer& target,
                                          std::span<const RasterViewSubmission> submissions,
                                          const TextureLibrary* textures = nullptr,
                                          bool clear = false);
    // Accepted render attempts consume the queue, including partial WorkLimit
    // frames. Preflight rejection leaves the queue available for correction.
    [[nodiscard]] RasterResult render(Framebuffer& target, const CameraView& view,
                                       const ShadingContext& lighting = {},
                                       const TextureLibrary* textures = nullptr);
    // Synchronizes Scene once, resolves MeshComponent submeshes and material slots,
    // honors inherited enablement/layers, and uses exact cached world matrices.
    // Clear is scoped to each view's framebuffer/scissor intersection. Transparent
    // submeshes sort back-to-front by transformed centre; intersecting triangles
    // within a submesh are still submitted in index order. The manual queue is
    // independent and remains untouched by automatic scene rendering.
    [[nodiscard]] RasterResult render_scene(scene::Scene& scene,
                                             const geometry::MeshLibrary& meshes,
                                             const CameraService& cameras,
                                             const MaterialService& materials,
                                             ImageService& images);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_rasterizer(core::Kernel& kernel);

} // namespace ge::render
