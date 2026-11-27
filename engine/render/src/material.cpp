#include <ge/render/material.hpp>
#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::render {
namespace {

constexpr double coordinate_limit = 1.0e12;
constexpr double minimum_roughness = 0.045;
std::atomic<std::uint64_t> next_material_owner{1};

bool within(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool bounded(math::Vec3 value, double limit = coordinate_limit) noexcept {
    return within(value.x, -limit, limit) && within(value.y, -limit, limit) &&
           within(value.z, -limit, limit);
}

bool color_range(Color value, double maximum) noexcept {
    return within(value.r, 0.0, maximum) && within(value.g, 0.0, maximum) &&
           within(value.b, 0.0, maximum) && within(value.a, 0.0, 1.0);
}

math::Vec3 direction(math::Vec3 value, math::Vec3 fallback = {}) noexcept {
    if (!math::finite(value)) {
        return fallback;
    }
    const double largest = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    if (largest == 0.0) {
        return fallback;
    }
    value /= largest;
    return value / std::sqrt(math::dot(value, value));
}

double hdr_channel(double value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0, material_hdr_limit) : 0.0;
}

Color hdr(math::Vec3 value, double alpha = 1.0) noexcept {
    return {hdr_channel(value.x), hdr_channel(value.y), hdr_channel(value.z),
            std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0};
}

void advance(std::uint64_t& revision) noexcept {
    if (revision != std::numeric_limits<std::uint64_t>::max()) {
        ++revision;
    }
}

std::uint64_t allocate_owner() {
    auto candidate = next_material_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == std::numeric_limits<std::uint64_t>::max()) {
            throw std::length_error("material owner identity space exhausted");
        }
        if (next_material_owner.compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return candidate;
        }
    }
}

MaterialLimits checked_limits(MaterialLimits limits) {
    if (limits.capacity > 65536 || limits.slots_per_entity == 0 ||
        limits.slots_per_entity > 4096 || limits.lights > material_light_limit) {
        throw std::invalid_argument("invalid material, slot or light capacity");
    }
    return limits;
}

math::Vec2 transform_uv(const MaterialTexture& binding, math::Vec2 uv,
                        bool derivative) noexcept {
    const double cosine = std::cos(binding.rotation);
    const double sine = std::sin(binding.rotation);
    const auto scaled = uv * binding.scale;
    const math::Vec2 rotated{cosine * scaled.x - sine * scaled.y,
                             sine * scaled.x + cosine * scaled.y};
    return derivative ? rotated : rotated + binding.offset;
}

Color texture_value(const MaterialTexture& binding, const SurfaceSample& surface,
                    const TextureLibrary* textures, Color fallback,
                    std::uint8_t& missing) noexcept {
    if (!binding.texture) {
        return fallback;
    }
    const auto* texture = textures ? textures->get(binding.texture) : nullptr;
    if (!texture || texture->empty()) {
        ++missing;
        return fallback;
    }
    const auto uv = transform_uv(binding, surface.uv, false);
    if (surface.gradients_valid) {
        return texture->sample_grad(uv, transform_uv(binding, surface.uv_dx, true),
                                    transform_uv(binding, surface.uv_dy, true), binding.sampler);
    }
    return texture->sample(uv, surface.lod, binding.sampler);
}

double channel(Color value, TextureChannel selected) noexcept {
    switch (selected) {
    case TextureChannel::Red: return math::saturate(value.r);
    case TextureChannel::Green: return math::saturate(value.g);
    case TextureChannel::Blue: return math::saturate(value.b);
    case TextureChannel::Alpha: return math::saturate(value.a);
    }
    return 1.0;
}

bool valid_surface(const SurfaceSample& surface) noexcept {
    const auto valid_uv = [](math::Vec2 value) {
        return within(value.x, -coordinate_limit, coordinate_limit) &&
               within(value.y, -coordinate_limit, coordinate_limit);
    };
    return bounded(surface.world_position) && bounded(surface.world_normal) &&
        math::length_squared(direction(surface.world_normal)) > 0.0 &&
        bounded(math::xyz(surface.world_tangent)) &&
        within(surface.world_tangent.w, -coordinate_limit, coordinate_limit) &&
        valid_uv(surface.uv) && valid_uv(surface.uv_dx) && valid_uv(surface.uv_dy) &&
        color_range(surface.vertex_color, 1.0) && within(surface.lod, -32.0, 32.0);
}

bool valid_sample(const MaterialSample& surface) noexcept {
    return surface.valid && bounded(surface.world_position) &&
        bounded(surface.normal) && bounded(surface.geometric_normal) &&
        math::length_squared(direction(surface.normal)) > 0.0 &&
        math::length_squared(direction(surface.geometric_normal)) > 0.0 &&
        color_range(surface.albedo, 1.0) && color_range(surface.emissive, material_hdr_limit) &&
        within(surface.metalness, 0.0, 1.0) && within(surface.roughness, 0.0, 1.0) &&
        within(surface.dielectric_f0, 0.0, 1.0) && within(surface.opacity, 0.0, 1.0) &&
        within(surface.occlusion, 0.0, 1.0) && surface.alpha_mode <= AlphaMode::Blend &&
        surface.shading_model <= ShadingModel::Unlit;
}

