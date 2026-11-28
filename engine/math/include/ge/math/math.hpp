#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace ge::math {

inline constexpr double pi = 3.141592653589793238462643383279502884;
inline constexpr double tau = 2.0 * pi;
inline constexpr double epsilon = 1.0e-10;
inline constexpr double infinity = std::numeric_limits<double>::infinity();

[[nodiscard]] constexpr double radians(double degrees_value) noexcept {
    return degrees_value * (pi / 180.0);
}
[[nodiscard]] constexpr double degrees(double radians_value) noexcept {
    return radians_value * (180.0 / pi);
}
[[nodiscard]] constexpr double clamp(double value, double low, double high) noexcept {
    return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] constexpr double saturate(double value) noexcept {
    return clamp(value, 0.0, 1.0);
}
[[nodiscard]] constexpr double lerp(double a, double b, double t) noexcept {
    return a * (1.0 - t) + b * t;
}
[[nodiscard]] double inverse_lerp(double a, double b, double value) noexcept;
[[nodiscard]] double smoothstep(double low, double high, double value) noexcept;
[[nodiscard]] double smootherstep(double low, double high, double value) noexcept;
[[nodiscard]] double wrap(double value, double low, double high) noexcept;
[[nodiscard]] double wrap_angle(double angle) noexcept;
[[nodiscard]] double delta_angle(double from, double to) noexcept;
[[nodiscard]] double lerp_angle(double from, double to, double t) noexcept;
[[nodiscard]] double move_towards(double current, double target, double max_delta) noexcept;
[[nodiscard]] double damping_factor(double half_life, double delta_seconds) noexcept;
[[nodiscard]] bool almost_equal(double a, double b, double tolerance = epsilon) noexcept;

struct Vec2 {
    double x{};
    double y{};
    [[nodiscard]] constexpr double& operator[](std::size_t i) noexcept { return i == 0 ? x : y; }
    [[nodiscard]] constexpr double operator[](std::size_t i) const noexcept { return i == 0 ? x : y; }
    constexpr bool operator==(const Vec2&) const = default;
};

