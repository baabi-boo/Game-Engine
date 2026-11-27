#include <ge/physics/collision.hpp>
#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ge::physics {
namespace {

using math::Vec3;
constexpr double tiny = 1.0e-12;
constexpr double coordinate_limit = 1.0e12;

bool bounded(Vec3 value) noexcept {
    return math::finite(value) && std::abs(value.x) <= coordinate_limit &&
        std::abs(value.y) <= coordinate_limit && std::abs(value.z) <= coordinate_limit;
}

bool valid_transform(const math::Transform& value) noexcept {
    return math::finite(value) && bounded(value.position) && bounded(value.scale) &&
        std::abs(value.scale.x) > tiny && std::abs(value.scale.y) > tiny &&
        std::abs(value.scale.z) > tiny && math::length(value.rotation) > tiny;
}

bool valid_material(CollisionMaterial material) noexcept {
    return std::isfinite(material.friction) && material.friction >= 0.0 &&
        material.friction <= 1000.0 && std::isfinite(material.restitution) &&
        material.restitution >= 0.0 && material.restitution <= 1.0;
}

bool is_plane(const CollisionShape& shape) noexcept {
    return std::holds_alternative<math::Plane>(shape);
}

struct BoxFrame {
    Vec3 center;
    Vec3 half;
    std::array<Vec3, 3> axes;
};

BoxFrame frame(const math::Obb& box) noexcept {
    const auto rotation = math::normalized(box.orientation);
    return {box.center, box.half_extents,
        {math::rotate(rotation, {1.0, 0.0, 0.0}),
         math::rotate(rotation, {0.0, 1.0, 0.0}),
         math::rotate(rotation, {0.0, 0.0, 1.0})}};
}

Vec3 local_point(const BoxFrame& box, Vec3 point) noexcept {
    const auto offset = point - box.center;
    return {math::dot(offset, box.axes[0]), math::dot(offset, box.axes[1]),
            math::dot(offset, box.axes[2])};
}

Vec3 world_point(const BoxFrame& box, Vec3 point) noexcept {
    return box.center + box.axes[0] * point.x + box.axes[1] * point.y + box.axes[2] * point.z;
}

Vec3 support(const BoxFrame& box, Vec3 direction) noexcept {
    auto result = box.center;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double sign = math::dot(box.axes[axis], direction) < 0.0 ? -1.0 : 1.0;
        result += box.axes[axis] * (box.half[axis] * sign);
    }
    return result;
}

double radius_on(const BoxFrame& box, Vec3 axis) noexcept {
    return box.half.x * std::abs(math::dot(box.axes[0], axis)) +
        box.half.y * std::abs(math::dot(box.axes[1], axis)) +
        box.half.z * std::abs(math::dot(box.axes[2], axis));
}

void add_point(ContactManifold& result, Vec3 first, Vec3 second,
               double penetration, std::uint64_t feature = 0) noexcept {
    if (!math::finite(first) || !math::finite(second) || !std::isfinite(penetration)) {
        return;
    }
    ContactPoint point{first, second, std::max(0.0, penetration), feature};
    for (std::size_t i = 0; i < result.point_count; ++i) {
        if (math::distance_squared(result.points[i].point_a, first) <= tiny * tiny) {
            if (point.penetration > result.points[i].penetration) {
                result.points[i] = point;
            }
            return;
        }
    }
    if (result.point_count < result.points.size()) {
        result.points[result.point_count++] = point;
        return;
    }
    auto shallowest = std::min_element(result.points.begin(), result.points.end(),
        [](const ContactPoint& a, const ContactPoint& b) { return a.penetration < b.penetration; });
    if (point.penetration > shallowest->penetration) {
        *shallowest = point;
    }
}

ContactManifold reversed(ContactManifold value) noexcept {
    value.normal = -value.normal;
    for (std::size_t i = 0; i < value.point_count; ++i) {
        std::swap(value.points[i].point_a, value.points[i].point_b);
    }
    return value;
}

ContactManifold sphere_sphere(const math::Sphere& a, const math::Sphere& b,
                              double tolerance) noexcept {
    ContactManifold result;
    const auto offset = b.center - a.center;
    const double distance = math::length(offset);
    const double radii = a.radius + b.radius;
    if (distance > radii + tolerance) {
        return result;
    }
    result.normal = distance > tiny ? offset / distance : Vec3{1.0, 0.0, 0.0};
    add_point(result, a.center + result.normal * a.radius,
        b.center - result.normal * b.radius, radii - distance);
    return result;
}

ContactManifold sphere_box(const math::Sphere& sphere, const math::Obb& box,
                           double tolerance) noexcept {
    ContactManifold result;
    const auto basis = frame(box);
    const auto position = local_point(basis, sphere.center);
    auto closest = math::clamp(position, -basis.half, basis.half);
    const auto offset = world_point(basis, closest) - sphere.center;
    const double distance = math::length(offset);
    if (distance > sphere.radius + tolerance) {
        return result;
    }
    double depth = sphere.radius - distance;
    std::size_t feature = 0;
    if (distance > tiny) {
        result.normal = offset / distance;
    } else {
        const auto gaps = basis.half - math::abs(position);
        feature = gaps.y < gaps.x ? 1U : 0U;
        feature = gaps.z < gaps[feature] ? 2U : feature;
        const double sign = position[feature] < 0.0 ? -1.0 : 1.0;
        result.normal = basis.axes[feature] * -sign;
        closest[feature] = basis.half[feature] * sign;
        depth = sphere.radius + gaps[feature];
    }
    add_point(result, sphere.center + result.normal * sphere.radius,
        world_point(basis, closest), depth, static_cast<std::uint64_t>(feature));
    return result;
}

ContactManifold sphere_plane(const math::Sphere& sphere, const math::Plane& source,
                             double tolerance) noexcept {
    ContactManifold result;
    const auto plane = *math::normalized(source);
    const double distance = math::dot(plane.normal, sphere.center) + plane.distance;
    if (distance <= sphere.radius + tolerance) {
        result.normal = -plane.normal;
        add_point(result, sphere.center - plane.normal * sphere.radius,
            sphere.center - plane.normal * distance, sphere.radius - distance);
    }
    return result;
}