math::Vec3 fresnel(math::Vec3 f0, double cosine) noexcept {
    const double complement = 1.0 - math::saturate(cosine);
    const double fifth = complement * complement * complement * complement * complement;
    return f0 + (math::Vec3{1.0, 1.0, 1.0} - f0) * fifth;
}

math::Vec3 reflectance(const MaterialSample& surface) noexcept {
    const math::Vec3 dielectric{surface.dielectric_f0, surface.dielectric_f0, surface.dielectric_f0};
    return math::lerp(dielectric, surface.albedo.rgb(), surface.metalness);
}

bool parse_number(std::string_view text, double& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}

template<class Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

ge::RuntimeHandle resolve_entity(const ecs::Registry& registry, std::string_view text) {
    if (const auto entity = registry.find_name(text)) {
        return entity;
    }
    ge::RuntimeHandle entity;
    const auto separator = text.find(':');
    if (separator == std::string_view::npos ||
        !parse_integer(text.substr(0, separator), entity.index) ||
        !parse_integer(text.substr(separator + 1), entity.generation)) {
        return {};
    }
    return registry.alive(entity) ? entity : ge::RuntimeHandle{};
}

MaterialTexture* select_texture(PbrMaterial& material, std::string_view name) noexcept {
    if (name == "albedo") { return &material.albedo_texture; }
    if (name == "metalness") { return &material.metalness_texture; }
    if (name == "roughness") { return &material.roughness_texture; }
    if (name == "normal") { return &material.normal_texture; }
    if (name == "emissive") { return &material.emissive_texture; }
    if (name == "occlusion") { return &material.occlusion_texture; }
    return nullptr;
}

std::string handle_text(MaterialHandle handle) {
    return std::to_string(handle.owner) + ":" + std::to_string(handle.index) + ":" +
           std::to_string(handle.generation);
}

core::CommandResult invalid_material(MaterialValidation validation) {
    return {false, std::string(validation.field) + ": " + std::string(validation.message)};
}

} // namespace

MaterialValidation validate_texture_binding(const MaterialTexture& binding) noexcept {
    if (binding.texture != TextureHandle{} && !binding.texture) {
        return {false, "texture", "malformed texture handle"};
    }
    if (!math::finite(binding.scale) || !math::finite(binding.offset) ||
        std::abs(binding.scale.x) > 1.0e6 || std::abs(binding.scale.y) > 1.0e6 ||
        std::abs(binding.offset.x) > 1.0e6 || std::abs(binding.offset.y) > 1.0e6 ||
        !within(binding.rotation, -1.0e6, 1.0e6)) {
        return {false, "uv_transform", "UV transform must be finite and within bounds"};
    }
    const auto& sampler = binding.sampler;
    if (sampler.address_u > AddressMode::Border || sampler.address_v > AddressMode::Border ||
        sampler.filter > FilterMode::Trilinear || binding.channel > TextureChannel::Alpha) {
        return {false, "sampler", "invalid addressing, filtering or channel enum"};
    }
    if (!color_range(sampler.border, material_hdr_limit) ||
        !within(sampler.lod_bias, -32.0, 32.0) || !within(sampler.min_lod, 0.0, 32.0) ||
        !within(sampler.max_lod, sampler.min_lod, 32.0)) {
        return {false, "sampler", "invalid border color or mip bounds"};
    }
    return {};
}

MaterialValidation validate_material(const PbrMaterial& material) noexcept {
    if (!color_range(material.albedo, 1.0)) {
        return {false, "albedo", "RGBA must be finite and between zero and one"};
    }
    if (!color_range(material.emissive, 1.0e6) || !within(material.emissive_strength, 0.0, 1.0e6)) {
        return {false, "emissive", "RGB and strength must be finite within [0,1000000]"};
    }
    const std::array<std::pair<std::string_view, double>, 5> factors{{
        {"metalness", material.metalness}, {"roughness", material.roughness},
        {"dielectric_f0", material.dielectric_f0}, {"occlusion_strength", material.occlusion_strength},
        {"alpha_cutoff", material.alpha_cutoff}}};
    for (const auto& [name, value] : factors) {
        if (!within(value, 0.0, 1.0)) {
            return {false, name, "factor must be finite and between zero and one"};
        }
    }
    if (!within(material.normal_scale, 0.0, 16.0)) {
        return {false, "normal_scale", "normal strength must be finite within [0,16]"};
    }
    if (material.alpha_mode > AlphaMode::Blend || material.shading_model > ShadingModel::Unlit) {
        return {false, "mode", "invalid alpha mode or shading model"};
    }
    const std::array<const MaterialTexture*, 6> bindings{{&material.albedo_texture,
        &material.metalness_texture, &material.roughness_texture, &material.normal_texture,
        &material.emissive_texture, &material.occlusion_texture}};
    for (const auto* binding : bindings) {
        const auto result = validate_texture_binding(*binding);
        if (!result) {
            return result;
        }
    }
    return {};
}

MaterialHandle MaterialComponent::at(std::uint32_t slot) const noexcept {
    return slot < slots.size() ? slots[slot] : MaterialHandle{};
}

struct MaterialLibrary::Record {
    std::string name;
    PbrMaterial material;
    MaterialHandle handle;
    std::uint64_t revision{};
};