struct Vec3 {
    double x{};
    double y{};
    double z{};
    [[nodiscard]] constexpr double& operator[](std::size_t i) noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
    [[nodiscard]] constexpr double operator[](std::size_t i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
    constexpr bool operator==(const Vec3&) const = default;
};

struct Vec4 {
    double x{};
    double y{};
    double z{};
    double w{};
    [[nodiscard]] constexpr double& operator[](std::size_t i) noexcept {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    [[nodiscard]] constexpr double operator[](std::size_t i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    constexpr bool operator==(const Vec4&) const = default;
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x+b.x, a.y+b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x-b.x, a.y-b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a) noexcept { return {-a.x, -a.y}; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 a, double s) noexcept { return {a.x*s, a.y*s}; }
[[nodiscard]] constexpr Vec2 operator*(double s, Vec2 a) noexcept { return a*s; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 a, Vec2 b) noexcept { return {a.x*b.x, a.y*b.y}; }
[[nodiscard]] constexpr Vec2 operator/(Vec2 a, double s) noexcept { return {a.x/s, a.y/s}; }
[[nodiscard]] constexpr Vec2 operator/(Vec2 a, Vec2 b) noexcept { return {a.x/b.x, a.y/b.y}; }
constexpr Vec2& operator+=(Vec2& a, Vec2 b) noexcept { a = a+b; return a; }
constexpr Vec2& operator-=(Vec2& a, Vec2 b) noexcept { a = a-b; return a; }
constexpr Vec2& operator*=(Vec2& a, double s) noexcept { a = a*s; return a; }
constexpr Vec2& operator/=(Vec2& a, double s) noexcept { a = a/s; return a; }

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, double s) noexcept { return {a.x*s, a.y*s, a.z*s}; }
[[nodiscard]] constexpr Vec3 operator*(double s, Vec3 a) noexcept { return a*s; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, Vec3 b) noexcept { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
[[nodiscard]] constexpr Vec3 operator/(Vec3 a, double s) noexcept { return {a.x/s, a.y/s, a.z/s}; }
[[nodiscard]] constexpr Vec3 operator/(Vec3 a, Vec3 b) noexcept { return {a.x/b.x, a.y/b.y, a.z/b.z}; }
constexpr Vec3& operator+=(Vec3& a, Vec3 b) noexcept { a = a+b; return a; }
constexpr Vec3& operator-=(Vec3& a, Vec3 b) noexcept { a = a-b; return a; }
constexpr Vec3& operator*=(Vec3& a, double s) noexcept { a = a*s; return a; }
constexpr Vec3& operator/=(Vec3& a, double s) noexcept { a = a/s; return a; }

[[nodiscard]] constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
[[nodiscard]] constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
[[nodiscard]] constexpr Vec4 operator-(Vec4 a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
[[nodiscard]] constexpr Vec4 operator*(Vec4 a, double s) noexcept { return {a.x*s, a.y*s, a.z*s, a.w*s}; }
[[nodiscard]] constexpr Vec4 operator*(double s, Vec4 a) noexcept { return a*s; }
[[nodiscard]] constexpr Vec4 operator*(Vec4 a, Vec4 b) noexcept { return {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}; }
[[nodiscard]] constexpr Vec4 operator/(Vec4 a, double s) noexcept { return {a.x/s, a.y/s, a.z/s, a.w/s}; }
constexpr Vec4& operator+=(Vec4& a, Vec4 b) noexcept { a = a+b; return a; }
constexpr Vec4& operator-=(Vec4& a, Vec4 b) noexcept { a = a-b; return a; }
constexpr Vec4& operator*=(Vec4& a, double s) noexcept { a = a*s; return a; }
constexpr Vec4& operator/=(Vec4& a, double s) noexcept { a = a/s; return a; }

[[nodiscard]] constexpr double dot(Vec2 a, Vec2 b) noexcept { return a.x*b.x + a.y*b.y; }
[[nodiscard]] constexpr double dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
[[nodiscard]] constexpr double dot(Vec4 a, Vec4 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }
[[nodiscard]] constexpr double cross(Vec2 a, Vec2 b) noexcept { return a.x*b.y - a.y*b.x; }
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
[[nodiscard]] constexpr double length_squared(Vec2 v) noexcept { return dot(v,v); }
[[nodiscard]] constexpr double length_squared(Vec3 v) noexcept { return dot(v,v); }
[[nodiscard]] constexpr double length_squared(Vec4 v) noexcept { return dot(v,v); }
[[nodiscard]] constexpr Vec2 perpendicular(Vec2 v) noexcept { return {-v.y,v.x}; }
[[nodiscard]] constexpr Vec2 xy(Vec3 v) noexcept { return {v.x,v.y}; }
[[nodiscard]] constexpr Vec3 xyz(Vec4 v) noexcept { return {v.x,v.y,v.z}; }
[[nodiscard]] constexpr Vec4 homogeneous(Vec3 v, double w = 1.0) noexcept { return {v.x,v.y,v.z,w}; }
[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, double t) noexcept { return a*(1.0-t)+b*t; }
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, double t) noexcept { return a*(1.0-t)+b*t; }
[[nodiscard]] constexpr Vec4 lerp(Vec4 a, Vec4 b, double t) noexcept { return a*(1.0-t)+b*t; }
[[nodiscard]] double length(Vec2 v) noexcept;
[[nodiscard]] double length(Vec3 v) noexcept;
[[nodiscard]] double length(Vec4 v) noexcept;
[[nodiscard]] double distance(Vec2 a, Vec2 b) noexcept;
[[nodiscard]] double distance(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] double distance_squared(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] bool finite(Vec2 v) noexcept;
[[nodiscard]] bool finite(Vec3 v) noexcept;
[[nodiscard]] bool finite(Vec4 v) noexcept;
[[nodiscard]] bool almost_equal(Vec2 a, Vec2 b, double tolerance = epsilon) noexcept;
[[nodiscard]] bool almost_equal(Vec3 a, Vec3 b, double tolerance = epsilon) noexcept;
[[nodiscard]] bool almost_equal(Vec4 a, Vec4 b, double tolerance = epsilon) noexcept;
[[nodiscard]] Vec2 normalized(Vec2 v, Vec2 fallback = {}) noexcept;
[[nodiscard]] Vec3 normalized(Vec3 v, Vec3 fallback = {}) noexcept;
[[nodiscard]] Vec4 normalized(Vec4 v, Vec4 fallback = {}) noexcept;
[[nodiscard]] bool normalize(Vec2& v) noexcept;
[[nodiscard]] bool normalize(Vec3& v) noexcept;
[[nodiscard]] bool normalize(Vec4& v) noexcept;
[[nodiscard]] Vec2 min(Vec2 a, Vec2 b) noexcept;
[[nodiscard]] Vec3 min(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec4 min(Vec4 a, Vec4 b) noexcept;
[[nodiscard]] Vec2 max(Vec2 a, Vec2 b) noexcept;
[[nodiscard]] Vec3 max(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec4 max(Vec4 a, Vec4 b) noexcept;
[[nodiscard]] Vec2 abs(Vec2 v) noexcept;
[[nodiscard]] Vec3 abs(Vec3 v) noexcept;
[[nodiscard]] Vec4 abs(Vec4 v) noexcept;
[[nodiscard]] Vec2 clamp(Vec2 v, Vec2 low, Vec2 high) noexcept;
[[nodiscard]] Vec3 clamp(Vec3 v, Vec3 low, Vec3 high) noexcept;
[[nodiscard]] Vec4 clamp(Vec4 v, Vec4 low, Vec4 high) noexcept;
[[nodiscard]] Vec3 clamp_length(Vec3 v, double maximum) noexcept;
[[nodiscard]] Vec3 move_towards(Vec3 current, Vec3 target, double max_delta) noexcept;
[[nodiscard]] Vec3 project(Vec3 v, Vec3 onto) noexcept;
[[nodiscard]] Vec3 reject(Vec3 v, Vec3 from) noexcept;
[[nodiscard]] Vec3 reflect(Vec3 incident, Vec3 normal) noexcept;
[[nodiscard]] std::optional<Vec3> refract(Vec3 incident, Vec3 normal, double eta) noexcept;
[[nodiscard]] double angle_between(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] double signed_angle(Vec3 a, Vec3 b, Vec3 axis) noexcept;
[[nodiscard]] std::pair<Vec3, Vec3> orthonormal_basis(Vec3 normal) noexcept;

struct Quat {
    double x{};
    double y{};
    double z{};
    double w{1.0};
    constexpr bool operator==(const Quat&) const = default;
};

[[nodiscard]] constexpr Quat operator-(Quat q) noexcept { return {-q.x,-q.y,-q.z,-q.w}; }
[[nodiscard]] constexpr Quat operator+(Quat a, Quat b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w}; }
[[nodiscard]] constexpr Quat operator*(Quat q, double s) noexcept { return {q.x*s,q.y*s,q.z*s,q.w*s}; }
[[nodiscard]] constexpr Quat operator*(double s, Quat q) noexcept { return q*s; }
[[nodiscard]] constexpr double dot(Quat a, Quat b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept { return {-q.x,-q.y,-q.z,q.w}; }
[[nodiscard]] Quat operator*(Quat a, Quat b) noexcept;
[[nodiscard]] Vec3 operator*(Quat q, Vec3 v) noexcept;
[[nodiscard]] bool finite(Quat q) noexcept;
[[nodiscard]] double length(Quat q) noexcept;
[[nodiscard]] Quat normalized(Quat q, Quat fallback = {}) noexcept;
[[nodiscard]] std::optional<Quat> inverse(Quat q) noexcept;
[[nodiscard]] Vec3 rotate(Quat q, Vec3 v) noexcept;
[[nodiscard]] Quat from_axis_angle(Vec3 axis, double angle) noexcept;
[[nodiscard]] std::pair<Vec3, double> to_axis_angle(Quat q) noexcept;
[[nodiscard]] Quat from_euler_xyz(Vec3 angles) noexcept;
[[nodiscard]] Vec3 to_euler_xyz(Quat q) noexcept;
[[nodiscard]] Quat from_to(Vec3 from, Vec3 to) noexcept;
[[nodiscard]] Quat look_rotation(Vec3 forward, Vec3 up = {0.0,1.0,0.0}) noexcept;
[[nodiscard]] Quat nlerp(Quat a, Quat b, double t) noexcept;
[[nodiscard]] Quat slerp(Quat a, Quat b, double t) noexcept;
[[nodiscard]] double angular_distance(Quat a, Quat b) noexcept;
[[nodiscard]] Quat rotate_towards(Quat a, Quat b, double max_radians) noexcept;
[[nodiscard]] Quat integrate_angular_velocity(Quat orientation, Vec3 velocity, double dt) noexcept;

// Matrices multiply column vectors. Element (row, column) is data[column*N+row].
struct Mat3 {
    std::array<double,9> data{1.0,0.0,0.0, 0.0,1.0,0.0, 0.0,0.0,1.0};
    [[nodiscard]] constexpr double& operator()(std::size_t row, std::size_t column) noexcept {
        return data[column*3+row];
    }
    [[nodiscard]] constexpr double operator()(std::size_t row, std::size_t column) const noexcept {
        return data[column*3+row];
    }
    [[nodiscard]] static constexpr Mat3 identity() noexcept { return {}; }
    [[nodiscard]] static constexpr Mat3 zero() noexcept { return {{}}; }
    constexpr bool operator==(const Mat3&) const = default;
};

struct Mat4 {
    std::array<double,16> data{1.0,0.0,0.0,0.0, 0.0,1.0,0.0,0.0,
                               0.0,0.0,1.0,0.0, 0.0,0.0,0.0,1.0};
    [[nodiscard]] constexpr double& operator()(std::size_t row, std::size_t column) noexcept {
        return data[column*4+row];
    }
    [[nodiscard]] constexpr double operator()(std::size_t row, std::size_t column) const noexcept {
        return data[column*4+row];
    }
    [[nodiscard]] static constexpr Mat4 identity() noexcept { return {}; }
    [[nodiscard]] static constexpr Mat4 zero() noexcept { return {{}}; }
    constexpr bool operator==(const Mat4&) const = default;
};

[[nodiscard]] Mat3 operator+(const Mat3& a, const Mat3& b) noexcept;
[[nodiscard]] Mat3 operator-(const Mat3& a, const Mat3& b) noexcept;
[[nodiscard]] Mat3 operator*(const Mat3& a, const Mat3& b) noexcept;
[[nodiscard]] Mat3 operator*(const Mat3& a, double scalar) noexcept;
[[nodiscard]] Vec3 operator*(const Mat3& a, Vec3 v) noexcept;
[[nodiscard]] Mat4 operator+(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] Mat4 operator-(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] Mat4 operator*(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] Mat4 operator*(const Mat4& a, double scalar) noexcept;
[[nodiscard]] Vec4 operator*(const Mat4& a, Vec4 v) noexcept;
[[nodiscard]] bool finite(const Mat3& m) noexcept;
[[nodiscard]] bool finite(const Mat4& m) noexcept;
[[nodiscard]] bool almost_equal(const Mat3& a, const Mat3& b, double tolerance = epsilon) noexcept;
[[nodiscard]] bool almost_equal(const Mat4& a, const Mat4& b, double tolerance = epsilon) noexcept;
[[nodiscard]] Mat3 transpose(const Mat3& m) noexcept;
[[nodiscard]] Mat4 transpose(const Mat4& m) noexcept;
[[nodiscard]] double determinant(const Mat3& m) noexcept;
[[nodiscard]] double determinant(const Mat4& m) noexcept;
[[nodiscard]] std::optional<Mat3> inverse(const Mat3& m, double tolerance = epsilon) noexcept;
// Affine inversion checks the linear part independently of translation size.
[[nodiscard]] std::optional<Mat4> inverse(const Mat4& m, double tolerance = epsilon) noexcept;
[[nodiscard]] Mat3 upper3x3(const Mat4& m) noexcept;
[[nodiscard]] Mat4 embed(const Mat3& m) noexcept;
[[nodiscard]] Mat3 outer_product(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Mat3 skew_symmetric(Vec3 v) noexcept;
[[nodiscard]] Mat3 rotation_matrix(Quat q) noexcept;
[[nodiscard]] Quat quaternion(const Mat3& m) noexcept;
[[nodiscard]] Mat4 translation_matrix(Vec3 v) noexcept;
[[nodiscard]] Mat4 scaling_matrix(Vec3 v) noexcept;
[[nodiscard]] Mat4 trs_matrix(Vec3 position, Quat rotation, Vec3 scale) noexcept;
[[nodiscard]] Vec3 transform_point(const Mat4& m, Vec3 p) noexcept;
[[nodiscard]] Vec3 transform_vector(const Mat4& m, Vec3 v) noexcept;
[[nodiscard]] std::optional<Vec3> project_point(const Mat4& m, Vec3 p) noexcept;
[[nodiscard]] std::optional<Vec3> transform_normal(const Mat4& m, Vec3 normal) noexcept;
[[nodiscard]] std::optional<Mat3> normal_matrix(const Mat4& m) noexcept;
// Projection constructors throw invalid_argument for invalid finite ranges.
[[nodiscard]] Mat4 perspective(double vertical_fov, double aspect, double near_plane, double far_plane);
[[nodiscard]] Mat4 perspective_zero_to_one(double vertical_fov, double aspect, double near_plane, double far_plane);
[[nodiscard]] Mat4 perspective_reverse_z(double vertical_fov, double aspect, double near_plane,
                                       double far_plane = infinity);
[[nodiscard]] Mat4 orthographic(double left, double right, double bottom, double top,
                                double near_plane, double far_plane);
[[nodiscard]] Mat4 orthographic_zero_to_one(double left, double right, double bottom, double top,
                                            double near_plane, double far_plane);
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = {0.0,1.0,0.0});

struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.0,1.0,1.0};
};

[[nodiscard]] bool finite(const Transform& transform) noexcept;
[[nodiscard]] Mat4 matrix(const Transform& transform) noexcept;
[[nodiscard]] Vec3 transform_point(const Transform& transform, Vec3 p) noexcept;
[[nodiscard]] Vec3 transform_vector(const Transform& transform, Vec3 v) noexcept;
[[nodiscard]] std::optional<Vec3> inverse_transform_point(const Transform& transform, Vec3 p) noexcept;
[[nodiscard]] std::optional<Vec3> inverse_transform_vector(const Transform& transform, Vec3 v) noexcept;
[[nodiscard]] Transform interpolate(const Transform& a, const Transform& b, double t) noexcept;
[[nodiscard]] Mat4 compose(const Transform& parent, const Transform& local) noexcept;
// Rejects perspective, zero scale and shear rather than silently losing them.
[[nodiscard]] std::optional<Transform> decompose(const Mat4& m, double tolerance = 1.0e-8) noexcept;

struct Ray {
    Vec3 origin{};
    Vec3 direction{0.0,0.0,-1.0};
    [[nodiscard]] constexpr Vec3 point(double t) const noexcept { return origin+direction*t; }
};

struct Plane {
    Vec3 normal{0.0,1.0,0.0};
    double distance{};
};

struct Aabb {
    Vec3 min{infinity,infinity,infinity};
    Vec3 max{-infinity,-infinity,-infinity};
};

struct Sphere {
    Vec3 center{};
    double radius{};
};

struct Segment {
    Vec3 a{};
    Vec3 b{};
};

struct Capsule {
    Vec3 a{};
    Vec3 b{0.0,1.0,0.0};
    double radius{0.5};
};

struct Triangle {
    Vec3 a{};
    Vec3 b{};
    Vec3 c{};
};

struct Obb {
    Vec3 center{};
    Vec3 half_extents{0.5,0.5,0.5};
    Quat orientation{};
};

// Ray hit distance is the ray parameter, in metres only for unit directions.
struct RayHit {
    double distance{};
    Vec3 point{};
    Vec3 normal{};
    Vec3 barycentric{};
    bool inside{};
};

struct RayInterval {
    double enter{};
    double exit{};
};

[[nodiscard]] bool valid(const Ray& ray) noexcept;
[[nodiscard]] bool valid(const Plane& plane) noexcept;
[[nodiscard]] bool valid(const Aabb& box) noexcept;
[[nodiscard]] bool valid(const Sphere& sphere) noexcept;
[[nodiscard]] bool valid(const Capsule& capsule) noexcept;
[[nodiscard]] bool valid(const Obb& box) noexcept;
[[nodiscard]] std::optional<Plane> normalized(const Plane& plane) noexcept;
[[nodiscard]] std::optional<Plane> plane_from_points(Vec3 a, Vec3 b, Vec3 c) noexcept;
[[nodiscard]] double signed_distance(const Plane& plane, Vec3 p) noexcept;
[[nodiscard]] Vec3 closest_point(const Plane& plane, Vec3 p) noexcept;
[[nodiscard]] std::optional<Plane> transform_plane(const Mat4& transform, const Plane& plane) noexcept;
[[nodiscard]] Vec3 center(const Aabb& box) noexcept;
[[nodiscard]] Vec3 extent(const Aabb& box) noexcept;
[[nodiscard]] Vec3 half_extent(const Aabb& box) noexcept;
[[nodiscard]] double surface_area(const Aabb& box) noexcept;
[[nodiscard]] double volume(const Aabb& box) noexcept;
[[nodiscard]] Aabb bounds(std::span<const Vec3> points) noexcept;
[[nodiscard]] Aabb bounds(const Sphere& sphere) noexcept;
[[nodiscard]] Aabb bounds(const Capsule& capsule) noexcept;
[[nodiscard]] Aabb bounds(const Triangle& triangle) noexcept;
[[nodiscard]] Aabb bounds(const Obb& box) noexcept;
[[nodiscard]] Aabb merge(const Aabb& a, const Aabb& b) noexcept;
[[nodiscard]] Aabb include(const Aabb& box, Vec3 p) noexcept;
[[nodiscard]] Aabb expanded(const Aabb& box, double margin) noexcept;
[[nodiscard]] Aabb translated(const Aabb& box, Vec3 offset) noexcept;
[[nodiscard]] Aabb transform_bounds(const Mat4& transform, const Aabb& box) noexcept;
[[nodiscard]] Sphere merge(const Sphere& a, const Sphere& b) noexcept;
[[nodiscard]] Sphere bounding_sphere(std::span<const Vec3> points) noexcept;
// Conservative under affine scale and shear; the resulting sphere need not be minimal.
[[nodiscard]] Sphere transform_sphere(const Mat4& transform, const Sphere& sphere) noexcept;
[[nodiscard]] bool contains(const Aabb& box, Vec3 p) noexcept;
[[nodiscard]] bool contains(const Aabb& outer, const Aabb& inner) noexcept;
[[nodiscard]] bool contains(const Sphere& sphere, Vec3 p) noexcept;
[[nodiscard]] bool overlaps(const Aabb& a, const Aabb& b) noexcept;
[[nodiscard]] bool overlaps(const Sphere& a, const Sphere& b) noexcept;
[[nodiscard]] bool overlaps(const Aabb& box, const Sphere& sphere) noexcept;
[[nodiscard]] bool overlaps(const Capsule& capsule, const Sphere& sphere) noexcept;
[[nodiscard]] bool overlaps(const Capsule& a, const Capsule& b) noexcept;
[[nodiscard]] bool overlaps(const Obb& a, const Obb& b, double tolerance = 1.0e-8) noexcept;
[[nodiscard]] Vec3 closest_point(const Aabb& box, Vec3 p) noexcept;
[[nodiscard]] Vec3 closest_point(const Segment& segment, Vec3 p) noexcept;
// Collapsed or nearly collinear triangles fall back to their edges. The
// collinearity tolerance is relative to edge lengths, preserving small faces.
[[nodiscard]] Vec3 closest_point(const Triangle& triangle, Vec3 p) noexcept;
[[nodiscard]] Vec3 closest_point(const Obb& box, Vec3 p) noexcept;
[[nodiscard]] double distance_squared(const Aabb& box, Vec3 p) noexcept;
[[nodiscard]] double distance_squared(const Segment& segment, Vec3 p) noexcept;
[[nodiscard]] double distance_squared(const Segment& a, const Segment& b) noexcept;
[[nodiscard]] std::pair<Vec3,Vec3> closest_points(const Segment& a, const Segment& b) noexcept;
[[nodiscard]] std::optional<Vec3> barycentric(const Triangle& triangle, Vec3 p) noexcept;
[[nodiscard]] Vec3 triangle_normal(const Triangle& triangle) noexcept;
[[nodiscard]] double triangle_area(const Triangle& triangle) noexcept;
[[nodiscard]] std::optional<RayInterval> ray_interval(const Ray& ray, const Aabb& box,
                                                    double minimum = 0.0, double maximum = infinity) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Plane& plane,
                                            double minimum = 0.0, double maximum = infinity) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Sphere& sphere,
                                            double minimum = 0.0, double maximum = infinity) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Aabb& box,
                                            double minimum = 0.0, double maximum = infinity) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Triangle& triangle,
                                            double minimum = 0.0, double maximum = infinity,
                                            bool cull_backface = false) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Obb& box,
                                            double minimum = 0.0, double maximum = infinity) noexcept;