ContactManifold box_plane(const math::Obb& box, const math::Plane& source,
                          double tolerance) noexcept {
    ContactManifold result;
    const auto plane = *math::normalized(source);
    const auto basis = frame(box);
    result.normal = -plane.normal;
    for (std::uint64_t corner = 0; corner < 8; ++corner) {
        Vec3 local;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            local[axis] = basis.half[axis] * ((corner & (std::uint64_t{1} << axis)) ? 1.0 : -1.0);
        }
        const auto point = world_point(basis, local);
        const double distance = math::dot(plane.normal, point) + plane.distance;
        if (distance <= tolerance) {
            add_point(result, point, point - plane.normal * distance, -distance, corner);
        }
    }
    return result;
}

struct ClipPolygon {
    std::array<Vec3, 12> vertices{};
    std::size_t count{};
};

ClipPolygon clip(const ClipPolygon& input, Vec3 normal, double distance,
                 double tolerance) noexcept {
    ClipPolygon output;
    if (input.count == 0) {
        return output;
    }
    auto previous = input.vertices[input.count - 1];
    double previous_distance = math::dot(normal, previous) - distance;
    for (std::size_t index = 0; index < input.count; ++index) {
        const auto current = input.vertices[index];
        const double current_distance = math::dot(normal, current) - distance;
        const bool inside = current_distance <= tolerance;
        const bool previous_inside = previous_distance <= tolerance;
        if (inside != previous_inside) {
            const double denominator = previous_distance - current_distance;
            if (std::abs(denominator) > tiny && output.count < output.vertices.size()) {
                const double amount = math::clamp(previous_distance / denominator, 0.0, 1.0);
                output.vertices[output.count++] = previous + (current - previous) * amount;
            }
        }
        if (inside && output.count < output.vertices.size()) {
            output.vertices[output.count++] = current;
        }
        previous = current;
        previous_distance = current_distance;
    }
    return output;
}

void face_contacts(ContactManifold& result, const BoxFrame& reference,
                   const BoxFrame& incident, std::size_t reference_axis,
                   bool reference_is_first, double tolerance) noexcept {
    const auto normal = reference_is_first ? result.normal : -result.normal;
    const auto reference_center = reference.center + normal * reference.half[reference_axis];
    std::size_t incident_axis = 0;
    for (std::size_t axis = 1; axis < 3; ++axis) {
        if (std::abs(math::dot(incident.axes[axis], normal)) >
            std::abs(math::dot(incident.axes[incident_axis], normal))) {
            incident_axis = axis;
        }
    }
    const double sign = math::dot(incident.axes[incident_axis], normal) > 0.0 ? -1.0 : 1.0;
    const auto incident_center = incident.center +
        incident.axes[incident_axis] * (incident.half[incident_axis] * sign);
    const std::size_t u = (incident_axis + 1) % 3;
    const std::size_t v = (incident_axis + 2) % 3;
    const auto horizontal = incident.axes[u] * incident.half[u];
    const auto vertical = incident.axes[v] * incident.half[v];
    ClipPolygon polygon{{incident_center - horizontal - vertical,
                         incident_center + horizontal - vertical,
                         incident_center + horizontal + vertical,
                         incident_center - horizontal + vertical}, 4};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (axis != reference_axis) {
            const auto side = reference.axes[axis];
            polygon = clip(polygon, side, math::dot(side, reference.center) + reference.half[axis], tolerance);
            polygon = clip(polygon, -side, math::dot(-side, reference.center) + reference.half[axis], tolerance);
        }
    }
    for (std::size_t index = 0; index < polygon.count; ++index) {
        const auto point = polygon.vertices[index];
        const double depth = math::dot(normal, reference_center - point);
        if (depth >= -tolerance) {
            const auto projected = point + normal * depth;
            const std::uint64_t feature = (static_cast<std::uint64_t>(reference_axis) << 16U) |
                (static_cast<std::uint64_t>(incident_axis) << 8U) | static_cast<std::uint64_t>(index);
            add_point(result, reference_is_first ? projected : point,
                reference_is_first ? point : projected, depth, feature);
        }
    }
}

math::Segment supporting_edge(const BoxFrame& box, std::size_t axis, Vec3 normal) noexcept {
    auto center = box.center;
    for (std::size_t other = 0; other < 3; ++other) {
        if (other != axis) {
            const double sign = math::dot(box.axes[other], normal) < 0.0 ? -1.0 : 1.0;
            center += box.axes[other] * (box.half[other] * sign);
        }
    }
    const auto extent = box.axes[axis] * box.half[axis];
    return {center - extent, center + extent};
}

ContactManifold box_box(const math::Obb& first, const math::Obb& second,
                        double tolerance) noexcept {
    ContactManifold result;
    const auto a = frame(first);
    const auto b = frame(second);
    const auto offset = b.center - a.center;
    double minimum = math::infinity;
    std::size_t feature = 0;
    const auto test_axis = [&](Vec3 axis, std::size_t candidate) {
        const double magnitude = math::length(axis);
        if (magnitude <= 1.0e-8) {
            return true;
        }
        axis /= magnitude;
        const double separation = math::dot(offset, axis);
        const double overlap = radius_on(a, axis) + radius_on(b, axis) - std::abs(separation);
        if (overlap < -tolerance) {
            return false;
        }
        if (overlap < minimum - tiny) {
            minimum = overlap;
            result.normal = separation < 0.0 ? -axis : axis;
            feature = candidate;
        }
        return true;
    };
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!test_axis(a.axes[axis], axis) || !test_axis(b.axes[axis], axis + 3)) {
            return {};
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            if (!test_axis(math::cross(a.axes[i], b.axes[j]), 6 + i * 3 + j)) {
                return {};
            }
        }
    }
    if (feature < 3) {
        face_contacts(result, a, b, feature, true, tolerance);
    } else if (feature < 6) {
        face_contacts(result, b, a, feature - 3, false, tolerance);
    } else {
        const auto edge_a = supporting_edge(a, (feature - 6) / 3, result.normal);
        const auto edge_b = supporting_edge(b, (feature - 6) % 3, -result.normal);
        const auto closest = closest_segments(edge_a, edge_b);
        add_point(result, closest.first, closest.second, minimum, static_cast<std::uint64_t>(feature));
    }
    if (result.point_count == 0) {
        add_point(result, support(a, result.normal), support(b, -result.normal), minimum);
    }
    return result;
}