MaterialLibrary::MaterialLibrary(MaterialLimits limits)
    : limits_(checked_limits(limits)), owner_(allocate_owner()), generations_(limits_.capacity),
      records_(limits_.capacity) {}

MaterialLibrary::~MaterialLibrary() = default;

bool MaterialLibrary::owns(MaterialHandle handle) const noexcept {
    return handle.owner == owner_ && generations_.alive({handle.index, handle.generation});
}

MaterialHandle MaterialLibrary::create(std::string_view name, const PbrMaterial& material) {
    const auto validation = validate_material(material);
    if (!validation) {
        throw std::invalid_argument(std::string(validation.field) + ": " + std::string(validation.message));
    }
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string_view::npos || find(name)) {
        throw std::invalid_argument("material names must be unique, nonempty and at most 128 bytes");
    }
    auto record = std::make_unique<Record>();
    record->name = name;
    record->material = material;
    const auto token = generations_.acquire();
    if (!token) {
        throw std::length_error("material library capacity exhausted");
    }
    advance(revision_);
    record->handle = {owner_, token->index, token->generation};
    record->revision = revision_;
    const auto handle = record->handle;
    records_[token->index - 1] = std::move(record);
    return handle;
}

bool MaterialLibrary::replace(MaterialHandle handle, const PbrMaterial& material) {
    if (!owns(handle) || !validate_material(material)) {
        return false;
    }
    auto& record = *records_[handle.index - 1];
    record.material = material;
    advance(revision_);
    record.revision = revision_;
    return true;
}

bool MaterialLibrary::erase(MaterialHandle handle) noexcept {
    if (!owns(handle)) {
        return false;
    }
    records_[handle.index - 1].reset();
    const bool released = generations_.release({handle.index, handle.generation});
    advance(revision_);
    return released;
}

void MaterialLibrary::clear() noexcept {
    for (const auto& record : records_) {
        if (record) {
            const auto handle = record->handle;
            static_cast<void>(erase(handle));
        }
    }
}

MaterialHandle MaterialLibrary::find(std::string_view name) const noexcept {
    for (const auto& record : records_) {
        if (record && record->name == name) {
            return record->handle;
        }
    }
    return {};
}

const PbrMaterial* MaterialLibrary::get(MaterialHandle handle) const noexcept {
    return owns(handle) ? &records_[handle.index - 1]->material : nullptr;
}

std::optional<MaterialInfo> MaterialLibrary::inspect(MaterialHandle handle) const noexcept {
    if (!owns(handle)) {
        return std::nullopt;
    }
    const auto& record = *records_[handle.index - 1];
    return MaterialInfo{handle, record.name, record.revision, &record.material};
}

std::vector<MaterialHandle> MaterialLibrary::handles() const {
    std::vector<MaterialHandle> result;
    result.reserve(size());
    for (const auto& record : records_) {
        if (record) {
            result.push_back(record->handle);
        }
    }
    return result;
}

std::size_t MaterialLibrary::size() const noexcept { return generations_.size(); }
std::uint64_t MaterialLibrary::revision() const noexcept { return revision_; }
const MaterialLimits& MaterialLibrary::limits() const noexcept { return limits_; }

bool MaterialLibrary::assign(ecs::Registry& registry, ge::RuntimeHandle entity,
                              MaterialHandle material, std::uint32_t slot) const {
    if (!owns(material) || slot >= limits_.slots_per_entity ||
        !registry.has<geometry::MeshComponent>(entity)) {
        return false;
    }
    const auto* previous = registry.get<MaterialComponent>(entity);
    if (previous && previous->slots.size() > limits_.slots_per_entity) {
        return false;
    }
    MaterialComponent replacement = previous ? *previous : MaterialComponent{};
    if (replacement.slots.size() <= slot) {
        replacement.slots.resize(static_cast<std::size_t>(slot) + 1);
    }
    replacement.slots[slot] = material;
    registry.emplace_or_replace<MaterialComponent>(entity, std::move(replacement));
    return true;
}

bool MaterialLibrary::unassign(ecs::Registry& registry, ge::RuntimeHandle entity,
                                std::uint32_t slot) const {
    auto* component = registry.get<MaterialComponent>(entity);
    if (!component || component->slots.size() > limits_.slots_per_entity || slot >= component->slots.size()) {
        return false;
    }
    component->slots[slot] = {};
    while (!component->slots.empty() && !component->slots.back()) {
        component->slots.pop_back();
    }
    registry.mark_changed<MaterialComponent>(entity);
    return true;
}

MaterialHandle MaterialLibrary::assigned(const ecs::Registry& registry, ge::RuntimeHandle entity,
                                          std::uint32_t slot) const noexcept {
    if (slot >= limits_.slots_per_entity || !registry.has<geometry::MeshComponent>(entity)) {
        return {};
    }
    const auto* component = registry.get<MaterialComponent>(entity);
    if (!component || component->slots.size() > limits_.slots_per_entity) {
        return {};
    }
    const auto handle = component->at(slot);
    return owns(handle) ? handle : MaterialHandle{};
}