[[nodiscard]] std::optional<RayHit> intersect(const Ray& ray, const Capsule& capsule,
                                            double minimum = 0.0, double maximum = infinity) noexcept;

enum class PlaneSide : std::uint8_t { negative, intersecting, positive };
[[nodiscard]] PlaneSide classify(const Plane& plane, const Aabb& box) noexcept;
[[nodiscard]] PlaneSide classify(const Plane& plane, const Sphere& sphere) noexcept;

enum class ClipDepth : std::uint8_t { negative_one_to_one, zero_to_one };
struct Frustum {
    std::array<Plane,6> planes{};
    std::array<Vec3,8> corners{};
};

[[nodiscard]] std::optional<Frustum> extract_frustum(const Mat4& view_projection,
                                                   ClipDepth depth = ClipDepth::negative_one_to_one) noexcept;
[[nodiscard]] bool intersects(const Frustum& frustum, const Aabb& box) noexcept;
[[nodiscard]] bool intersects(const Frustum& frustum, const Sphere& sphere) noexcept;
[[nodiscard]] bool contains(const Frustum& frustum, Vec3 p) noexcept;
// Viewport is (x,y,width,height), with a lower-left origin and window depth in [0,1].
[[nodiscard]] std::optional<Vec3> unproject(Vec3 window, const Mat4& inverse_view_projection,
                                         Vec4 viewport, ClipDepth depth = ClipDepth::negative_one_to_one) noexcept;