ContactManifold capsule_sphere(const math::Capsule& capsule, const math::Sphere& sphere,
                               double tolerance) noexcept {
    const auto nearest = math::closest_point(math::Segment{capsule.a, capsule.b}, sphere.center);
    if (math::distance_squared(nearest, sphere.center) <= tiny * tiny) {
        ContactManifold result;
        result.normal = math::orthonormal_basis(math::normalized(capsule.b - capsule.a,
            {0.0, 1.0, 0.0})).first;
        add_point(result, nearest + result.normal * capsule.radius,
            sphere.center - result.normal * sphere.radius, capsule.radius + sphere.radius);
        return result;
    }
    return sphere_sphere({nearest, capsule.radius}, sphere, tolerance);
}

ContactManifold capsule_capsule(const math::Capsule& a, const math::Capsule& b,
                                double tolerance) noexcept {
    const auto nearest = closest_segments({a.a, a.b}, {b.a, b.b});
    auto result = sphere_sphere({nearest.first, a.radius}, {nearest.second, b.radius}, tolerance);
    const auto direction_a = a.b - a.a;
    const auto direction_b = b.b - b.a;
    if (math::distance_squared(nearest.first, nearest.second) <= tiny * tiny) {
        const auto perpendicular = math::cross(math::normalized(direction_a), math::normalized(direction_b));
        const auto fallback = math::orthonormal_basis(math::normalized(direction_a,
            math::normalized(direction_b, {0.0, 1.0, 0.0}))).first;
        result.normal = math::normalized(perpendicular, fallback);
        result.points[0].point_a = nearest.first + result.normal * a.radius;
        result.points[0].point_b = nearest.second - result.normal * b.radius;
    }
    if (result.touching() && math::length_squared(direction_a) > tiny &&
        math::length_squared(direction_b) > tiny &&
        math::length_squared(math::cross(math::normalized(direction_a), math::normalized(direction_b))) < tiny) {
        for (const auto endpoint : {a.a, a.b}) {
            const auto other = math::closest_point(math::Segment{b.a, b.b}, endpoint);
            const auto contact = sphere_sphere({endpoint, a.radius}, {other, b.radius}, tolerance);
            if (contact.touching() && math::dot(contact.normal, result.normal) > 0.999) {
                add_point(result, contact.points[0].point_a, contact.points[0].point_b,
                    contact.points[0].penetration, 1);
            }
        }
    }
    return result;
}

SegmentClosestPoints segment_box(math::Segment segment, const BoxFrame& box) noexcept {
    const auto start = local_point(box, segment.a);
    const auto direction = local_point(box, segment.b) - start;
    std::array<double, 8> breaks{0.0, 1.0};
    std::size_t count = 2;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) > tiny) {
            for (const double bound : {-box.half[axis], box.half[axis]}) {
                const double parameter = (bound - start[axis]) / direction[axis];
                if (parameter > 0.0 && parameter < 1.0) {
                    breaks[count++] = parameter;
                }
            }
        }
    }
    std::sort(breaks.begin(), breaks.begin() + static_cast<std::ptrdiff_t>(count));
    double best_distance = math::infinity;
    double best_parameter = 0.0;
    const auto consider = [&](double parameter) {
        const auto point = start + direction * parameter;
        const auto clamped = math::clamp(point, -box.half, box.half);
        const double distance = math::distance_squared(point, clamped);
        if (distance < best_distance) {
            best_distance = distance;
            best_parameter = parameter;
        }
    };
    for (std::size_t interval = 0; interval + 1 < count; ++interval) {
        const double low = breaks[interval];
        const double high = breaks[interval + 1];
        const auto midpoint = start + direction * ((low + high) * 0.5);
        double numerator = 0.0;
        double denominator = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (midpoint[axis] < -box.half[axis] || midpoint[axis] > box.half[axis]) {
                const double boundary = midpoint[axis] < 0.0 ? -box.half[axis] : box.half[axis];
                numerator += direction[axis] * (start[axis] - boundary);
                denominator += direction[axis] * direction[axis];
            }
        }
        consider(low);
        consider(high);
        if (denominator > tiny) {
            consider(math::clamp(-numerator / denominator, low, high));
        }
    }
    const auto point = start + direction * best_parameter;
    return {world_point(box, point), world_point(box, math::clamp(point, -box.half, box.half)),
        best_parameter, 0.0};
}

ContactManifold capsule_box(const math::Capsule& capsule, const math::Obb& box,
                            double tolerance) noexcept {
    ContactManifold result;
    const auto basis = frame(box);
    const auto nearest = segment_box({capsule.a, capsule.b}, basis);
    const double distance = math::distance(nearest.first, nearest.second);
    if (distance > capsule.radius + tolerance) {
        return result;
    }
    if (distance > tiny) {
        result.normal = (nearest.second - nearest.first) / distance;
        add_point(result, nearest.first + result.normal * capsule.radius,
            nearest.second, capsule.radius - distance);
        return result;
    }
    // An intersecting centerline uses a conservative separating box face. This
    // gives a valid separation distance, though an edge direction may be shorter.
    const auto first = local_point(basis, capsule.a);
    const auto second = local_point(basis, capsule.b);
    double best = math::infinity;
    std::size_t selected_axis = 0;
    double selected_sign = 1.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (const double sign : {-1.0, 1.0}) {
            const double low = std::min(first[axis] * sign, second[axis] * sign);
            const double depth = basis.half[axis] - low + capsule.radius;
            if (depth < best) {
                best = depth;
                selected_axis = axis;
                selected_sign = sign;
            }
        }
    }
    const auto endpoint = first[selected_axis] * selected_sign < second[selected_axis] * selected_sign
        ? first : second;
    auto surface = math::clamp(endpoint, -basis.half, basis.half);
    surface[selected_axis] = basis.half[selected_axis] * selected_sign;
    result.normal = basis.axes[selected_axis] * -selected_sign;
    add_point(result, world_point(basis, endpoint) + result.normal * capsule.radius,
        world_point(basis, surface), best, static_cast<std::uint64_t>(selected_axis));
    return result;
}

ContactManifold capsule_plane(const math::Capsule& capsule, const math::Plane& plane,
                              double tolerance) noexcept {
    auto result = sphere_plane({capsule.a, capsule.radius}, plane, tolerance);
    const auto second = sphere_plane({capsule.b, capsule.radius}, plane, tolerance);
    if (second.touching()) {
        result.normal = second.normal;
        add_point(result, second.points[0].point_a, second.points[0].point_b,
            second.points[0].penetration, 1);
    }
    return result;
}