MaterialValidation validate_light(const Light& light) noexcept {
    if (light.type > LightType::Spot || !bounded(light.position) || !bounded(light.direction)) {
        return {false, "light", "invalid type or nonfinite/out-of-bounds transform"};
    }
    if (light.type != LightType::Point && math::length_squared(direction(light.direction)) == 0.0) {
        return {false, "direction", "directional and spot lights require a nonzero direction"};
    }
    if (!color_range(light.color, 1.0e6) || !within(light.intensity, 0.0, material_hdr_limit)) {
        return {false, "radiance", "invalid light color or intensity"};
    }
    if (!within(light.radius, 1.0e-4, coordinate_limit) ||
        !within(light.range, light.radius, coordinate_limit)) {
        return {false, "range", "range must contain a finite positive source radius"};
    }
    if (!within(light.outer_cone, 1.0e-6, math::pi * 0.5) ||
        !within(light.inner_cone, 0.0, light.outer_cone)) {
        return {false, "cone", "cone half angles must satisfy 0 <= inner <= outer <= pi/2"};
    }
    return {};
}

LightSample sample_light(const Light& light, math::Vec3 world_position) noexcept {
    LightSample result;
    if (!light.enabled || !validate_light(light) || !bounded(world_position)) {
        return result;
    }
    double attenuation = 1.0;
    if (light.type == LightType::Directional) {
        result.direction = -direction(light.direction);
    } else {
        const auto displacement = light.position - world_position;
        const double squared_distance = math::length_squared(displacement);
        result.distance = std::sqrt(squared_distance);
        if (result.distance == 0.0 || result.distance >= light.range) {
            return result;
        }
        result.direction = displacement / result.distance;
        const double ratio = result.distance / light.range;
        const double window = 1.0 - ratio * ratio * ratio * ratio;
        attenuation = window * window / std::max(squared_distance, light.radius * light.radius);
        if (light.type == LightType::Spot) {
            const double cosine = math::dot(direction(light.direction), -result.direction);
            const double outer = std::cos(light.outer_cone);
            const double inner = std::cos(light.inner_cone);
            if (cosine <= outer) {
                return result;
            }
            const double cone = inner > outer ? math::saturate((cosine - outer) / (inner - outer)) : 1.0;
            attenuation *= cone * cone * (3.0 - 2.0 * cone);
        }
    }
    result.radiance = hdr(light.color.rgb() * (light.intensity * attenuation));
    result.valid = true;
    return result;
}

MaterialSample sample_material(const PbrMaterial& material, const SurfaceSample& surface,
                                 const TextureLibrary* textures) noexcept {
    MaterialSample result;
    if (!validate_material(material) || !valid_surface(surface)) {
        return result;
    }
    result.world_position = surface.world_position;
    result.geometric_normal = direction(surface.world_normal);
    if (!surface.front_facing && material.double_sided) {
        result.geometric_normal = -result.geometric_normal;
    }
    result.normal = result.geometric_normal;
    result.alpha_mode = material.alpha_mode;
    result.shading_model = material.shading_model;
    result.dielectric_f0 = material.dielectric_f0;
    auto& missing = result.missing_textures;
    const auto sampled_albedo = clamped(texture_value(material.albedo_texture, surface, textures,
                                                     Color::white(), missing));
    result.albedo = material.albedo * sampled_albedo;
    if (material.use_vertex_color) {
        result.albedo = result.albedo * surface.vertex_color;
    }
    result.opacity = material.alpha_mode == AlphaMode::Opaque ? 1.0 : result.albedo.a;
    result.albedo.a = result.opacity;
    result.metalness = material.metalness * channel(texture_value(material.metalness_texture,
        surface, textures, Color::white(), missing), material.metalness_texture.channel);
    result.roughness = material.roughness * channel(texture_value(material.roughness_texture,
        surface, textures, Color::white(), missing), material.roughness_texture.channel);
    const auto occlusion = channel(texture_value(material.occlusion_texture, surface, textures,
                                                 Color::white(), missing), material.occlusion_texture.channel);
    result.occlusion = math::lerp(1.0, occlusion, material.occlusion_strength);
    const auto emission = texture_value(material.emissive_texture, surface, textures, Color::white(), missing);
    result.emissive = hdr(material.emissive.rgb() * hdr(emission.rgb()).rgb() * material.emissive_strength);
    if (material.normal_texture.texture) {
        const auto encoded = clamped(texture_value(material.normal_texture, surface, textures,
                                                    {0.5, 0.5, 1.0, 1.0}, missing));
        const auto tangent_normal = direction({(encoded.r * 2.0 - 1.0) * material.normal_scale,
            (encoded.g * 2.0 - 1.0) * material.normal_scale, std::max(0.0, encoded.b * 2.0 - 1.0)},
            {0.0, 0.0, 1.0});
        const auto normal = result.geometric_normal;
        const auto reference = std::abs(normal.y) < 0.999 ? math::Vec3{0.0, 1.0, 0.0}
                                                        : math::Vec3{1.0, 0.0, 0.0};
        const auto source_tangent = direction(math::xyz(surface.world_tangent));
        const auto projected = source_tangent - normal * math::dot(normal, source_tangent);
        const auto tangent = math::length_squared(projected) > 1.0e-12 ? direction(projected)
            : direction(math::cross(reference, normal));
        const double handedness = surface.world_tangent.w < 0.0 ? -1.0 : 1.0;
        const auto bitangent = math::cross(normal, tangent) * handedness;
        result.normal = direction(tangent * tangent_normal.x + bitangent * tangent_normal.y +
                                   normal * tangent_normal.z, normal);
    }
    result.discarded = (!surface.front_facing && !material.double_sided) ||
        (material.alpha_mode == AlphaMode::Mask && result.opacity < material.alpha_cutoff) ||
        (material.alpha_mode == AlphaMode::Blend && result.opacity == 0.0);
    if (material.alpha_mode == AlphaMode::Mask && !result.discarded) {
        result.opacity = 1.0;
        result.albedo.a = 1.0;
    }
    result.valid = true;
    return result;
}