[[nodiscard]] std::optional<Ray> picking_ray(Vec2 pixel, const Mat4& inverse_view_projection,
                                            Vec4 viewport, ClipDepth depth = ClipDepth::negative_one_to_one) noexcept;

[[nodiscard]] Vec3 cubic_bezier(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept;
[[nodiscard]] Vec3 cubic_bezier_tangent(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept;
[[nodiscard]] Vec3 catmull_rom(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept;
[[nodiscard]] Vec3 cubic_hermite(Vec3 a, Vec3 tangent_a, Vec3 b, Vec3 tangent_b, double t) noexcept;
[[nodiscard]] double approximate_bezier_length(Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                                              std::uint32_t segments = 32) noexcept;
[[nodiscard]] double halton(std::uint64_t index, std::uint32_t base) noexcept;
[[nodiscard]] Vec2 concentric_disk(Vec2 uniform_sample) noexcept;
[[nodiscard]] Vec3 uniform_sphere(Vec2 uniform_sample) noexcept;
[[nodiscard]] Vec3 cosine_hemisphere(Vec2 uniform_sample) noexcept;
[[nodiscard]] Vec3 spherical_to_cartesian(double radius, double azimuth, double elevation) noexcept;
[[nodiscard]] Vec3 cartesian_to_spherical(Vec3 position) noexcept;
[[nodiscard]] Vec2 octahedral_encode(Vec3 normal) noexcept;
[[nodiscard]] Vec3 octahedral_decode(Vec2 encoded) noexcept;

} // namespace ge::math