ContactManifold sphere_triangle(const math::Sphere& sphere, const math::Triangle& triangle,
                                double tolerance) noexcept {
    ContactManifold result;
    const auto closest = math::closest_point(triangle, sphere.center);
    const auto offset = closest - sphere.center;
    const double distance = math::length(offset);
    if (distance > sphere.radius + tolerance) {
        return result;
    }
    const auto face_normal = math::normalized(math::cross(triangle.b - triangle.a, triangle.c - triangle.a));
    result.normal = distance > tiny ? offset / distance : -face_normal;
    add_point(result, sphere.center + result.normal * sphere.radius, closest, sphere.radius - distance);
    return result;
}

CollisionConfig checked_config(CollisionConfig value) {
    if (value.capacity > 65536 || value.max_pairs > 1048576 || value.max_manifolds > 262144 ||
        value.max_planes > 1024 || value.traversal_limit == 0 || value.traversal_limit > 16777216 ||
        !std::isfinite(value.contact_tolerance) || value.contact_tolerance < 0.0 ||
        value.contact_tolerance > 0.1 || !std::isfinite(value.fat_margin) ||
        value.fat_margin < 0.0 || value.fat_margin > 1000.0) {
        throw std::invalid_argument("collision limits or numeric tolerances are invalid");
    }
    return value;
}

} // namespace

ShapeKind shape_kind(const CollisionShape& shape) noexcept {
    return static_cast<ShapeKind>(shape.index());
}

bool valid_shape(const CollisionShape& shape) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, math::Sphere>) {
            return bounded(value.center) && std::isfinite(value.radius) &&
                value.radius > 0.0 && value.radius <= coordinate_limit;
        } else if constexpr (std::is_same_v<T, math::Obb>) {
            return bounded(value.center) && bounded(value.half_extents) &&
                value.half_extents.x > 0.0 && value.half_extents.y > 0.0 &&
                value.half_extents.z > 0.0 && math::finite(value.orientation) &&
                math::length(value.orientation) > tiny;
        } else if constexpr (std::is_same_v<T, math::Capsule>) {
            return bounded(value.a) && bounded(value.b) && std::isfinite(value.radius) &&
                value.radius > 0.0 && value.radius <= coordinate_limit;
        } else if constexpr (std::is_same_v<T, math::Plane>) {
            const auto plane = math::normalized(value);
            return plane && std::abs(plane->distance) <= coordinate_limit;
        } else {
            return bounded(value.a) && bounded(value.b) && bounded(value.c) &&
                math::length_squared(math::cross(value.b - value.a, value.c - value.a)) > tiny * tiny;
        }
    }, shape);
}

bool should_collide(CollisionFilter first, CollisionFilter second) noexcept {
    if (first.group != 0 && first.group == second.group) {
        return first.group > 0;
    }
    return (first.layers & second.mask) != 0 && (second.layers & first.mask) != 0;
}

SegmentClosestPoints closest_segments(math::Segment first, math::Segment second) noexcept {
    SegmentClosestPoints result{first.a, second.a, 0.0, 0.0};
    if (!bounded(first.a) || !bounded(first.b) || !bounded(second.a) || !bounded(second.b)) {
        return {};
    }
    const auto u = first.b - first.a;
    const auto v = second.b - second.a;
    const auto offset = first.a - second.a;
    const double a = math::dot(u, u);
    const double e = math::dot(v, v);
    const double f = math::dot(v, offset);
    double s = 0.0;
    double t = 0.0;
    if (a <= tiny && e <= tiny) {
        return result;
    }
    if (a <= tiny) {
        t = math::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = math::dot(u, offset);
        if (e <= tiny) {
            s = math::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = math::dot(u, v);
            const double determinant = a * e - b * b;
            if (determinant > tiny * a * e) {
                s = math::clamp((b * f - c * e) / determinant, 0.0, 1.0);
            }
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = math::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = math::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    return {first.a + u * s, second.a + v * t, s, t};
}

std::optional<math::Aabb> shape_bounds(const CollisionShape& shape) noexcept {
    if (!valid_shape(shape)) {
        return std::nullopt;
    }
    return std::visit([](const auto& value) -> std::optional<math::Aabb> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, math::Plane>) {
            return std::nullopt;
        } else {
            const auto result = math::bounds(value);
            return math::valid(result) ? std::optional<math::Aabb>{result} : std::nullopt;
        }
    }, shape);
}

std::optional<CollisionShape> transform_shape(const CollisionShape& shape,
                                              const math::Transform& transform) noexcept {
    if (!valid_shape(shape) || !valid_transform(transform)) {
        return std::nullopt;
    }
    const auto matrix = math::matrix(transform);
    const auto scale = math::abs(transform.scale);
    const double radius_scale = std::max({scale.x, scale.y, scale.z});
    const auto candidate = std::visit([&](const auto& value) -> std::optional<CollisionShape> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, math::Sphere>) {
            return math::Sphere{math::transform_point(matrix, value.center), value.radius * radius_scale};
        } else if constexpr (std::is_same_v<T, math::Capsule>) {
            return math::Capsule{math::transform_point(matrix, value.a), math::transform_point(matrix, value.b),
                value.radius * radius_scale};
        } else if constexpr (std::is_same_v<T, math::Obb>) {
            const auto composed = math::decompose(matrix * math::trs_matrix(value.center, value.orientation,
                {1.0, 1.0, 1.0}));
            if (!composed) {
                return std::nullopt;
            }
            return math::Obb{composed->position, value.half_extents * math::abs(composed->scale), composed->rotation};
        } else if constexpr (std::is_same_v<T, math::Plane>) {
            const auto plane = math::transform_plane(matrix, value);
            return plane ? std::optional<CollisionShape>{*plane} : std::nullopt;
        } else {
            return math::Triangle{math::transform_point(matrix, value.a), math::transform_point(matrix, value.b),
                math::transform_point(matrix, value.c)};
        }
    }, shape);
    return candidate && valid_shape(*candidate) ? candidate : std::nullopt;
}