BrdfSample evaluate_brdf(const MaterialSample& surface, math::Vec3 view_direction,
                          math::Vec3 light_direction) noexcept {
    BrdfSample result;
    if (!valid_sample(surface) || surface.discarded || surface.shading_model == ShadingModel::Unlit) {
        return result;
    }
    const auto normal = direction(surface.normal);
    const auto view = direction(view_direction);
    const auto light = direction(light_direction);
    const double nv = math::saturate(math::dot(normal, view));
    const double nl = math::saturate(math::dot(normal, light));
    if (nv == 0.0 || nl == 0.0 || math::dot(surface.geometric_normal, light) <= 0.0) {
        return result;
    }
    if (surface.shading_model == ShadingModel::Lambert) {
        result.diffuse = hdr(surface.albedo.rgb() / math::pi);
        return result;
    }
    const auto halfway = direction(view + light, normal);
    const double nh = math::saturate(math::dot(normal, halfway));
    const double vh = math::saturate(math::dot(view, halfway));
    const double roughness = std::max(minimum_roughness, surface.roughness);
    const double alpha = roughness * roughness;
    const double alpha_squared = alpha * alpha;
    const double denominator = nh * nh * (alpha_squared - 1.0) + 1.0;
    const double distribution = alpha_squared / (math::pi * denominator * denominator);
    const double view_mask = nl * std::sqrt(nv * nv * (1.0 - alpha_squared) + alpha_squared);
    const double light_mask = nv * std::sqrt(nl * nl * (1.0 - alpha_squared) + alpha_squared);
    const double visibility = 0.5 / std::max(view_mask + light_mask, 1.0e-20);
    const auto reflection = fresnel(reflectance(surface), vh);
    result.specular = hdr(reflection * (distribution * visibility));
    result.diffuse = hdr(surface.albedo.rgb() * (math::Vec3{1.0, 1.0, 1.0} - reflection) *
                         ((1.0 - surface.metalness) / math::pi));
    return result;
}

ShadingResult shade_surface(const MaterialSample& surface, const ShadingContext& context) noexcept {
    ShadingResult result;
    result.missing_textures = surface.missing_textures;
    if (!valid_sample(surface) || !color_range(context.ambient, material_hdr_limit) ||
        !math::finite(context.view_direction) || math::length_squared(direction(context.view_direction)) == 0.0) {
        return result;
    }
    result.valid = true;
    result.discarded = surface.discarded;
    if (surface.discarded) {
        return result;
    }
    if (surface.shading_model == ShadingModel::Unlit) {
        result.color = hdr(surface.albedo.rgb() + surface.emissive.rgb(), surface.opacity);
        return result;
    }
    auto diffuse = surface.albedo.rgb() / math::pi;
    if (surface.shading_model == ShadingModel::MetallicRoughness) {
        const auto reflection = fresnel(reflectance(surface), math::dot(direction(surface.normal),
                                                                        direction(context.view_direction)));
        diffuse = diffuse * (math::Vec3{1.0, 1.0, 1.0} - reflection) * (1.0 - surface.metalness);
    }
    auto radiance = surface.emissive.rgb() + diffuse * context.ambient.rgb() * surface.occlusion;
    const auto count = std::min({context.lights.size(), context.max_lights, material_light_limit});
    for (std::size_t index = 0; index < count; ++index) {
        const auto& light = context.lights[index];
        if ((light.layers & context.light_mask) == 0) {
            continue;
        }
        const auto illumination = sample_light(light, surface.world_position);
        const double nl = math::saturate(math::dot(direction(surface.normal), illumination.direction));
        if (!illumination.valid || nl == 0.0 || math::dot(surface.geometric_normal, illumination.direction) <= 0.0) {
            continue;
        }
        double visibility = 1.0;
        if (context.shadow_factor && light.casts_shadow) {
            const double factor = context.shadow_factor(light, surface, illumination, context.shadow_user);
            visibility = std::isfinite(factor) ? math::saturate(factor) : 0.0;
        }
        const auto brdf = evaluate_brdf(surface, context.view_direction, illumination.direction);
        radiance += (brdf.diffuse.rgb() + brdf.specular.rgb()) * illumination.radiance.rgb() * (nl * visibility);
        ++result.lights_evaluated;
    }
    result.color = hdr(radiance, surface.opacity);
    return result;
}

ShadingResult shade_material(const PbrMaterial& material, const SurfaceSample& surface,
                              const ShadingContext& context, const TextureLibrary* textures) noexcept {
    return shade_surface(sample_material(material, surface, textures), context);
}

MaterialService::MaterialService(MaterialLimits limits) : materials_(limits) {}
MaterialLibrary& MaterialService::materials() noexcept { return materials_; }
const MaterialLibrary& MaterialService::materials() const noexcept { return materials_; }
std::span<const Light> MaterialService::lights() const noexcept { return lights_; }

bool MaterialService::set_lights(std::span<const Light> incoming_lights) {
    if (incoming_lights.size() > materials_.limits().lights ||
        std::any_of(incoming_lights.begin(), incoming_lights.end(),
                    [](const Light& light) { return !validate_light(light); })) {
        return false;
    }
    std::vector<Light> replacement(incoming_lights.begin(), incoming_lights.end());
    lights_.swap(replacement);
    advance(lighting_revision_);
    return true;
}

std::optional<std::size_t> MaterialService::add_light(const Light& light) {
    if (lights_.size() >= materials_.limits().lights || !validate_light(light)) {
        return std::nullopt;
    }
    lights_.push_back(light);
    advance(lighting_revision_);
    return lights_.size() - 1;
}

bool MaterialService::set_light(std::size_t index, const Light& light) noexcept {
    if (index >= lights_.size() || !validate_light(light)) {
        return false;
    }
    lights_[index] = light;
    advance(lighting_revision_);
    return true;
}

bool MaterialService::erase_light(std::size_t index) noexcept {
    if (index >= lights_.size()) {
        return false;
    }
    lights_.erase(lights_.begin() + static_cast<std::ptrdiff_t>(index));
    advance(lighting_revision_);
    return true;
}

void MaterialService::clear_lights() noexcept {
    if (!lights_.empty()) {
        lights_.clear();
        advance(lighting_revision_);
    }
}

Color MaterialService::ambient() const noexcept { return ambient_; }

bool MaterialService::set_ambient(Color irradiance) noexcept {
    if (!color_range(irradiance, material_hdr_limit)) {
        return false;
    }
    ambient_ = irradiance;
    advance(lighting_revision_);
    return true;
}

ShadingContext MaterialService::context(math::Vec3 view_direction) const noexcept {
    ShadingContext result;
    result.view_direction = view_direction;
    result.ambient = ambient_;
    result.lights = lights_;
    result.max_lights = materials_.limits().lights;
    return result;
}

const PbrMaterial& MaterialService::resolve(const ecs::Registry& registry, ge::RuntimeHandle entity,
                                            std::uint32_t slot) const noexcept {
    const auto* material = materials_.get(materials_.assigned(registry, entity, slot));
    return material ? *material : fallback_;
}

const PbrMaterial& MaterialService::fallback() const noexcept { return fallback_; }

bool MaterialService::set_fallback(const PbrMaterial& material) noexcept {
    if (!validate_material(material)) {
        return false;
    }
    fallback_ = material;
    return true;
}

std::uint64_t MaterialService::lighting_revision() const noexcept { return lighting_revision_; }

namespace {

bool edit_property(PbrMaterial& material, core::CommandArgs args) {
    if (args.size() < 2) {
        return false;
    }
    const auto& field = args[0];
    if (field == "albedo" || field == "emissive") {
        if (args.size() != 4 && args.size() != 5) {
            return false;
        }
        Color value{Color::white()};
        if (!parse_number(args[1], value.r) || !parse_number(args[2], value.g) ||
            !parse_number(args[3], value.b) || (args.size() == 5 && !parse_number(args[4], value.a))) {
            return false;
        }
        (field == "albedo" ? material.albedo : material.emissive) = value;
        return true;
    }
    if (args.size() != 2) {
        return false;
    }
    const std::array<std::pair<std::string_view, double*>, 7> factors{{
        {"metalness", &material.metalness}, {"roughness", &material.roughness},
        {"dielectric_f0", &material.dielectric_f0}, {"emissive_strength", &material.emissive_strength},
        {"normal_scale", &material.normal_scale}, {"occlusion_strength", &material.occlusion_strength},
        {"alpha_cutoff", &material.alpha_cutoff}}};
    for (const auto& [name, value] : factors) {
        if (field == name) {
            return parse_number(args[1], *value);
        }
    }
    if (field == "alpha") {
        if (args[1] == "opaque") { material.alpha_mode = AlphaMode::Opaque; }
        else if (args[1] == "mask") { material.alpha_mode = AlphaMode::Mask; }
        else if (args[1] == "blend") { material.alpha_mode = AlphaMode::Blend; }
        else { return false; }
        return true;
    }
    if (field == "model") {
        if (args[1] == "pbr") { material.shading_model = ShadingModel::MetallicRoughness; }
        else if (args[1] == "lambert") { material.shading_model = ShadingModel::Lambert; }
        else if (args[1] == "unlit") { material.shading_model = ShadingModel::Unlit; }
        else { return false; }
        return true;
    }
    if (field == "double_sided" || field == "vertex_color") {
        if (args[1] != "true" && args[1] != "false" && args[1] != "1" && args[1] != "0") {
            return false;
        }
        const bool enabled = args[1] == "true" || args[1] == "1";
        (field == "double_sided" ? material.double_sided : material.use_vertex_color) = enabled;
        return true;
    }
    return false;
}

} // namespace