ContactManifold narrowphase(const CollisionShape& first, const CollisionShape& second,
                            double tolerance) noexcept {
    if (!valid_shape(first) || !valid_shape(second) || !std::isfinite(tolerance) ||
        tolerance < 0.0 || tolerance > 0.1) {
        return {};
    }
    return std::visit([tolerance](const auto& a, const auto& b) -> ContactManifold {
        using A = std::decay_t<decltype(a)>;
        using B = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<A, math::Sphere> && std::is_same_v<B, math::Sphere>) {
            return sphere_sphere(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Sphere> && std::is_same_v<B, math::Obb>) {
            return sphere_box(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Obb> && std::is_same_v<B, math::Sphere>) {
            return reversed(sphere_box(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Sphere> && std::is_same_v<B, math::Plane>) {
            return sphere_plane(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Plane> && std::is_same_v<B, math::Sphere>) {
            return reversed(sphere_plane(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Obb> && std::is_same_v<B, math::Obb>) {
            return box_box(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Obb> && std::is_same_v<B, math::Plane>) {
            return box_plane(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Plane> && std::is_same_v<B, math::Obb>) {
            return reversed(box_plane(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Capsule> && std::is_same_v<B, math::Sphere>) {
            return capsule_sphere(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Sphere> && std::is_same_v<B, math::Capsule>) {
            return reversed(capsule_sphere(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Capsule> && std::is_same_v<B, math::Capsule>) {
            return capsule_capsule(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Capsule> && std::is_same_v<B, math::Obb>) {
            return capsule_box(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Obb> && std::is_same_v<B, math::Capsule>) {
            return reversed(capsule_box(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Capsule> && std::is_same_v<B, math::Plane>) {
            return capsule_plane(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Plane> && std::is_same_v<B, math::Capsule>) {
            return reversed(capsule_plane(b, a, tolerance));
        } else if constexpr (std::is_same_v<A, math::Sphere> && std::is_same_v<B, math::Triangle>) {
            return sphere_triangle(a, b, tolerance);
        } else if constexpr (std::is_same_v<A, math::Triangle> && std::is_same_v<B, math::Sphere>) {
            return reversed(sphere_triangle(b, a, tolerance));
        } else {
            return {};
        }
    }, first, second);
}

CollisionWorld::CollisionWorld(ecs::Registry& shared_registry, CollisionConfig configuration)
    : registry_(shared_registry), config_(checked_config(configuration)), handles_(config_.capacity),
      tree_({config_.capacity, config_.fat_margin, 2.0, config_.traversal_limit}) {
    slots_.reserve(config_.capacity);
    pairs_.resize(config_.max_pairs);
    contacts_.reserve(config_.max_manifolds);
    (void)registry_.register_component<ColliderComponent>("physics.colliders");
}

ecs::Registry& CollisionWorld::registry() noexcept { return registry_; }
const CollisionConfig& CollisionWorld::config() const noexcept { return config_; }
std::size_t CollisionWorld::size() const noexcept { return handles_.size(); }

Collider* CollisionWorld::mutable_collider(ColliderHandle handle) noexcept {
    if (!handles_.alive({handle.index, handle.generation})) {
        return nullptr;
    }
    auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
    return slot ? &*slot : nullptr;
}

const Collider* CollisionWorld::get(ColliderHandle handle) const noexcept {
    if (!handles_.alive({handle.index, handle.generation})) {
        return nullptr;
    }
    const auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
    return slot && registry_.alive(slot->entity) ? &*slot : nullptr;
}

ColliderHandle CollisionWorld::from_proxy(spatial::ProxyHandle proxy) const noexcept {
    const auto found = proxy_owners_.find(proxy);
    return found != proxy_owners_.end() ? found->second : ColliderHandle{};
}

void CollisionWorld::invalidate_contacts() noexcept {
    contacts_.clear();
    statistics_ = {};
    statistics_.colliders = size();
}

void CollisionWorld::deactivate(Collider& collider) noexcept {
    if (collider.proxy) {
        (void)tree_.remove(collider.proxy);
        proxy_owners_.erase(collider.proxy);
        collider.proxy = {};
    }
    collider.active = false;
}

bool CollisionWorld::refresh(Collider& collider, const ColliderDescription& description,
                             const math::Transform& world_transform, bool enabled) {
    if (!valid_shape(description.shape) || !valid_material(description.material) ||
        !valid_transform(description.local_transform) || !valid_transform(world_transform)) {
        return false;
    }
    const auto combined = math::decompose(math::matrix(world_transform) * math::matrix(description.local_transform));
    const auto world_shape = combined ? transform_shape(description.shape, *combined) : std::nullopt;
    if (!world_shape) {
        return false;
    }
    const auto bounds = shape_bounds(*world_shape);
    const bool active = enabled && description.enabled && registry_.alive(collider.entity);
    if (is_plane(description.shape) && !is_plane(collider.description.shape)) {
        const auto planes = std::count_if(slots_.begin(), slots_.end(), [](const auto& slot) {
            return slot && is_plane(slot->description.shape);
        });
        if (static_cast<std::size_t>(planes) >= config_.max_planes) {
            return false;
        }
    }
    if (active && !is_plane(*world_shape) && !bounds) {
        return false;
    }
    if (active && bounds) {
        const auto expanded = math::expanded(*bounds, config_.contact_tolerance);
        if (collider.proxy) {
            const auto updated = tree_.update(collider.proxy, expanded,
                world_transform.position - collider.transform.position);
            if (updated == spatial::ProxyUpdate::InvalidBounds || updated == spatial::ProxyUpdate::InvalidHandle) {
                return false;
            }
        } else {
            const auto proxy = tree_.insert(collider.entity, expanded);
            if (!proxy) {
                return false;
            }
            try {
                proxy_owners_.emplace(proxy, collider.handle);
            } catch (...) {
                (void)tree_.remove(proxy);
                throw;
            }
            collider.proxy = proxy;
        }
    } else {
        deactivate(collider);
    }
    collider.description = description;
    collider.transform = world_transform;
    collider.world_shape = *world_shape;
    collider.bounds = bounds;
    collider.active = active;
    invalidate_contacts();
    return true;
}

ColliderHandle CollisionWorld::add(ecs::Entity entity, const ColliderDescription& description) {
    if (registry_.iterating() || !registry_.alive(entity) || !valid_shape(description.shape) ||
        !valid_material(description.material) || !valid_transform(description.local_transform)) {
        return {};
    }
    auto& component = registry_.emplace<ColliderComponent>(entity);
    if (component.colliders.size() >= config_.capacity) {
        return {};
    }
    const auto token = handles_.acquire();
    if (!token) {
        return {};
    }
    const ColliderHandle handle{token->index, token->generation};
    const auto index = static_cast<std::size_t>(handle.index - 1);
    try {
        if (slots_.size() <= index) {
            slots_.resize(index + 1);
        }
        slots_[index] = Collider{};
        auto& collider = *slots_[index];
        collider.handle = handle;
        collider.entity = entity;
        if (!refresh(collider, description, {}, false)) {
            slots_[index].reset();
            (void)handles_.release(*token);
            return {};
        }
        component.colliders.push_back(handle);
        (void)registry_.mark_changed<ColliderComponent>(entity);
    } catch (...) {
        if (index < slots_.size() && slots_[index]) {
            deactivate(*slots_[index]);
            slots_[index].reset();
        }
        std::erase(component.colliders, handle);
        (void)handles_.release(*token);
        throw;
    }
    return handle;
}

bool CollisionWorld::remove(ColliderHandle handle) {
    auto* collider = mutable_collider(handle);
    if (!collider || registry_.iterating()) {
        return false;
    }
    if (auto* component = registry_.get<ColliderComponent>(collider->entity)) {
        std::erase(component->colliders, handle);
        (void)registry_.mark_changed<ColliderComponent>(collider->entity);
    }
    deactivate(*collider);
    slots_[static_cast<std::size_t>(handle.index - 1)].reset();
    (void)handles_.release({handle.index, handle.generation});
    invalidate_contacts();
    return true;
}

std::size_t CollisionWorld::remove(ecs::Entity entity) {
    std::size_t removed = 0;
    for (auto& slot : slots_) {
        if (slot && slot->entity == entity && remove(slot->handle)) {
            ++removed;
        }
    }
    return removed;
}

void CollisionWorld::clear() {
    if (!registry_.iterating()) {
        for (auto& slot : slots_) {
            if (slot) {
                (void)remove(slot->handle);
            }
        }
    }
}

std::vector<ColliderHandle> CollisionWorld::colliders(ecs::Entity entity) const {
    std::vector<ColliderHandle> result;
    if (registry_.alive(entity)) {
        for (const auto& slot : slots_) {
            if (slot && slot->entity == entity) {
                result.push_back(slot->handle);
            }
        }
    }
    return result;
}

bool CollisionWorld::set_shape(ColliderHandle handle, const CollisionShape& shape) {
    auto* collider = mutable_collider(handle);
    if (!collider || !registry_.alive(collider->entity)) {
        return false;
    }
    auto description = collider->description;
    description.shape = shape;
    return refresh(*collider, description, collider->transform, collider->active);
}

bool CollisionWorld::set_filter(ColliderHandle handle, CollisionFilter filter) {
    auto* collider = mutable_collider(handle);
    if (!collider || !registry_.alive(collider->entity)) {
        return false;
    }
    collider->description.filter = filter;
    invalidate_contacts();
    return true;
}

bool CollisionWorld::set_material(ColliderHandle handle, CollisionMaterial material) {
    auto* collider = mutable_collider(handle);
    if (!collider || !registry_.alive(collider->entity) || !valid_material(material)) {
        return false;
    }
    collider->description.material = material;
    invalidate_contacts();
    return true;
}

bool CollisionWorld::set_enabled(ColliderHandle handle, bool enabled) {
    auto* collider = mutable_collider(handle);
    if (!collider || !registry_.alive(collider->entity)) {
        return false;
    }
    auto description = collider->description;
    description.enabled = enabled;
    return refresh(*collider, description, collider->transform, enabled && registry_.enabled(collider->entity));
}

bool CollisionWorld::set_trigger(ColliderHandle handle, bool trigger) {
    auto* collider = mutable_collider(handle);
    if (!collider || !registry_.alive(collider->entity)) {
        return false;
    }
    collider->description.trigger = trigger;
    invalidate_contacts();
    return true;
}

bool CollisionWorld::set_transform(ColliderHandle handle, const math::Transform& world_transform) {
    auto* collider = mutable_collider(handle);
    return collider && registry_.alive(collider->entity) &&
        refresh(*collider, collider->description, world_transform, registry_.enabled(collider->entity));
}

CollisionSyncResult CollisionWorld::sync(scene::Scene& scene_graph) {
    CollisionSyncResult result;
    if (&scene_graph.registry() != &registry_ || registry_.iterating()) {
        result.scene_busy = true;
        return result;
    }
    const auto updated = scene_graph.update();
    if (updated.error != scene::SceneError::None) {
        result.scene_busy = true;
        return result;
    }
    invalidate_contacts();
    for (auto& slot : slots_) {
        if (!slot) {
            continue;
        }
        auto& collider = *slot;
        const auto* component = registry_.get<ColliderComponent>(collider.entity);
        if (!component || std::find(component->colliders.begin(), component->colliders.end(),
                                    collider.handle) == component->colliders.end()) {
            result.removed += remove(collider.handle) ? 1U : 0U;
            continue;
        }
        const auto* matrix = scene_graph.cached_world_matrix(collider.entity);
        const auto world_transform = matrix ? math::decompose(*matrix) : std::nullopt;
        bool enabled = collider.description.enabled;
        for (auto ancestor = collider.entity; ancestor && enabled; ancestor = scene_graph.parent(ancestor)) {
            enabled = registry_.enabled(ancestor);
        }
        if (!world_transform || !refresh(collider, collider.description, *world_transform, enabled)) {
            deactivate(collider);
            ++result.rejected;
        } else if (collider.active) {
            ++result.updated;
        } else {
            ++result.disabled;
        }
    }
    return result;
}

void CollisionWorld::evaluate_pair(ColliderHandle first, ColliderHandle second) {
    ++statistics_.candidate_pairs;
    if (second < first) {
        std::swap(first, second);
    }
    const auto* a = get(first);
    const auto* b = get(second);
    if (!a || !b || !a->active || !b->active || !registry_.enabled(a->entity) ||
        !registry_.enabled(b->entity) || (a->entity == b->entity && !config_.collide_same_entity) ||
        !should_collide(a->description.filter, b->description.filter)) {
        ++statistics_.filtered_pairs;
        return;
    }
    ++statistics_.narrowphase_pairs;
    auto contact = narrowphase(a->world_shape, b->world_shape, config_.contact_tolerance);
    if (!contact.touching()) {
        return;
    }
    if (contacts_.size() == config_.max_manifolds) {
        statistics_.contacts_truncated = true;
        return;
    }
    contact.first = first;
    contact.second = second;
    contact.first_entity = a->entity;
    contact.second_entity = b->entity;
    contact.trigger = a->description.trigger || b->description.trigger;
    contact.friction = std::sqrt(a->description.material.friction * b->description.material.friction);
    contact.restitution = std::max(a->description.material.restitution, b->description.material.restitution);
    statistics_.contact_points += contact.point_count;
    contacts_.push_back(contact);
    statistics_.manifolds = contacts_.size();
}

CollisionStatistics CollisionWorld::detect() {
    invalidate_contacts();
    for (const auto& slot : slots_) {
        if (slot && slot->active && registry_.alive(slot->entity)) {
            ++statistics_.active;
            statistics_.planes += is_plane(slot->world_shape) ? 1U : 0U;
        }
    }
    statistics_.proxies = tree_.size();
    spatial::QueryFilter filter;
    filter.max_visits = config_.traversal_limit;
    const auto report = tree_.collect_pairs(pairs_, filter, config_.collide_same_entity);
    statistics_.pairs_truncated = report.truncated;
    statistics_.traversal_exhausted = report.traversal_exhausted;
    for (std::size_t pair = 0; pair < report.written; ++pair) {
        evaluate_pair(from_proxy(pairs_[pair].first), from_proxy(pairs_[pair].second));
    }
    for (const auto& plane : slots_) {
        if (!plane || !plane->active || !is_plane(plane->world_shape)) {
            continue;
        }
        for (const auto& finite : slots_) {
            if (!finite || !finite->active || is_plane(finite->world_shape)) {
                continue;
            }
            if (statistics_.candidate_pairs >= config_.max_pairs) {
                statistics_.pairs_truncated = true;
                return statistics_;
            }
            evaluate_pair(plane->handle, finite->handle);
        }
    }
    return statistics_;
}

std::span<const ContactManifold> CollisionWorld::contacts() const noexcept { return contacts_; }
const CollisionStatistics& CollisionWorld::statistics() const noexcept { return statistics_; }

CollisionQueryResult CollisionWorld::query(const math::Aabb& bounds,
                                           std::span<ColliderHandle> output,
                                           CollisionFilter filter, ecs::Entity exclude) const {
    CollisionQueryResult result;
    if (!math::valid(bounds) || !bounded(bounds.min) || !bounded(bounds.max)) {
        result.invalid_input = true;
        return result;
    }
    const auto accept = [&](const Collider& collider) {
        if (collider.active && registry_.alive(collider.entity) && registry_.enabled(collider.entity) &&
            collider.entity != exclude && should_collide(filter, collider.description.filter)) {
            ++result.matched;
            if (result.written < output.size()) {
                output[result.written++] = collider.handle;
            } else {
                result.truncated = true;
            }
        }
    };
    std::vector<spatial::QueryHit> hits(tree_.size());
    spatial::QueryFilter tree_filter;
    tree_filter.exclude = exclude;
    tree_filter.max_visits = config_.traversal_limit;
    const auto report = tree_.query(bounds, hits, tree_filter);
    result.truncated = report.truncated;
    for (std::size_t index = 0; index < report.written; ++index) {
        if (const auto* collider = get(from_proxy(hits[index].proxy))) {
            accept(*collider);
        }
    }
    for (const auto& slot : slots_) {
        if (slot && slot->active && is_plane(slot->world_shape)) {
            const auto plane = *math::normalized(std::get<math::Plane>(slot->world_shape));
            const double radius = math::dot(math::abs(plane.normal), math::half_extent(bounds));
            const double distance = math::dot(plane.normal, math::center(bounds)) + plane.distance;
            if (distance - radius <= config_.contact_tolerance) {
                accept(*slot);
            }
        }
    }
    return result;
}

namespace {

template<class T>
std::optional<T> parse_number(std::string_view text) noexcept {
    T result{};
    if (text.empty() || text.size() > 128) {
        return std::nullopt;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(result)) {
            return std::nullopt;
        }
    }
    return result;
}

std::optional<CollisionShape> parse_shape(core::CommandArgs args) {
    if (args.empty() || args.size() > 10) {
        return std::nullopt;
    }
    std::array<double, 9> values{};
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto number = parse_number<double>(args[index]);
        if (!number) {
            return std::nullopt;
        }
        values[index - 1] = *number;
    }
    std::optional<CollisionShape> shape;
    if (args[0] == "sphere" && args.size() == 2) {
        shape = math::Sphere{{}, values[0]};
    } else if (args[0] == "box" && args.size() == 4) {
        shape = math::Obb{{}, {values[0], values[1], values[2]}, {}};
    } else if (args[0] == "capsule" && args.size() == 3 && values[1] >= 0.0) {
        shape = math::Capsule{{0.0, -values[1], 0.0}, {0.0, values[1], 0.0}, values[0]};
    } else if (args[0] == "plane" && args.size() == 5) {
        shape = math::Plane{{values[0], values[1], values[2]}, values[3]};
    } else if (args[0] == "triangle" && args.size() == 10) {
        shape = math::Triangle{{values[0], values[1], values[2]}, {values[3], values[4], values[5]},
                              {values[6], values[7], values[8]}};
    }
    return shape && valid_shape(*shape) ? shape : std::nullopt;
}

ColliderHandle parse_collider(std::string_view text) noexcept {
    const auto parsed = ecs::parse_entity(text);
    return parsed ? ColliderHandle{parsed->index, parsed->generation} : ColliderHandle{};
}

std::string format_collider(ColliderHandle handle) {
    return std::to_string(handle.index) + ":" + std::to_string(handle.generation);
}

} // namespace

void register_collision(core::Kernel& kernel) {
    if (kernel.services().find<CollisionWorld>()) {
        return;
    }
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) {
        throw std::logic_error("collision requires the shared ECS Registry service");
    }
    CollisionConfig configuration;
    configuration.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        kernel.config().integer("collision.capacity", 16384), 0, 65536));
    configuration.max_pairs = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("collision.max_pairs", 65536), 0, 1048576));
    configuration.max_manifolds = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("collision.max_manifolds", 16384), 0, 262144));
    configuration.contact_tolerance = kernel.config().number("collision.tolerance", 1.0e-8);
    (void)kernel.services().emplace<CollisionWorld>(*registry, configuration);
    (void)kernel.register_module({"physics.collision", "Bounded primitive collision and contact manifolds", 1});
    (void)kernel.add_system({"physics.collision", core::Phase::PreSimulation, -8000, {}},
        [previous_rejected = std::size_t{}, previous_truncated = false]
        (core::Kernel& host, const core::Frame&) mutable {
            auto& world = host.services().get<CollisionWorld>();
            CollisionSyncResult synchronized;
            if (auto* scene_service = host.services().find<scene::Scene>()) {
                synchronized = world.sync(*scene_service);
            }
            const auto statistics = world.detect();
            const bool truncated = statistics.pairs_truncated || statistics.contacts_truncated;
            host.metric("collision.colliders", static_cast<double>(statistics.colliders));
            host.metric("collision.active", static_cast<double>(statistics.active));
            host.metric("collision.pairs", static_cast<double>(statistics.candidate_pairs));
            host.metric("collision.manifolds", static_cast<double>(statistics.manifolds));
            host.metric("collision.points", static_cast<double>(statistics.contact_points));
            host.metric("collision.rejected", static_cast<double>(synchronized.rejected));
            host.metric("collision.truncated", truncated ? 1.0 : 0.0);
            if ((synchronized.rejected != previous_rejected && synchronized.rejected != 0) ||
                (truncated && !previous_truncated)) {
                host.diagnose(core::Diagnostic::Severity::Warning, "collision",
                    std::to_string(synchronized.rejected) + " colliders excluded by transform or capacity limits; " +
                    (truncated ? "contact detection truncated" : "contact detection complete"));
            }
            previous_rejected = synchronized.rejected;
            previous_truncated = truncated;
        });
    (void)kernel.add_command("collider.create", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2) {
            return core::CommandResult{false, "usage: collider.create entity sphere|box|capsule|plane|triangle dimensions..."};
        }
        auto& world = host.services().get<CollisionWorld>();
        const auto shape = parse_shape(args.subspan(1));
        const auto parsed = ecs::parse_entity(args[0]);
        const auto named = parsed ? std::vector<ecs::Entity>{} : world.registry().find_all_named(args[0]);
        const auto entity = parsed ? *parsed : (named.size() == 1 ? named[0] : ecs::Entity{});
        if (!shape || !world.registry().alive(entity)) {
            return core::CommandResult{false, "shape is invalid or entity does not resolve uniquely"};
        }
        ColliderDescription description;
        description.shape = *shape;
        const auto handle = world.add(entity, description);
        if (auto* scene_service = host.services().find<scene::Scene>()) {
            (void)world.sync(*scene_service);
        }
        return core::CommandResult{static_cast<bool>(handle), handle ? format_collider(handle) : "collider admission failed"};
    }, "Attach to an existing entity: sphere radius; box halfX halfY halfZ; capsule radius halfLength; plane nx ny nz d; triangle 9 coordinates");
    (void)kernel.add_command("collider.assign", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2) {
            return core::CommandResult{false, "usage: collider.assign collider shape dimensions..."};
        }
        const auto shape = parse_shape(args.subspan(1));
        const bool changed = shape && host.services().get<CollisionWorld>().set_shape(parse_collider(args[0]), *shape);
        return core::CommandResult{changed, changed ? "shape assigned" : "invalid shape, stale handle, or incompatible transform"};
    }, "Replace a collider's local shape while preserving its handle and filtering");
    (void)kernel.add_command("collider.filter", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 3 || args.size() > 4) {
            return core::CommandResult{false, "usage: collider.filter collider layers mask [group]"};
        }
        const auto layers = parse_number<std::uint64_t>(args[1]);
        const auto mask = parse_number<std::uint64_t>(args[2]);
        const auto group = args.size() == 4 ? parse_number<std::int32_t>(args[3]) : std::optional<std::int32_t>{0};
        const bool changed = layers && mask && group && host.services().get<CollisionWorld>().set_filter(
            parse_collider(args[0]), {*layers, *mask, *group});
        return core::CommandResult{changed, changed ? "filter assigned" : "invalid filter or stale collider handle"};
    }, "Set decimal layer bits, acceptance mask, and optional signed collision group");
    (void)kernel.add_command("collider.trigger", [](core::Kernel& host, core::CommandArgs args) {
        const bool valid = args.size() == 2 && (args[1] == "true" || args[1] == "false");
        const bool changed = valid && host.services().get<CollisionWorld>().set_trigger(
            parse_collider(args[0]), args[1] == "true");
        return core::CommandResult{changed, changed ? "trigger mode assigned" : "usage: collider.trigger collider true|false"};
    }, "Mark contact manifolds as triggers for consumers to exclude from impulse solving");
    (void)kernel.add_command("collider.remove", [](core::Kernel& host, core::CommandArgs args) {
        const bool removed = args.size() == 1 && host.services().get<CollisionWorld>().remove(parse_collider(args[0]));
        return core::CommandResult{removed, removed ? "collider removed" : "usage: collider.remove live-collider"};
    }, "Remove a collider and invalidate its generation token");
    (void)kernel.add_command("collider.contact", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: collider.contact"};
        }
        auto& world = host.services().get<CollisionWorld>();
        if (auto* scene_service = host.services().find<scene::Scene>()) {
            (void)world.sync(*scene_service);
        }
        const auto statistics = world.detect();
        std::ostringstream output;
        output << statistics.manifolds << " manifolds, " << statistics.contact_points << " points";
        if (statistics.pairs_truncated || statistics.contacts_truncated) {
            output << " (truncated)";
        }
        const auto contacts = world.contacts();
        for (std::size_t index = 0; index < std::min<std::size_t>(contacts.size(), 16); ++index) {
            const auto& contact = contacts[index];
            output << '\n' << format_collider(contact.first) << " / " << format_collider(contact.second)
                << " normal=" << contact.normal.x << ',' << contact.normal.y << ',' << contact.normal.z
                << " points=" << contact.point_count << (contact.trigger ? " trigger" : " solid");
        }
        return core::CommandResult{true, output.str()};
    }, "Synchronize scene colliders, detect contacts, and list up to sixteen manifolds");
}

} // namespace ge::physics