void register_material(core::Kernel& kernel) {
    if (!kernel.register_module({"render.material", "Shared materials and bounded CPU physically based shading", 1})) {
        return;
    }
    kernel.services().emplace<MaterialService>();
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<MaterialComponent>("render.material");
    }
    kernel.add_command("material.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1 && args.size() != 4 && args.size() != 5) {
            return {false, "usage: material.create NAME [R G B [A]]"};
        }
        PbrMaterial material;
        if (args.size() >= 4 && (!parse_number(args[1], material.albedo.r) ||
            !parse_number(args[2], material.albedo.g) || !parse_number(args[3], material.albedo.b))) {
            return {false, "albedo components must be finite numbers"};
        }
        if (args.size() == 5 && !parse_number(args[4], material.albedo.a)) {
            return {false, "alpha must be a finite number"};
        }
        const auto handle = host.services().get<MaterialService>().materials().create(args[0], material);
        return {true, "created material " + handle_text(handle)};
    }, "Create a linear-color material in the shared generation-checked library");
    kernel.add_command("material.set", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 3 || args.size() > 6) {
            return {false, "usage: material.set NAME FIELD VALUE..."};
        }
        auto& library = host.services().get<MaterialService>().materials();
        const auto handle = library.find(args[0]);
        const auto* previous = library.get(handle);
        if (!previous) {
            return {false, "material name is missing"};
        }
        auto replacement = *previous;
        if (!edit_property(replacement, args.subspan(1))) {
            return {false, "unknown property or invalid values; colors use RGB[A], factors use one number"};
        }
        const auto validation = validate_material(replacement);
        if (!validation) {
            return invalid_material(validation);
        }
        const bool changed = library.replace(handle, replacement);
        return {changed, changed ? "material updated" : "material handle became stale"};
    }, "Edit albedo, emissive, PBR factors, alpha/model modes, double_sided or vertex_color");
    kernel.add_command("material.texture", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3 && args.size() != 4) {
            return {false, "usage: material.texture NAME SLOT TEXTURE_NAME|none [r|g|b|a]"};
        }
        auto& library = host.services().get<MaterialService>().materials();
        const auto handle = library.find(args[0]);
        const auto* previous = library.get(handle);
        if (!previous) { return {false, "material name is missing"}; }
        auto replacement = *previous;
        auto* binding = select_texture(replacement, args[1]);
        if (!binding) { return {false, "unknown texture slot"}; }
        if (args[2] == "none") {
            binding->texture = {};
        } else {
            const auto* images = host.services().find<ImageService>();
            const auto texture = images ? images->textures().find(args[2]) : std::nullopt;
            if (!texture) { return {false, "texture name or image service is missing"}; }
            binding->texture = *texture;
        }
        if (args.size() == 4) {
            if (args[3] == "r") { binding->channel = TextureChannel::Red; }
            else if (args[3] == "g") { binding->channel = TextureChannel::Green; }
            else if (args[3] == "b") { binding->channel = TextureChannel::Blue; }
            else if (args[3] == "a") { binding->channel = TextureChannel::Alpha; }
            else { return {false, "channel must be r, g, b or a"}; }
        }
        const bool changed = library.replace(handle, replacement);
        return {changed, changed ? "texture binding updated" : "texture binding rejected"};
    }, "Bind an existing image-service texture to albedo/metalness/roughness/normal/emissive/occlusion");
    kernel.add_command("material.uv", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 6 && args.size() != 7) {
            return {false, "usage: material.uv NAME SLOT SCALE_U SCALE_V OFFSET_U OFFSET_V [ROTATION_RADIANS]"};
        }
        auto& library = host.services().get<MaterialService>().materials();
        const auto handle = library.find(args[0]);
        const auto* previous = library.get(handle);
        if (!previous) { return {false, "material name is missing"}; }
        auto replacement = *previous;
        auto* binding = select_texture(replacement, args[1]);
        if (!binding || !parse_number(args[2], binding->scale.x) || !parse_number(args[3], binding->scale.y) ||
            !parse_number(args[4], binding->offset.x) || !parse_number(args[5], binding->offset.y) ||
            (args.size() == 7 && !parse_number(args[6], binding->rotation))) {
            return {false, "invalid texture slot or UV transform"};
        }
        const bool changed = library.replace(handle, replacement);
        return {changed, changed ? "UV transform updated" : "UV transform exceeds material limits"};
    }, "Set a texture's UV transform; derivatives receive the same scale and rotation");
    kernel.add_command("material.assign", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2 && args.size() != 3) {
            return {false, "usage: material.assign ENTITY_NAME_OR_INDEX:GENERATION MATERIAL_NAME [SLOT]"};
        }
        auto* registry = host.services().find<ecs::Registry>();
        if (!registry) { return {false, "entity registry is unavailable"}; }
        std::uint32_t slot{};
        if (args.size() == 3 && !parse_integer(args[2], slot)) {
            return {false, "slot must be an unsigned integer"};
        }
        const auto& library = host.services().get<MaterialService>().materials();
        const bool assigned = library.assign(*registry, resolve_entity(*registry, args[0]), library.find(args[1]), slot);
        return {assigned, assigned ? "material assigned" : "invalid entity, missing mesh/material or slot limit"};
    }, "Assign a material to a mesh entity's submesh material slot");
    kernel.add_command("material.sample", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3 && args.size() != 9) {
            return {false, "usage: material.sample NAME U V [PX PY PZ NX NY NZ]"};
        }
        const auto& service = host.services().get<MaterialService>();
        const auto* material = service.materials().get(service.materials().find(args[0]));
        if (!material) { return {false, "material name is missing"}; }
        SurfaceSample surface;
        if (!parse_number(args[1], surface.uv.x) || !parse_number(args[2], surface.uv.y)) {
            return {false, "UV must contain finite numbers"};
        }
        if (args.size() == 9) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!parse_number(args[3 + axis], surface.world_position[axis]) ||
                    !parse_number(args[6 + axis], surface.world_normal[axis])) {
                    return {false, "position and normal must contain finite numbers"};
                }
            }
        }
        const auto* images = host.services().find<ImageService>();
        const auto result = shade_material(*material, surface, service.context(surface.world_normal),
                                           images ? &images->textures() : nullptr);
        std::ostringstream output;
        output << std::setprecision(10) << "rgba=" << result.color.r << ',' << result.color.g << ','
               << result.color.b << ',' << result.color.a << " discarded=" << result.discarded
               << " lights=" << result.lights_evaluated << " missing_textures="
               << static_cast<unsigned int>(result.missing_textures);
        return {result.valid, result.valid ? output.str() : "surface attributes are invalid"};
    }, "Evaluate a world-space surface with current materials, textures, ambient and lights");
    kernel.add_command("material.light", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& service = host.services().get<MaterialService>();
        if (args.size() == 1 && args[0] == "clear") {
            service.clear_lights();
            return {true, "lights cleared"};
        }
        Light light;
        if (args.size() == 8 && args[0] == "directional") { light.type = LightType::Directional; }
        else if (args.size() == 9 && args[0] == "point") { light.type = LightType::Point; }
        else if (args.size() == 14 && args[0] == "spot") { light.type = LightType::Spot; }
        else {
            return {false, "usage: material.light directional DX DY DZ R G B INTENSITY | "
                "point X Y Z R G B INTENSITY RANGE | spot X Y Z DX DY DZ R G B INTENSITY RANGE INNER_DEG OUTER_DEG | clear"};
        }
        std::array<double, 13> values{};
        for (std::size_t index = 1; index < args.size(); ++index) {
            if (!parse_number(args[index], values[index - 1])) {
                return {false, "light parameters must be finite numbers"};
            }
        }
        const math::Vec3 first{values[0], values[1], values[2]};
        if (light.type == LightType::Directional) { light.direction = first; }
        else { light.position = first; }
        const std::size_t color_index = light.type == LightType::Spot ? 6U : 3U;
        light.color = {values[color_index], values[color_index + 1], values[color_index + 2], 1.0};
        light.intensity = values[color_index + 3];
        if (light.type != LightType::Directional) { light.range = values[color_index + 4]; }
        if (light.type == LightType::Spot) {
            light.direction = {values[3], values[4], values[5]};
            light.inner_cone = math::radians(values[11]);
            light.outer_cone = math::radians(values[12]);
        }
        const auto validation = validate_light(light);
        if (!validation) { return invalid_material(validation); }
        const auto index = service.add_light(light);
        return {index.has_value(), index ? "added light " + std::to_string(*index) : "light capacity exhausted"};
    }, "Add a shared directional, point or spot light, or clear all live lights");
    kernel.add_command("material.ambient", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        Color value;
        if (args.size() != 3 || !parse_number(args[0], value.r) ||
            !parse_number(args[1], value.g) || !parse_number(args[2], value.b)) {
            return {false, "usage: material.ambient R G B"};
        }
        const bool changed = host.services().get<MaterialService>().set_ambient(value);
        return {changed, changed ? "ambient irradiance updated" : "ambient irradiance is out of bounds"};
    }, "Set diffuse ambient irradiance in linear HDR units");
    kernel.add_command("material.erase", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: material.erase NAME"}; }
        auto& library = host.services().get<MaterialService>().materials();
        const bool erased = library.erase(library.find(args[0]));
        return {erased, erased ? "material erased; stale assignments use fallback" : "material name is missing"};
    }, "Invalidate a material generation without reusing stale entity assignments");
    kernel.add_system({"render.material.state", core::Phase::Render, -200, {}},
        [](core::Kernel& host, const core::Frame&) {
            const auto& service = host.services().get<MaterialService>();
            const auto& library = service.materials();
            std::size_t invalid_assignments{};
            if (auto* registry = host.services().find<ecs::Registry>()) {
                if (registry->component_type<MaterialComponent>() == 0) {
                    registry->register_component<MaterialComponent>("render.material");
                }
                registry->each<MaterialComponent>([&](ge::RuntimeHandle entity, const MaterialComponent& component) {
                    if (component.slots.size() > library.limits().slots_per_entity ||
                        !registry->has<geometry::MeshComponent>(entity)) {
                        ++invalid_assignments;
                        return;
                    }
                    for (const auto material : component.slots) {
                        if (material && !library.get(material)) { ++invalid_assignments; }
                    }
                });
            }
            host.metric("material.count", static_cast<double>(library.size()));
            host.metric("material.lights", static_cast<double>(service.lights().size()));
            host.metric("material.invalid_assignments", static_cast<double>(invalid_assignments));
        });
}

} // namespace ge::render
