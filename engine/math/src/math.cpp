#include <ge/math/math.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ge::math {
namespace {

bool valid_interval(double minimum, double maximum) noexcept {
    return std::isfinite(minimum) && !std::isnan(maximum) && maximum >= minimum;
}

bool valid_tolerance(double tolerance) noexcept {
    return std::isfinite(tolerance) && tolerance >= 0.0;
}

double sign_nonzero(double value) noexcept {
    return value < 0.0 ? -1.0 : 1.0;
}

Vec3 column(const Mat3& m, std::size_t index) noexcept {
    return {m(0,index),m(1,index),m(2,index)};
}

Vec4 row(const Mat4& m, std::size_t index) noexcept {
    return {m(index,0),m(index,1),m(index,2),m(index,3)};
}

bool valid_projection(double fov, double aspect, double near_plane, double far_plane) noexcept {
    return std::isfinite(fov) && fov > 0.0 && fov < pi &&
           std::isfinite(aspect) && aspect > 0.0 &&
           std::isfinite(near_plane) && near_plane > 0.0 &&
           std::isfinite(far_plane) && far_plane > near_plane;
}

bool valid_orthographic(double left, double right, double bottom, double top,
                        double near_plane, double far_plane) noexcept {
    return std::isfinite(left) && std::isfinite(right) && right > left &&
           std::isfinite(bottom) && std::isfinite(top) && top > bottom &&
           std::isfinite(near_plane) && std::isfinite(far_plane) && far_plane > near_plane;
}

} // namespace

double inverse_lerp(double a, double b, double value) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(value) || a == b) {
        return 0.0;
    }
    return (value-a)/(b-a);
}

double smoothstep(double low, double high, double value) noexcept {
    const double t = saturate(inverse_lerp(low,high,value));
    return t*t*(3.0-2.0*t);
}

double smootherstep(double low, double high, double value) noexcept {
    const double t = saturate(inverse_lerp(low,high,value));
    return t*t*t*(t*(t*6.0-15.0)+10.0);
}

double wrap(double value, double low, double high) noexcept {
    const double range = high-low;
    if (!std::isfinite(value) || !std::isfinite(low) || !std::isfinite(range) || range <= 0.0) {
        return low;
    }
    double result = std::fmod(value-low,range);
    if (result < 0.0) {
        result += range;
    }
    return result+low;
}

double wrap_angle(double angle) noexcept {
    return wrap(angle,-pi,pi);
}

double delta_angle(double from, double to) noexcept {
    return wrap_angle(to-from);
}

double lerp_angle(double from, double to, double t) noexcept {
    return from+delta_angle(from,to)*t;
}

double move_towards(double current, double target, double max_delta) noexcept {
    if (!std::isfinite(current) || !std::isfinite(target) ||
        !std::isfinite(max_delta) || max_delta < 0.0) {
        return current;
    }
    const double delta = target-current;
    if (std::abs(delta) <= max_delta) {
        return target;
    }
    return current+std::copysign(max_delta,delta);
}

double damping_factor(double half_life, double delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0) {
        return 0.0;
    }
    if (half_life <= 0.0) {
        return 1.0;
    }
    if (!std::isfinite(half_life)) {
        return 0.0;
    }
    return -std::expm1(-std::log(2.0)*delta_seconds/half_life);
}

bool almost_equal(double a, double b, double tolerance) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b) || !valid_tolerance(tolerance)) {
        return false;
    }
    return std::abs(a-b) <= tolerance*std::max({1.0,std::abs(a),std::abs(b)});
}

double length(Vec2 v) noexcept {
    return std::hypot(v.x,v.y);
}

double length(Vec3 v) noexcept {
    return std::hypot(v.x,v.y,v.z);
}

double length(Vec4 v) noexcept {
    return std::hypot(std::hypot(v.x,v.y),std::hypot(v.z,v.w));
}

double distance(Vec2 a, Vec2 b) noexcept {
    return length(a-b);
}

double distance(Vec3 a, Vec3 b) noexcept {
    return length(a-b);
}

double distance_squared(Vec3 a, Vec3 b) noexcept {
    return length_squared(a-b);
}

bool finite(Vec2 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

bool finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(Vec4 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}

bool almost_equal(Vec2 a, Vec2 b, double tolerance) noexcept {
    return almost_equal(a.x,b.x,tolerance) && almost_equal(a.y,b.y,tolerance);
}

bool almost_equal(Vec3 a, Vec3 b, double tolerance) noexcept {
    return almost_equal(a.x,b.x,tolerance) && almost_equal(a.y,b.y,tolerance) &&
           almost_equal(a.z,b.z,tolerance);
}

bool almost_equal(Vec4 a, Vec4 b, double tolerance) noexcept {
    return almost_equal(a.x,b.x,tolerance) && almost_equal(a.y,b.y,tolerance) &&
           almost_equal(a.z,b.z,tolerance) && almost_equal(a.w,b.w,tolerance);
}

Vec2 normalized(Vec2 v, Vec2 fallback) noexcept {
    const double magnitude = std::max(std::abs(v.x),std::abs(v.y));
    if (!finite(v) || magnitude == 0.0) {
        return fallback;
    }
    const Vec2 scaled = v/magnitude;
    return scaled/length(scaled);
}

Vec3 normalized(Vec3 v, Vec3 fallback) noexcept {
    const double magnitude = std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)});
    if (!finite(v) || magnitude == 0.0) {
        return fallback;
    }
    const Vec3 scaled = v/magnitude;
    return scaled/length(scaled);
}

Vec4 normalized(Vec4 v, Vec4 fallback) noexcept {
    const double magnitude = std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z),std::abs(v.w)});
    if (!finite(v) || magnitude == 0.0) {
        return fallback;
    }
    const Vec4 scaled = v/magnitude;
    return scaled/length(scaled);
}

bool normalize(Vec2& v) noexcept {
    if (!finite(v) || (v.x == 0.0 && v.y == 0.0)) {
        return false;
    }
    v = normalized(v);
    return true;
}

bool normalize(Vec3& v) noexcept {
    if (!finite(v) || (v.x == 0.0 && v.y == 0.0 && v.z == 0.0)) {
        return false;
    }
    v = normalized(v);
    return true;
}

bool normalize(Vec4& v) noexcept {
    if (!finite(v) || (v.x == 0.0 && v.y == 0.0 && v.z == 0.0 && v.w == 0.0)) {
        return false;
    }
    v = normalized(v);
    return true;
}

Vec2 min(Vec2 a, Vec2 b) noexcept {
    return {std::min(a.x,b.x),std::min(a.y,b.y)};
}

Vec3 min(Vec3 a, Vec3 b) noexcept {
    return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)};
}

Vec4 min(Vec4 a, Vec4 b) noexcept {
    return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z),std::min(a.w,b.w)};
}

Vec2 max(Vec2 a, Vec2 b) noexcept {
    return {std::max(a.x,b.x),std::max(a.y,b.y)};
}

Vec3 max(Vec3 a, Vec3 b) noexcept {
    return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};
}

Vec4 max(Vec4 a, Vec4 b) noexcept {
    return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z),std::max(a.w,b.w)};
}

Vec2 abs(Vec2 v) noexcept {
    return {std::abs(v.x),std::abs(v.y)};
}

Vec3 abs(Vec3 v) noexcept {
    return {std::abs(v.x),std::abs(v.y),std::abs(v.z)};
}

Vec4 abs(Vec4 v) noexcept {
    return {std::abs(v.x),std::abs(v.y),std::abs(v.z),std::abs(v.w)};
}

Vec2 clamp(Vec2 v, Vec2 low, Vec2 high) noexcept {
    return {clamp(v.x,low.x,high.x),clamp(v.y,low.y,high.y)};
}

Vec3 clamp(Vec3 v, Vec3 low, Vec3 high) noexcept {
    return {clamp(v.x,low.x,high.x),clamp(v.y,low.y,high.y),clamp(v.z,low.z,high.z)};
}

Vec4 clamp(Vec4 v, Vec4 low, Vec4 high) noexcept {
    return {clamp(v.x,low.x,high.x),clamp(v.y,low.y,high.y),
            clamp(v.z,low.z,high.z),clamp(v.w,low.w,high.w)};
}

Vec3 clamp_length(Vec3 v, double maximum) noexcept {
    if (!finite(v) || !std::isfinite(maximum) || maximum <= 0.0) {
        return {};
    }
    return length(v) > maximum ? normalized(v)*maximum : v;
}

Vec3 move_towards(Vec3 current, Vec3 target, double max_delta) noexcept {
    if (!finite(current) || !finite(target) || !std::isfinite(max_delta) || max_delta < 0.0) {
        return current;
    }
    const Vec3 delta = target-current;
    const double magnitude = length(delta);
    if (magnitude <= max_delta || magnitude == 0.0) {
        return target;
    }
    return current+normalized(delta)*max_delta;
}

Vec3 project(Vec3 v, Vec3 onto) noexcept {
    const Vec3 direction = normalized(onto);
    return direction*dot(v,direction);
}

Vec3 reject(Vec3 v, Vec3 from) noexcept {
    return v-project(v,from);
}

Vec3 reflect(Vec3 incident, Vec3 normal) noexcept {
    const Vec3 n = normalized(normal);
    return incident-n*(2.0*dot(incident,n));
}

std::optional<Vec3> refract(Vec3 incident, Vec3 normal, double eta) noexcept {
    if (!normalize(incident) || !normalize(normal) || !std::isfinite(eta) || eta <= 0.0) {
        return std::nullopt;
    }
    const double cosine = clamp(dot(normal,incident),-1.0,1.0);
    const double discriminant = 1.0-eta*eta*(1.0-cosine*cosine);
    if (discriminant < 0.0) {
        return std::nullopt;
    }
    return incident*eta-normal*(eta*cosine+std::sqrt(discriminant));
}

double angle_between(Vec3 a, Vec3 b) noexcept {
    if (!normalize(a) || !normalize(b)) {
        return 0.0;
    }
    return std::atan2(length(cross(a,b)),clamp(dot(a,b),-1.0,1.0));
}

double signed_angle(Vec3 a, Vec3 b, Vec3 axis) noexcept {
    if (!normalize(axis)) {
        return 0.0;
    }
    a = reject(a,axis);
    b = reject(b,axis);
    if (!normalize(a) || !normalize(b)) {
        return 0.0;
    }
    return std::atan2(dot(axis,cross(a,b)),clamp(dot(a,b),-1.0,1.0));
}

std::pair<Vec3,Vec3> orthonormal_basis(Vec3 normal) noexcept {
    normal = normalized(normal,{0.0,1.0,0.0});
    const Vec3 axis = std::abs(normal.y) < 0.9 ? Vec3{0.0,1.0,0.0} : Vec3{1.0,0.0,0.0};
    const Vec3 tangent = normalized(cross(axis,normal));
    return {tangent,cross(normal,tangent)};
}

Quat operator*(Quat a, Quat b) noexcept {
    return {
        a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z
    };
}

Vec3 operator*(Quat q, Vec3 v) noexcept {
    return rotate(q,v);
}

bool finite(Quat q) noexcept {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

double length(Quat q) noexcept {
    return length(Vec4{q.x,q.y,q.z,q.w});
}

Quat normalized(Quat q, Quat fallback) noexcept {
    if (!finite(q) || (q.x == 0.0 && q.y == 0.0 && q.z == 0.0 && q.w == 0.0)) {
        return fallback;
    }
    const Vec4 unit = normalized(Vec4{q.x,q.y,q.z,q.w});
    return {unit.x,unit.y,unit.z,unit.w};
}

std::optional<Quat> inverse(Quat q) noexcept {
    const double magnitude = length(q);
    if (!finite(q) || !std::isfinite(magnitude) || magnitude <= 0.0) {
        return std::nullopt;
    }
    const Quat result = conjugate(normalized(q))*(1.0/magnitude);
    return finite(result) ? std::optional<Quat>{result} : std::nullopt;
}

Vec3 rotate(Quat q, Vec3 v) noexcept {
    q = normalized(q);
    const Vec3 imaginary{q.x,q.y,q.z};
    const Vec3 twice_cross = cross(imaginary,v)*2.0;
    return v+twice_cross*q.w+cross(imaginary,twice_cross);
}

Quat from_axis_angle(Vec3 axis, double angle) noexcept {
    if (!normalize(axis) || !std::isfinite(angle)) {
        return {};
    }
    const double half = angle*0.5;
    const double sine = std::sin(half);
    return {axis.x*sine,axis.y*sine,axis.z*sine,std::cos(half)};
}

std::pair<Vec3,double> to_axis_angle(Quat q) noexcept {
    q = normalized(q);
    if (q.w < 0.0) {
        q = -q;
    }
    const double sine = length(Vec3{q.x,q.y,q.z});
    if (sine <= epsilon) {
        return {{1.0,0.0,0.0},0.0};
    }
    return {Vec3{q.x,q.y,q.z}/sine,2.0*std::atan2(sine,q.w)};
}

Quat from_euler_xyz(Vec3 angles) noexcept {
    if (!finite(angles)) {
        return {};
    }
    const Quat x = from_axis_angle({1.0,0.0,0.0},angles.x);
    const Quat y = from_axis_angle({0.0,1.0,0.0},angles.y);
    const Quat z = from_axis_angle({0.0,0.0,1.0},angles.z);
    return normalized(z*y*x);
}

Vec3 to_euler_xyz(Quat q) noexcept {
    const Mat3 rotation = rotation_matrix(q);
    const double y = std::asin(clamp(-rotation(2,0),-1.0,1.0));
    if (std::abs(std::cos(y)) > 1.0e-8) {
        return {std::atan2(rotation(2,1),rotation(2,2)),y,
                std::atan2(rotation(1,0),rotation(0,0))};
    }
    return {std::atan2(-rotation(1,2),rotation(1,1)),y,0.0};
}

Quat from_to(Vec3 from, Vec3 to) noexcept {
    if (!normalize(from) || !normalize(to)) {
        return {};
    }
    const double cosine = clamp(dot(from,to),-1.0,1.0);
    if (cosine >= 1.0-epsilon) {
        return {};
    }
    if (cosine <= -1.0+epsilon) {
        return from_axis_angle(orthonormal_basis(from).first,pi);
    }
    const Vec3 axis = cross(from,to);
    return normalized(Quat{axis.x,axis.y,axis.z,1.0+cosine});
}

Quat look_rotation(Vec3 forward, Vec3 up) noexcept {
    if (!normalize(forward)) {
        return {};
    }
    const Vec3 backward = -forward;
    Vec3 right = cross(up,backward);
    if (!normalize(right)) {
        right = orthonormal_basis(backward).first;
    }
    const Vec3 corrected_up = cross(backward,right);
    Mat3 basis;
    for (std::size_t i = 0; i < 3; ++i) {
        basis(i,0) = right[i];
        basis(i,1) = corrected_up[i];
        basis(i,2) = backward[i];
    }
    return quaternion(basis);
}

Quat nlerp(Quat a, Quat b, double t) noexcept {
    a = normalized(a);
    b = normalized(b);
    if (!std::isfinite(t)) {
        return a;
    }
    if (dot(a,b) < 0.0) {
        b = -b;
    }
    return normalized(a*(1.0-t)+b*t,a);
}

Quat slerp(Quat a, Quat b, double t) noexcept {
    a = normalized(a);
    b = normalized(b);
    if (!std::isfinite(t)) {
        return a;
    }
    double cosine = dot(a,b);
    if (cosine < 0.0) {
        b = -b;
        cosine = -cosine;
    }
    cosine = clamp(cosine,0.0,1.0);
    if (cosine > 0.9995) {
        return nlerp(a,b,t);
    }
    const double theta = std::acos(cosine);
    const double sine = std::sin(theta);
    return normalized(a*(std::sin((1.0-t)*theta)/sine)+b*(std::sin(t*theta)/sine));
}

double angular_distance(Quat a, Quat b) noexcept {
    return 2.0*std::acos(clamp(std::abs(dot(normalized(a),normalized(b))),0.0,1.0));
}

Quat rotate_towards(Quat a, Quat b, double max_radians) noexcept {
    if (!std::isfinite(max_radians) || max_radians <= 0.0) {
        return normalized(a);
    }
    const double angle = angular_distance(a,b);
    return angle <= max_radians ? normalized(b) : slerp(a,b,max_radians/angle);
}

Quat integrate_angular_velocity(Quat orientation, Vec3 velocity, double dt) noexcept {
    if (!finite(velocity) || !std::isfinite(dt)) {
        return normalized(orientation);
    }
    const double speed = length(velocity);
    if (speed <= epsilon || dt == 0.0) {
        return normalized(orientation);
    }
    return normalized(from_axis_angle(velocity,speed*dt)*normalized(orientation));
}

Mat3 operator+(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]+b.data[i];
    }
    return result;
}

Mat3 operator-(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]-b.data[i];
    }
    return result;
}

Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result = Mat3::zero();
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t k = 0; k < 3; ++k) {
                result(r,c) += a(r,k)*b(k,c);
            }
        }
    }
    return result;
}

Mat3 operator*(const Mat3& a, double scalar) noexcept {
    Mat3 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]*scalar;
    }
    return result;
}

Vec3 operator*(const Mat3& a, Vec3 v) noexcept {
    return {a(0,0)*v.x+a(0,1)*v.y+a(0,2)*v.z,
            a(1,0)*v.x+a(1,1)*v.y+a(1,2)*v.z,
            a(2,0)*v.x+a(2,1)*v.y+a(2,2)*v.z};
}

Mat4 operator+(const Mat4& a, const Mat4& b) noexcept {
    Mat4 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]+b.data[i];
    }
    return result;
}

Mat4 operator-(const Mat4& a, const Mat4& b) noexcept {
    Mat4 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]-b.data[i];
    }
    return result;
}

Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 result = Mat4::zero();
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t k = 0; k < 4; ++k) {
                result(r,c) += a(r,k)*b(k,c);
            }
        }
    }
    return result;
}

Mat4 operator*(const Mat4& a, double scalar) noexcept {
    Mat4 result;
    for (std::size_t i = 0; i < result.data.size(); ++i) {
        result.data[i] = a.data[i]*scalar;
    }
    return result;
}

Vec4 operator*(const Mat4& a, Vec4 v) noexcept {
    return {dot(row(a,0),v),dot(row(a,1),v),dot(row(a,2),v),dot(row(a,3),v)};
}

bool finite(const Mat3& m) noexcept {
    return std::all_of(m.data.begin(),m.data.end(),[](double v) { return std::isfinite(v); });
}

bool finite(const Mat4& m) noexcept {
    return std::all_of(m.data.begin(),m.data.end(),[](double v) { return std::isfinite(v); });
}

bool almost_equal(const Mat3& a, const Mat3& b, double tolerance) noexcept {
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        if (!almost_equal(a.data[i],b.data[i],tolerance)) {
            return false;
        }
    }
    return true;
}

bool almost_equal(const Mat4& a, const Mat4& b, double tolerance) noexcept {
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        if (!almost_equal(a.data[i],b.data[i],tolerance)) {
            return false;
        }
    }
    return true;
}

Mat3 transpose(const Mat3& m) noexcept {
    Mat3 result;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            result(r,c) = m(c,r);
        }
    }
    return result;
}

Mat4 transpose(const Mat4& m) noexcept {
    Mat4 result;
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            result(r,c) = m(c,r);
        }
    }
    return result;
}

double determinant(const Mat3& m) noexcept {
    return m(0,0)*(m(1,1)*m(2,2)-m(1,2)*m(2,1))-
           m(0,1)*(m(1,0)*m(2,2)-m(1,2)*m(2,0))+
           m(0,2)*(m(1,0)*m(2,1)-m(1,1)*m(2,0));
}

double determinant(const Mat4& m) noexcept {
    double result = 0.0;
    for (std::size_t selected = 0; selected < 4; ++selected) {
        Mat3 minor = Mat3::zero();
        std::size_t target_column = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            if (c == selected) {
                continue;
            }
            for (std::size_t r = 1; r < 4; ++r) {
                minor(r-1,target_column) = m(r,c);
            }
            ++target_column;
        }
        const double sign = selected%2 == 0 ? 1.0 : -1.0;
        result += sign*m(0,selected)*determinant(minor);
    }
    return result;
}

namespace {

template<std::size_t N, typename Matrix>
std::optional<Matrix> inverse_matrix(const Matrix& input, double tolerance) noexcept {
    if (!finite(input) || !valid_tolerance(tolerance)) {
        return std::nullopt;
    }
    std::array<std::array<double,N*2>,N> augmented{};
    std::array<double,N> scales{};
    for (std::size_t r = 0; r < N; ++r) {
        for (std::size_t c = 0; c < N; ++c) {
            augmented[r][c] = input(r,c);
            scales[r] = std::max(scales[r],std::abs(input(r,c)));
        }
        augmented[r][r+N] = 1.0;
        if (scales[r] == 0.0) {
            return std::nullopt;
        }
    }
    for (std::size_t c = 0; c < N; ++c) {
        std::size_t pivot = c;
        double best = std::abs(augmented[c][c])/scales[c];
        for (std::size_t r = c+1; r < N; ++r) {
            const double candidate = std::abs(augmented[r][c])/scales[r];
            if (candidate > best) {
                best = candidate;
                pivot = r;
            }
        }
        if (best <= tolerance) {
            return std::nullopt;
        }
        if (pivot != c) {
            std::swap(augmented[pivot],augmented[c]);
            std::swap(scales[pivot],scales[c]);
        }
        const double divisor = augmented[c][c];
        for (std::size_t k = 0; k < N*2; ++k) {
            augmented[c][k] /= divisor;
        }
        for (std::size_t r = 0; r < N; ++r) {
            if (r == c) {
                continue;
            }
            const double factor = augmented[r][c];
            for (std::size_t k = 0; k < N*2; ++k) {
                augmented[r][k] -= factor*augmented[c][k];
            }
        }
    }
    Matrix result = Matrix::zero();
    for (std::size_t r = 0; r < N; ++r) {
        for (std::size_t c = 0; c < N; ++c) {
            result(r,c) = augmented[r][c+N];
        }
    }
    return finite(result) ? std::optional<Matrix>{result} : std::nullopt;
}

} // namespace

std::optional<Mat3> inverse(const Mat3& m, double tolerance) noexcept {
    return inverse_matrix<3>(m,tolerance);
}

std::optional<Mat4> inverse(const Mat4& m, double tolerance) noexcept {
    return inverse_matrix<4>(m,tolerance);
}

Mat3 upper3x3(const Mat4& m) noexcept {
    Mat3 result;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            result(r,c) = m(r,c);
        }
    }
    return result;
}

Mat4 embed(const Mat3& m) noexcept {
    Mat4 result;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            result(r,c) = m(r,c);
        }
    }
    return result;
}

Mat3 outer_product(Vec3 a, Vec3 b) noexcept {
    Mat3 result;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            result(r,c) = a[r]*b[c];
        }
    }
    return result;
}

Mat3 skew_symmetric(Vec3 v) noexcept {
    return {{0.0,v.z,-v.y, -v.z,0.0,v.x, v.y,-v.x,0.0}};
}

Mat3 rotation_matrix(Quat q) noexcept {
    q = normalized(q);
    const double xx = q.x*q.x;
    const double yy = q.y*q.y;
    const double zz = q.z*q.z;
    const double xy_value = q.x*q.y;
    const double xz = q.x*q.z;
    const double yz = q.y*q.z;
    const double wx = q.w*q.x;
    const double wy = q.w*q.y;
    const double wz = q.w*q.z;
    return {{1.0-2.0*(yy+zz),2.0*(xy_value+wz),2.0*(xz-wy),
             2.0*(xy_value-wz),1.0-2.0*(xx+zz),2.0*(yz+wx),
             2.0*(xz+wy),2.0*(yz-wx),1.0-2.0*(xx+yy)}};
}

Quat quaternion(const Mat3& m) noexcept {
    if (!finite(m)) {
        return {};
    }
    Quat result;
    const double trace = m(0,0)+m(1,1)+m(2,2);
    if (trace > 0.0) {
        const double s = 2.0*std::sqrt(std::max(0.0,trace+1.0));
        if (s <= epsilon) {
            return {};
        }
        result = {(m(2,1)-m(1,2))/s,(m(0,2)-m(2,0))/s,
                  (m(1,0)-m(0,1))/s,s*0.25};
    } else if (m(0,0) >= m(1,1) && m(0,0) >= m(2,2)) {
        const double s = 2.0*std::sqrt(std::max(0.0,1.0+m(0,0)-m(1,1)-m(2,2)));
        if (s <= epsilon) {
            return {};
        }
        result = {s*0.25,(m(0,1)+m(1,0))/s,(m(0,2)+m(2,0))/s,(m(2,1)-m(1,2))/s};
    } else if (m(1,1) >= m(2,2)) {
        const double s = 2.0*std::sqrt(std::max(0.0,1.0+m(1,1)-m(0,0)-m(2,2)));
        if (s <= epsilon) {
            return {};
        }
        result = {(m(0,1)+m(1,0))/s,s*0.25,(m(1,2)+m(2,1))/s,(m(0,2)-m(2,0))/s};
    } else {
        const double s = 2.0*std::sqrt(std::max(0.0,1.0+m(2,2)-m(0,0)-m(1,1)));
        if (s <= epsilon) {
            return {};
        }
        result = {(m(0,2)+m(2,0))/s,(m(1,2)+m(2,1))/s,s*0.25,(m(1,0)-m(0,1))/s};
    }
    return normalized(result);
}

Mat4 translation_matrix(Vec3 v) noexcept {
    Mat4 result;
    result(0,3) = v.x;
    result(1,3) = v.y;
    result(2,3) = v.z;
    return result;
}

Mat4 scaling_matrix(Vec3 v) noexcept {
    Mat4 result;
    result(0,0) = v.x;
    result(1,1) = v.y;
    result(2,2) = v.z;
    return result;
}

Mat4 trs_matrix(Vec3 position, Quat rotation, Vec3 scale) noexcept {
    Mat4 result = embed(rotation_matrix(rotation));
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            result(r,c) *= scale[c];
        }
    }
    result(0,3) = position.x;
    result(1,3) = position.y;
    result(2,3) = position.z;
    return result;
}

Vec3 transform_point(const Mat4& m, Vec3 p) noexcept {
    return xyz(m*homogeneous(p));
}

Vec3 transform_vector(const Mat4& m, Vec3 v) noexcept {
    return xyz(m*homogeneous(v,0.0));
}

std::optional<Vec3> project_point(const Mat4& m, Vec3 p) noexcept {
    const Vec4 transformed = m*homogeneous(p);
    if (!finite(transformed) || std::abs(transformed.w) <= epsilon) {
        return std::nullopt;
    }
    const Vec3 result = xyz(transformed)/transformed.w;
    return finite(result) ? std::optional<Vec3>{result} : std::nullopt;
}

std::optional<Vec3> transform_normal(const Mat4& m, Vec3 normal) noexcept {
    const auto basis = normal_matrix(m);
    if (!basis || !normalize(normal)) {
        return std::nullopt;
    }
    Vec3 result = *basis*normal;
    return normalize(result) ? std::optional<Vec3>{result} : std::nullopt;
}

std::optional<Mat3> normal_matrix(const Mat4& m) noexcept {
    const auto inverted = inverse(upper3x3(m));
    return inverted ? std::optional<Mat3>{transpose(*inverted)} : std::nullopt;
}

Mat4 perspective(double vertical_fov, double aspect, double near_plane, double far_plane) {
    if (!valid_projection(vertical_fov,aspect,near_plane,far_plane)) {
        throw std::invalid_argument("Invalid perspective projection range");
    }
    const double cotangent = 1.0/std::tan(vertical_fov*0.5);
    const double depth = far_plane-near_plane;
    Mat4 result = Mat4::zero();
    result(0,0) = cotangent/aspect;
    result(1,1) = cotangent;
    result(2,2) = -(far_plane+near_plane)/depth;
    result(2,3) = -2.0*near_plane*(far_plane/depth);
    result(3,2) = -1.0;
    if (!finite(result)) {
        throw std::invalid_argument("Perspective projection exceeds finite precision");
    }
    return result;
}

Mat4 perspective_zero_to_one(double vertical_fov, double aspect, double near_plane, double far_plane) {
    Mat4 result = perspective(vertical_fov,aspect,near_plane,far_plane);
    result(2,2) = -far_plane/(far_plane-near_plane);
    result(2,3) = -near_plane*(far_plane/(far_plane-near_plane));
    return result;
}

Mat4 perspective_reverse_z(double vertical_fov, double aspect, double near_plane, double far_plane) {
    const bool infinite_far = far_plane == infinity;
    const double finite_far = infinite_far ? near_plane*2.0 : far_plane;
    if (!valid_projection(vertical_fov,aspect,near_plane,finite_far)) {
        throw std::invalid_argument("Invalid reversed depth projection range");
    }
    Mat4 result = Mat4::zero();
    const double cotangent = 1.0/std::tan(vertical_fov*0.5);
    result(0,0) = cotangent/aspect;
    result(1,1) = cotangent;
    result(2,2) = infinite_far ? 0.0 : near_plane/(far_plane-near_plane);
    result(2,3) = infinite_far ? near_plane : near_plane*(far_plane/(far_plane-near_plane));
    result(3,2) = -1.0;
    if (!finite(result)) {
        throw std::invalid_argument("Reversed depth projection exceeds finite precision");
    }
    return result;
}

Mat4 orthographic(double left, double right, double bottom, double top,
                   double near_plane, double far_plane) {
    if (!valid_orthographic(left,right,bottom,top,near_plane,far_plane)) {
        throw std::invalid_argument("Invalid orthographic projection range");
    }
    Mat4 result;
    result(0,0) = 2.0/(right-left);
    result(1,1) = 2.0/(top-bottom);
    result(2,2) = -2.0/(far_plane-near_plane);
    result(0,3) = -(right+left)/(right-left);
    result(1,3) = -(top+bottom)/(top-bottom);
    result(2,3) = -(far_plane+near_plane)/(far_plane-near_plane);
    if (!finite(result)) {
        throw std::invalid_argument("Orthographic projection exceeds finite precision");
    }
    return result;
}

Mat4 orthographic_zero_to_one(double left, double right, double bottom, double top,
                              double near_plane, double far_plane) {
    Mat4 result = orthographic(left,right,bottom,top,near_plane,far_plane);
    result(2,2) = -1.0/(far_plane-near_plane);
    result(2,3) = -near_plane/(far_plane-near_plane);
    return result;
}

Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 backward = eye-target;
    if (!finite(eye) || !finite(target) || !normalize(backward) || !normalize(up)) {
        throw std::invalid_argument("Invalid camera eye, target or up direction");
    }
    Vec3 right = cross(up,backward);
    if (!normalize(right)) {
        throw std::invalid_argument("Camera up direction is parallel to view direction");
    }
    const Vec3 corrected_up = cross(backward,right);
    Mat4 result;
    for (std::size_t c = 0; c < 3; ++c) {
        result(0,c) = right[c];
        result(1,c) = corrected_up[c];
        result(2,c) = backward[c];
    }
    result(0,3) = -dot(right,eye);
    result(1,3) = -dot(corrected_up,eye);
    result(2,3) = -dot(backward,eye);
    return result;
}

bool finite(const Transform& transform) noexcept {
    return finite(transform.position) && finite(transform.rotation) && finite(transform.scale);
}

Mat4 matrix(const Transform& transform) noexcept {
    return trs_matrix(transform.position,transform.rotation,transform.scale);
}

Vec3 transform_point(const Transform& transform, Vec3 p) noexcept {
    return rotate(transform.rotation,p*transform.scale)+transform.position;
}

Vec3 transform_vector(const Transform& transform, Vec3 v) noexcept {
    return rotate(transform.rotation,v*transform.scale);
}

std::optional<Vec3> inverse_transform_point(const Transform& transform, Vec3 p) noexcept {
    if (!finite(transform) || !finite(p) || transform.scale.x == 0.0 ||
        transform.scale.y == 0.0 || transform.scale.z == 0.0) {
        return std::nullopt;
    }
    const Vec3 result = rotate(conjugate(normalized(transform.rotation)),p-transform.position)/transform.scale;
    return finite(result) ? std::optional<Vec3>{result} : std::nullopt;
}

std::optional<Vec3> inverse_transform_vector(const Transform& transform, Vec3 v) noexcept {
    if (!finite(transform) || !finite(v) || transform.scale.x == 0.0 ||
        transform.scale.y == 0.0 || transform.scale.z == 0.0) {
        return std::nullopt;
    }
    const Vec3 result = rotate(conjugate(normalized(transform.rotation)),v)/transform.scale;
    return finite(result) ? std::optional<Vec3>{result} : std::nullopt;
}

Transform interpolate(const Transform& a, const Transform& b, double t) noexcept {
    return {lerp(a.position,b.position,t),slerp(a.rotation,b.rotation,t),lerp(a.scale,b.scale,t)};
}

Mat4 compose(const Transform& parent, const Transform& local) noexcept {
    return matrix(parent)*matrix(local);
}

std::optional<Transform> decompose(const Mat4& m, double tolerance) noexcept {
    if (!finite(m) || !valid_tolerance(tolerance) ||
        !almost_equal(m(3,0),0.0,tolerance) || !almost_equal(m(3,1),0.0,tolerance) ||
        !almost_equal(m(3,2),0.0,tolerance) || !almost_equal(m(3,3),1.0,tolerance)) {
        return std::nullopt;
    }
    Mat3 basis = upper3x3(m);
    Vec3 scales{length(column(basis,0)),length(column(basis,1)),length(column(basis,2))};
    if (!finite(scales) || scales.x <= tolerance || scales.y <= tolerance || scales.z <= tolerance) {
        return std::nullopt;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            basis(r,c) /= scales[c];
        }
    }
    if (std::abs(dot(column(basis,0),column(basis,1))) > tolerance ||
        std::abs(dot(column(basis,0),column(basis,2))) > tolerance ||
        std::abs(dot(column(basis,1),column(basis,2))) > tolerance) {
        return std::nullopt;
    }
    if (determinant(basis) < 0.0) {
        scales.x = -scales.x;
        for (std::size_t r = 0; r < 3; ++r) {
            basis(r,0) = -basis(r,0);
        }
    }
    return Transform{{m(0,3),m(1,3),m(2,3)},quaternion(basis),scales};
}

bool valid(const Ray& ray) noexcept {
    return finite(ray.origin) && finite(ray.direction) &&
           (ray.direction.x != 0.0 || ray.direction.y != 0.0 || ray.direction.z != 0.0);
}

bool valid(const Plane& plane) noexcept {
    return finite(plane.normal) && std::isfinite(plane.distance) && length(plane.normal) > 0.0;
}

bool valid(const Aabb& box) noexcept {
    return finite(box.min) && finite(box.max) &&
           box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

bool valid(const Sphere& sphere) noexcept {
    return finite(sphere.center) && std::isfinite(sphere.radius) && sphere.radius >= 0.0;
}

bool valid(const Capsule& capsule) noexcept {
    return finite(capsule.a) && finite(capsule.b) &&
           std::isfinite(capsule.radius) && capsule.radius >= 0.0;
}

bool valid(const Obb& box) noexcept {
    return finite(box.center) && finite(box.half_extents) && finite(box.orientation) &&
           box.half_extents.x >= 0.0 && box.half_extents.y >= 0.0 && box.half_extents.z >= 0.0;
}

std::optional<Plane> normalized(const Plane& plane) noexcept {
    const double magnitude = length(plane.normal);
    if (!valid(plane) || !std::isfinite(magnitude)) {
        return std::nullopt;
    }
    const double offset = plane.distance/magnitude;
    return std::isfinite(offset) ? std::optional<Plane>{Plane{plane.normal/magnitude,offset}} : std::nullopt;
}

std::optional<Plane> plane_from_points(Vec3 a, Vec3 b, Vec3 c) noexcept {
    if (!finite(a) || !finite(b) || !finite(c)) {
        return std::nullopt;
    }
    Vec3 normal = cross(b-a,c-a);
    if (!normalize(normal)) {
        return std::nullopt;
    }
    const double offset = -dot(normal,a);
    return std::isfinite(offset) ? std::optional<Plane>{Plane{normal,offset}} : std::nullopt;
}

double signed_distance(const Plane& plane, Vec3 p) noexcept {
    const auto unit = normalized(plane);
    return unit && finite(p) ? dot(unit->normal,p)+unit->distance : infinity;
}

Vec3 closest_point(const Plane& plane, Vec3 p) noexcept {
    const auto unit = normalized(plane);
    return unit ? p-unit->normal*(dot(unit->normal,p)+unit->distance) : p;
}

std::optional<Plane> transform_plane(const Mat4& transform, const Plane& plane) noexcept {
    const auto inverted = inverse(transform);
    if (!inverted || !valid(plane)) {
        return std::nullopt;
    }
    const Vec4 coefficients = transpose(*inverted)*homogeneous(plane.normal,plane.distance);
    return normalized(Plane{xyz(coefficients),coefficients.w});
}

Vec3 center(const Aabb& box) noexcept {
    return valid(box) ? box.min*0.5+box.max*0.5 : Vec3{};
}

Vec3 extent(const Aabb& box) noexcept {
    return valid(box) ? box.max-box.min : Vec3{};
}

Vec3 half_extent(const Aabb& box) noexcept {
    return valid(box) ? box.max*0.5-box.min*0.5 : Vec3{};
}

double surface_area(const Aabb& box) noexcept {
    const Vec3 size = extent(box);
    return 2.0*(size.x*size.y+size.x*size.z+size.y*size.z);
}

double volume(const Aabb& box) noexcept {
    const Vec3 size = extent(box);
    return size.x*size.y*size.z;
}

Aabb bounds(std::span<const Vec3> points) noexcept {
    Aabb result;
    for (const Vec3 point : points) {
        if (!finite(point)) {
            return {};
        }
        result.min = min(result.min,point);
        result.max = max(result.max,point);
    }
    return result;
}

Aabb bounds(const Sphere& sphere) noexcept {
    if (!valid(sphere)) {
        return {};
    }
    const Vec3 radius{sphere.radius,sphere.radius,sphere.radius};
    return {sphere.center-radius,sphere.center+radius};
}

Aabb bounds(const Capsule& capsule) noexcept {
    if (!valid(capsule)) {
        return {};
    }
    const Vec3 radius{capsule.radius,capsule.radius,capsule.radius};
    return {min(capsule.a,capsule.b)-radius,max(capsule.a,capsule.b)+radius};
}

Aabb bounds(const Triangle& triangle) noexcept {
    if (!finite(triangle.a) || !finite(triangle.b) || !finite(triangle.c)) {
        return {};
    }
    return {min(min(triangle.a,triangle.b),triangle.c),max(max(triangle.a,triangle.b),triangle.c)};
}

Aabb bounds(const Obb& box) noexcept {
    if (!valid(box)) {
        return {};
    }
    const Mat3 basis = rotation_matrix(box.orientation);
    const Vec3 radius = abs(column(basis,0))*box.half_extents.x+
                        abs(column(basis,1))*box.half_extents.y+
                        abs(column(basis,2))*box.half_extents.z;
    return {box.center-radius,box.center+radius};
}

Aabb merge(const Aabb& a, const Aabb& b) noexcept {
    if (!valid(a)) {
        return b;
    }
    if (!valid(b)) {
        return a;
    }
    return {min(a.min,b.min),max(a.max,b.max)};
}

Aabb include(const Aabb& box, Vec3 p) noexcept {
    if (!finite(p)) {
        return box;
    }
    return valid(box) ? Aabb{min(box.min,p),max(box.max,p)} : Aabb{p,p};
}

Aabb expanded(const Aabb& box, double margin) noexcept {
    if (!valid(box) || !std::isfinite(margin)) {
        return {};
    }
    const Vec3 offset{margin,margin,margin};
    const Aabb result{box.min-offset,box.max+offset};
    return valid(result) ? result : Aabb{};
}

Aabb translated(const Aabb& box, Vec3 offset) noexcept {
    if (!valid(box) || !finite(offset)) {
        return {};
    }
    return {box.min+offset,box.max+offset};
}

Aabb transform_bounds(const Mat4& transform, const Aabb& box) noexcept {
    if (!valid(box) || !finite(transform) || transform(3,0) != 0.0 ||
        transform(3,1) != 0.0 || transform(3,2) != 0.0 || transform(3,3) != 1.0) {
        return {};
    }
    const Vec3 middle = transform_point(transform,center(box));
    const Vec3 half = half_extent(box);
    Vec3 radius{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            radius[r] += std::abs(transform(r,c))*half[c];
        }
    }
    return {middle-radius,middle+radius};
}

Sphere merge(const Sphere& a, const Sphere& b) noexcept {
    if (!valid(a)) {
        return b;
    }
    if (!valid(b)) {
        return a;
    }
    const Vec3 delta = b.center-a.center;
    const double separation = length(delta);
    if (a.radius >= separation+b.radius) {
        return a;
    }
    if (b.radius >= separation+a.radius) {
        return b;
    }
    const double radius = (separation+a.radius+b.radius)*0.5;
    return {a.center+normalized(delta)*(radius-a.radius),radius};
}

Sphere bounding_sphere(std::span<const Vec3> points) noexcept {
    const Aabb box = bounds(points);
    if (!valid(box)) {
        return {{},0.0};
    }
    Sphere result{center(box),0.0};
    for (const Vec3 point : points) {
        result.radius = std::max(result.radius,distance(result.center,point));
    }
    return result;
}

Sphere transform_sphere(const Mat4& transform, const Sphere& sphere) noexcept {
    if (!valid(sphere) || !finite(transform) || transform(3,0) != 0.0 ||
        transform(3,1) != 0.0 || transform(3,2) != 0.0 || transform(3,3) != 1.0) {
        return {{},-1.0};
    }
    double row_norm = 0.0;
    double column_norm = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        double row_sum = 0.0;
        double column_sum = 0.0;
        for (std::size_t other = 0; other < 3; ++other) {
            row_sum += std::abs(transform(axis,other));
            column_sum += std::abs(transform(other,axis));
        }
        row_norm = std::max(row_norm,row_sum);
        column_norm = std::max(column_norm,column_sum);
    }
    return {transform_point(transform,sphere.center),sphere.radius*std::sqrt(row_norm*column_norm)};
}

bool contains(const Aabb& box, Vec3 p) noexcept {
    return valid(box) && finite(p) && p.x >= box.min.x && p.x <= box.max.x &&
           p.y >= box.min.y && p.y <= box.max.y && p.z >= box.min.z && p.z <= box.max.z;
}

bool contains(const Aabb& outer, const Aabb& inner) noexcept {
    return valid(inner) && contains(outer,inner.min) && contains(outer,inner.max);
}

bool contains(const Sphere& sphere, Vec3 p) noexcept {
    return valid(sphere) && finite(p) && distance(sphere.center,p) <= sphere.radius;
}

bool overlaps(const Aabb& a, const Aabb& b) noexcept {
    return valid(a) && valid(b) && a.min.x <= b.max.x && a.max.x >= b.min.x &&
           a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool overlaps(const Sphere& a, const Sphere& b) noexcept {
    return valid(a) && valid(b) && distance(a.center,b.center) <= a.radius+b.radius;
}

bool overlaps(const Aabb& box, const Sphere& sphere) noexcept {
    return valid(box) && valid(sphere) && distance(closest_point(box,sphere.center),sphere.center) <= sphere.radius;
}

bool overlaps(const Capsule& capsule, const Sphere& sphere) noexcept {
    return valid(capsule) && valid(sphere) &&
           distance(closest_point(Segment{capsule.a,capsule.b},sphere.center),sphere.center) <=
           capsule.radius+sphere.radius;
}

bool overlaps(const Capsule& a, const Capsule& b) noexcept {
    if (!valid(a) || !valid(b)) {
        return false;
    }
    const auto points = closest_points(Segment{a.a,a.b},Segment{b.a,b.b});
    return distance(points.first,points.second) <= a.radius+b.radius;
}

bool overlaps(const Obb& a, const Obb& b, double tolerance) noexcept {
    if (!valid(a) || !valid(b) || !valid_tolerance(tolerance)) {
        return false;
    }
    const Mat3 basis_a = rotation_matrix(a.orientation);
    const Mat3 basis_b = rotation_matrix(b.orientation);
    Mat3 relative = Mat3::zero();
    Mat3 absolute = Mat3::zero();
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            relative(i,j) = dot(column(basis_a,i),column(basis_b,j));
            absolute(i,j) = std::abs(relative(i,j))+tolerance;
        }
    }
    const Vec3 offset = transpose(basis_a)*(b.center-a.center);
    for (std::size_t i = 0; i < 3; ++i) {
        double radius_b = 0.0;
        for (std::size_t j = 0; j < 3; ++j) {
            radius_b += b.half_extents[j]*absolute(i,j);
        }
        if (std::abs(offset[i]) > a.half_extents[i]+radius_b) {
            return false;
        }
    }
    for (std::size_t j = 0; j < 3; ++j) {
        double radius_a = 0.0;
        double projected_offset = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            radius_a += a.half_extents[i]*absolute(i,j);
            projected_offset += offset[i]*relative(i,j);
        }
        if (std::abs(projected_offset) > radius_a+b.half_extents[j]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t i1 = (i+1)%3;
        const std::size_t i2 = (i+2)%3;
        for (std::size_t j = 0; j < 3; ++j) {
            const std::size_t j1 = (j+1)%3;
            const std::size_t j2 = (j+2)%3;
            const double radius_a = a.half_extents[i1]*absolute(i2,j)+a.half_extents[i2]*absolute(i1,j);
            const double radius_b = b.half_extents[j1]*absolute(i,j2)+b.half_extents[j2]*absolute(i,j1);
            const double separation = std::abs(offset[i2]*relative(i1,j)-offset[i1]*relative(i2,j));
            if (separation > radius_a+radius_b) {
                return false;
            }
        }
    }
    return true;
}

Vec3 closest_point(const Aabb& box, Vec3 p) noexcept {
    return valid(box) ? clamp(p,box.min,box.max) : p;
}

Vec3 closest_point(const Segment& segment, Vec3 p) noexcept {
    const Vec3 delta = segment.b-segment.a;
    const double denominator = length_squared(delta);
    if (!std::isfinite(denominator) || denominator <= epsilon*epsilon) {
        return segment.a;
    }
    return segment.a+delta*saturate(dot(p-segment.a,delta)/denominator);
}

Vec3 closest_point(const Triangle& triangle, Vec3 p) noexcept {
    const Vec3 ab = triangle.b-triangle.a;
    const Vec3 ac = triangle.c-triangle.a;
    if (length_squared(cross(ab,ac)) <= epsilon*epsilon) {
        const Vec3 first = closest_point(Segment{triangle.a,triangle.b},p);
        const Vec3 second = closest_point(Segment{triangle.b,triangle.c},p);
        const Vec3 third = closest_point(Segment{triangle.c,triangle.a},p);
        const Vec3 nearest = distance_squared(first,p) <= distance_squared(second,p) ? first : second;
        return distance_squared(nearest,p) <= distance_squared(third,p) ? nearest : third;
    }
    const Vec3 ap = p-triangle.a;
    const double d1 = dot(ab,ap);
    const double d2 = dot(ac,ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return triangle.a;
    }
    const Vec3 bp = p-triangle.b;
    const double d3 = dot(ab,bp);
    const double d4 = dot(ac,bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return triangle.b;
    }
    const double vc = d1*d4-d3*d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return triangle.a+ab*(d1/(d1-d3));
    }
    const Vec3 cp = p-triangle.c;
    const double d5 = dot(ab,cp);
    const double d6 = dot(ac,cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return triangle.c;
    }
    const double vb = d5*d2-d1*d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return triangle.a+ac*(d2/(d2-d6));
    }
    const double va = d3*d6-d5*d4;
    if (va <= 0.0 && d4-d3 >= 0.0 && d5-d6 >= 0.0) {
        return triangle.b+(triangle.c-triangle.b)*((d4-d3)/((d4-d3)+(d5-d6)));
    }
    const double denominator = va+vb+vc;
    return triangle.a+ab*(vb/denominator)+ac*(vc/denominator);
}

Vec3 closest_point(const Obb& box, Vec3 p) noexcept {
    if (!valid(box)) {
        return p;
    }
    const Quat orientation = normalized(box.orientation);
    const Vec3 local = rotate(conjugate(orientation),p-box.center);
    return box.center+rotate(orientation,clamp(local,-box.half_extents,box.half_extents));
}

double distance_squared(const Aabb& box, Vec3 p) noexcept {
    return valid(box) ? distance_squared(closest_point(box,p),p) : infinity;
}

double distance_squared(const Segment& segment, Vec3 p) noexcept {
    return distance_squared(closest_point(segment,p),p);
}

double distance_squared(const Segment& a, const Segment& b) noexcept {
    const auto points = closest_points(a,b);
    return distance_squared(points.first,points.second);
}

std::pair<Vec3,Vec3> closest_points(const Segment& first, const Segment& second) noexcept {
    const Vec3 d1 = first.b-first.a;
    const Vec3 d2 = second.b-second.a;
    const Vec3 r = first.a-second.a;
    const double a = dot(d1,d1);
    const double e = dot(d2,d2);
    const double f = dot(d2,r);
    double s = 0.0;
    double t = 0.0;
    if (a <= epsilon*epsilon && e <= epsilon*epsilon) {
        return {first.a,second.a};
    }
    if (a <= epsilon*epsilon) {
        t = saturate(f/e);
    } else {
        const double c = dot(d1,r);
        if (e <= epsilon*epsilon) {
            s = saturate(-c/a);
        } else {
            const double b = dot(d1,d2);
            const double denominator = a*e-b*b;
            if (denominator > epsilon*a*e) {
                s = saturate((b*f-c*e)/denominator);
            }
            t = (b*s+f)/e;
            if (t < 0.0) {
                t = 0.0;
                s = saturate(-c/a);
            } else if (t > 1.0) {
                t = 1.0;
                s = saturate((b-c)/a);
            }
        }
    }
    return {first.a+d1*s,second.a+d2*t};
}

std::optional<Vec3> barycentric(const Triangle& triangle, Vec3 p) noexcept {
    const Vec3 a = triangle.b-triangle.a;
    const Vec3 b = triangle.c-triangle.a;
    const Vec3 c = p-triangle.a;
    const double aa = dot(a,a);
    const double ab = dot(a,b);
    const double bb = dot(b,b);
    const double ca = dot(c,a);
    const double cb = dot(c,b);
    const double denominator = aa*bb-ab*ab;
    if (!std::isfinite(denominator) || denominator <= epsilon*aa*bb) {
        return std::nullopt;
    }
    const double v = (bb*ca-ab*cb)/denominator;
    const double w = (aa*cb-ab*ca)/denominator;
    const Vec3 result{1.0-v-w,v,w};
    return finite(result) ? std::optional<Vec3>{result} : std::nullopt;
}

Vec3 triangle_normal(const Triangle& triangle) noexcept {
    return normalized(cross(triangle.b-triangle.a,triangle.c-triangle.a));
}

double triangle_area(const Triangle& triangle) noexcept {
    return length(cross(triangle.b-triangle.a,triangle.c-triangle.a))*0.5;
}

std::optional<RayInterval> ray_interval(const Ray& ray, const Aabb& box,
                                      double minimum, double maximum) noexcept {
    if (!valid(ray) || !valid(box) || !valid_interval(minimum,maximum)) {
        return std::nullopt;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (ray.direction[axis] == 0.0) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        double near_hit = (box.min[axis]-ray.origin[axis])/ray.direction[axis];
        double far_hit = (box.max[axis]-ray.origin[axis])/ray.direction[axis];
        if (near_hit > far_hit) {
            std::swap(near_hit,far_hit);
        }
        minimum = std::max(minimum,near_hit);
        maximum = std::min(maximum,far_hit);
        if (minimum > maximum) {
            return std::nullopt;
        }
    }
    return RayInterval{minimum,maximum};
}

std::optional<RayHit> intersect(const Ray& ray, const Plane& plane,
                               double minimum, double maximum) noexcept {
    const auto unit = normalized(plane);
    if (!valid(ray) || !unit || !valid_interval(minimum,maximum)) {
        return std::nullopt;
    }
    const double denominator = dot(unit->normal,ray.direction);
    if (std::abs(denominator) <= epsilon*length(ray.direction)) {
        return std::nullopt;
    }
    const double t = -(dot(unit->normal,ray.origin)+unit->distance)/denominator;
    if (!std::isfinite(t) || t < minimum || t > maximum) {
        return std::nullopt;
    }
    return RayHit{t,ray.point(t),unit->normal,{},false};
}

std::optional<RayHit> intersect(const Ray& ray, const Sphere& sphere,
                               double minimum, double maximum) noexcept {
    if (!valid(ray) || !valid(sphere) || !valid_interval(minimum,maximum)) {
        return std::nullopt;
    }
    const Vec3 offset = ray.origin-sphere.center;
    const double a = dot(ray.direction,ray.direction);
    const double half_b = dot(offset,ray.direction);
    const double c = dot(offset,offset)-sphere.radius*sphere.radius;
    const double discriminant = half_b*half_b-a*c;
    if (!std::isfinite(discriminant) || discriminant < 0.0 || a <= 0.0) {
        return std::nullopt;
    }
    const double root = std::sqrt(discriminant);
    double t = (-half_b-root)/a;
    if (t < minimum || t > maximum) {
        t = (-half_b+root)/a;
    }
    if (!std::isfinite(t) || t < minimum || t > maximum) {
        return std::nullopt;
    }
    const Vec3 point = ray.point(t);
    return RayHit{t,point,normalized(point-sphere.center,-normalized(ray.direction)),{},c < 0.0};
}

std::optional<RayHit> intersect(const Ray& ray, const Aabb& box,
                               double minimum, double maximum) noexcept {
    if (!valid(ray) || !valid(box) || !valid_interval(minimum,maximum)) {
        return std::nullopt;
    }
    double enter = -infinity;
    double exit = infinity;
    Vec3 enter_normal{};
    Vec3 exit_normal{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (ray.direction[axis] == 0.0) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        double near_hit = (box.min[axis]-ray.origin[axis])/ray.direction[axis];
        double far_hit = (box.max[axis]-ray.origin[axis])/ray.direction[axis];
        Vec3 normal{};
        normal[axis] = -1.0;
        if (near_hit > far_hit) {
            std::swap(near_hit,far_hit);
            normal[axis] = 1.0;
        }
        if (near_hit > enter) {
            enter = near_hit;
            enter_normal = normal;
        }
        if (far_hit < exit) {
            exit = far_hit;
            exit_normal = -normal;
        }
        if (enter > exit) {
            return std::nullopt;
        }
    }
    const bool from_inside = enter < minimum;
    const double t = from_inside ? exit : enter;
    if (!std::isfinite(t) || t < minimum || t > maximum) {
        return std::nullopt;
    }
    return RayHit{t,ray.point(t),from_inside ? exit_normal : enter_normal,{},contains(box,ray.origin)};
}

std::optional<RayHit> intersect(const Ray& ray, const Triangle& triangle,
                               double minimum, double maximum, bool cull_backface) noexcept {
    if (!valid(ray) || !valid_interval(minimum,maximum) ||
        !finite(triangle.a) || !finite(triangle.b) || !finite(triangle.c)) {
        return std::nullopt;
    }
    const Vec3 edge1 = triangle.b-triangle.a;
    const Vec3 edge2 = triangle.c-triangle.a;
    const Vec3 perpendicular_edge = cross(ray.direction,edge2);
    const double determinant_value = dot(edge1,perpendicular_edge);
    const double threshold = epsilon*length(edge1)*length(edge2)*length(ray.direction);
    if (!std::isfinite(determinant_value) ||
        (cull_backface ? determinant_value <= threshold : std::abs(determinant_value) <= threshold)) {
        return std::nullopt;
    }
    const double reciprocal = 1.0/determinant_value;
    const Vec3 offset = ray.origin-triangle.a;
    const double u = dot(offset,perpendicular_edge)*reciprocal;
    if (u < 0.0 || u > 1.0) {
        return std::nullopt;
    }
    const Vec3 crossed = cross(offset,edge1);
    const double v = dot(ray.direction,crossed)*reciprocal;
    if (v < 0.0 || u+v > 1.0) {
        return std::nullopt;
    }
    const double t = dot(edge2,crossed)*reciprocal;
    if (!std::isfinite(t) || t < minimum || t > maximum) {
        return std::nullopt;
    }
    return RayHit{t,ray.point(t),normalized(cross(edge1,edge2)),{1.0-u-v,u,v},false};
}

std::optional<RayHit> intersect(const Ray& ray, const Obb& box,
                               double minimum, double maximum) noexcept {
    if (!valid(ray) || !valid(box)) {
        return std::nullopt;
    }
    const Quat orientation = normalized(box.orientation);
    const Quat inverse_orientation = conjugate(orientation);
    const Ray local{rotate(inverse_orientation,ray.origin-box.center),rotate(inverse_orientation,ray.direction)};
    auto hit = intersect(local,Aabb{-box.half_extents,box.half_extents},minimum,maximum);
    if (hit) {
        hit->point = ray.point(hit->distance);
        hit->normal = rotate(orientation,hit->normal);
    }
    return hit;
}

std::optional<RayHit> intersect(const Ray& ray, const Capsule& capsule,
                               double minimum, double maximum) noexcept {
    if (!valid(ray) || !valid(capsule) || !valid_interval(minimum,maximum)) {
        return std::nullopt;
    }
    const Vec3 axis_delta = capsule.b-capsule.a;
    const double axis_length = length(axis_delta);
    if (axis_length <= epsilon) {
        return intersect(ray,Sphere{capsule.a,capsule.radius},minimum,maximum);
    }
    const Vec3 axis = axis_delta/axis_length;
    const Vec3 offset = ray.origin-capsule.a;
    const double along_direction = dot(ray.direction,axis);
    const double along_origin = dot(offset,axis);
    const double direction_squared = dot(ray.direction,ray.direction);
    const double qa = direction_squared-along_direction*along_direction;
    const double qb = dot(offset,ray.direction)-along_origin*along_direction;
    const double qc = dot(offset,offset)-along_origin*along_origin-capsule.radius*capsule.radius;
    std::optional<RayHit> best;
    const bool inside = distance(closest_point(Segment{capsule.a,capsule.b},ray.origin),ray.origin) < capsule.radius;
    const auto consider = [&](double t, Vec3 normal) {
        if (std::isfinite(t) && t >= minimum && t <= maximum && (!best || t < best->distance)) {
            best = RayHit{t,ray.point(t),normalized(normal,-normalized(ray.direction)),{},inside};
        }
    };
    const double discriminant = qb*qb-qa*qc;
    if (qa > epsilon*direction_squared && std::isfinite(discriminant) && discriminant >= 0.0) {
        const double root = std::sqrt(discriminant);
        for (const double t : std::array<double,2>{(-qb-root)/qa,(-qb+root)/qa}) {
            const double axial = along_origin+t*along_direction;
            if (axial >= 0.0 && axial <= axis_length) {
                consider(t,ray.point(t)-(capsule.a+axis*axial));
            }
        }
    }
    for (std::size_t cap = 0; cap < 2; ++cap) {
        const Vec3 cap_center = cap == 0 ? capsule.a : capsule.b;
        const Vec3 cap_offset = ray.origin-cap_center;
        const double half_b = dot(cap_offset,ray.direction);
        const double c = dot(cap_offset,cap_offset)-capsule.radius*capsule.radius;
        const double cap_discriminant = half_b*half_b-direction_squared*c;
        if (!std::isfinite(cap_discriminant) || cap_discriminant < 0.0) {
            continue;
        }
        const double root = std::sqrt(cap_discriminant);
        for (const double t : std::array<double,2>{(-half_b-root)/direction_squared,(-half_b+root)/direction_squared}) {
            const Vec3 point = ray.point(t);
            const double axial = dot(point-capsule.a,axis);
            if ((cap == 0 && axial <= 0.0) || (cap == 1 && axial >= axis_length)) {
                consider(t,point-cap_center);
            }
        }
    }
    return best;
}

PlaneSide classify(const Plane& plane, const Aabb& box) noexcept {
    if (!valid(plane) || !valid(box)) {
        return PlaneSide::intersecting;
    }
    const double separation = dot(plane.normal,center(box))+plane.distance;
    const double radius = dot(abs(plane.normal),half_extent(box));
    if (separation > radius) {
        return PlaneSide::positive;
    }
    return separation < -radius ? PlaneSide::negative : PlaneSide::intersecting;
}

PlaneSide classify(const Plane& plane, const Sphere& sphere) noexcept {
    const auto unit = normalized(plane);
    if (!unit || !valid(sphere)) {
        return PlaneSide::intersecting;
    }
    const double separation = dot(unit->normal,sphere.center)+unit->distance;
    if (separation > sphere.radius) {
        return PlaneSide::positive;
    }
    return separation < -sphere.radius ? PlaneSide::negative : PlaneSide::intersecting;
}

std::optional<Frustum> extract_frustum(const Mat4& view_projection, ClipDepth depth) noexcept {
    const auto inverted = inverse(view_projection);
    if (!inverted) {
        return std::nullopt;
    }
    const Vec4 fourth = row(view_projection,3);
    const std::array<Vec4,6> coefficients{
        fourth+row(view_projection,0),fourth-row(view_projection,0),
        fourth+row(view_projection,1),fourth-row(view_projection,1),
        depth == ClipDepth::zero_to_one ? row(view_projection,2) : fourth+row(view_projection,2),
        fourth-row(view_projection,2)
    };
    Frustum result;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        const auto plane = normalized(Plane{xyz(coefficients[i]),coefficients[i].w});
        if (!plane) {
            return std::nullopt;
        }
        result.planes[i] = *plane;
    }
    for (std::size_t i = 0; i < result.corners.size(); ++i) {
        const double x = (i&1U) != 0 ? 1.0 : -1.0;
        const double y = (i&2U) != 0 ? 1.0 : -1.0;
        const double z = (i&4U) != 0 ? 1.0 : (depth == ClipDepth::zero_to_one ? 0.0 : -1.0);
        const auto point = project_point(*inverted,{x,y,z});
        if (!point) {
            return std::nullopt;
        }
        result.corners[i] = *point;
    }
    return result;
}

bool intersects(const Frustum& frustum, const Aabb& box) noexcept {
    if (!valid(box)) {
        return false;
    }
    for (const Plane& plane : frustum.planes) {
        if (classify(plane,box) == PlaneSide::negative) {
            return false;
        }
    }
    return true;
}

bool intersects(const Frustum& frustum, const Sphere& sphere) noexcept {
    if (!valid(sphere)) {
        return false;
    }
    for (const Plane& plane : frustum.planes) {
        if (classify(plane,sphere) == PlaneSide::negative) {
            return false;
        }
    }
    return true;
}

bool contains(const Frustum& frustum, Vec3 p) noexcept {
    if (!finite(p)) {
        return false;
    }
    for (const Plane& plane : frustum.planes) {
        if (!valid(plane) || dot(plane.normal,p)+plane.distance < 0.0) {
            return false;
        }
    }
    return true;
}

std::optional<Vec3> unproject(Vec3 window, const Mat4& inverse_view_projection,
                              Vec4 viewport, ClipDepth depth) noexcept {
    if (!finite(window) || !finite(viewport) || viewport.z <= 0.0 || viewport.w <= 0.0 ||
        window.z < 0.0 || window.z > 1.0) {
        return std::nullopt;
    }
    const Vec3 ndc{2.0*(window.x-viewport.x)/viewport.z-1.0,
                   2.0*(window.y-viewport.y)/viewport.w-1.0,
                   depth == ClipDepth::zero_to_one ? window.z : 2.0*window.z-1.0};
    return project_point(inverse_view_projection,ndc);
}

std::optional<Ray> picking_ray(Vec2 pixel, const Mat4& inverse_view_projection,
                               Vec4 viewport, ClipDepth depth) noexcept {
    const auto near_point = unproject({pixel.x,pixel.y,0.0},inverse_view_projection,viewport,depth);
    const auto far_point = unproject({pixel.x,pixel.y,1.0},inverse_view_projection,viewport,depth);
    if (!near_point || !far_point) {
        return std::nullopt;
    }
    Vec3 direction = *far_point-*near_point;
    if (!normalize(direction)) {
        return std::nullopt;
    }
    return Ray{*near_point,direction};
}

Vec3 cubic_bezier(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept {
    const Vec3 ab = lerp(a,b,t);
    const Vec3 bc = lerp(b,c,t);
    const Vec3 cd = lerp(c,d,t);
    return lerp(lerp(ab,bc,t),lerp(bc,cd,t),t);
}

Vec3 cubic_bezier_tangent(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept {
    const double u = 1.0-t;
    return (b-a)*(3.0*u*u)+(c-b)*(6.0*u*t)+(d-c)*(3.0*t*t);
}

Vec3 catmull_rom(Vec3 a, Vec3 b, Vec3 c, Vec3 d, double t) noexcept {
    const double t2 = t*t;
    const double t3 = t2*t;
    return (b*2.0+(c-a)*t+(a*2.0-b*5.0+c*4.0-d)*t2+(-a+b*3.0-c*3.0+d)*t3)*0.5;
}

Vec3 cubic_hermite(Vec3 a, Vec3 tangent_a, Vec3 b, Vec3 tangent_b, double t) noexcept {
    const double t2 = t*t;
    const double t3 = t2*t;
    return a*(2.0*t3-3.0*t2+1.0)+tangent_a*(t3-2.0*t2+t)+
           b*(-2.0*t3+3.0*t2)+tangent_b*(t3-t2);
}

double approximate_bezier_length(Vec3 a, Vec3 b, Vec3 c, Vec3 d, std::uint32_t segments) noexcept {
    if (!finite(a) || !finite(b) || !finite(c) || !finite(d) || segments == 0) {
        return 0.0;
    }
    segments = std::min(segments,std::uint32_t{65536});
    double result = 0.0;
    Vec3 previous = a;
    for (std::uint32_t i = 1; i <= segments; ++i) {
        const Vec3 point = cubic_bezier(a,b,c,d,static_cast<double>(i)/static_cast<double>(segments));
        result += distance(previous,point);
        previous = point;
    }
    return result;
}

double halton(std::uint64_t index, std::uint32_t base) noexcept {
    if (base < 2) {
        return 0.0;
    }
    double fraction = 1.0;
    double result = 0.0;
    while (index != 0) {
        fraction /= static_cast<double>(base);
        result += fraction*static_cast<double>(index%base);
        index /= base;
    }
    return result;
}

Vec2 concentric_disk(Vec2 uniform_sample) noexcept {
    if (!finite(uniform_sample)) {
        return {};
    }
    const Vec2 square = clamp(uniform_sample,{0.0,0.0},{1.0,1.0})*2.0-Vec2{1.0,1.0};
    if (square.x == 0.0 && square.y == 0.0) {
        return {};
    }
    double radius = 0.0;
    double angle = 0.0;
    if (std::abs(square.y) > std::abs(square.x)) {
        radius = square.y;
        angle = pi*0.5-(pi*0.25)*(square.x/square.y);
    } else {
        radius = square.x;
        angle = (pi*0.25)*(square.y/square.x);
    }
    return {radius*std::cos(angle),radius*std::sin(angle)};
}

Vec3 uniform_sphere(Vec2 uniform_sample) noexcept {
    if (!finite(uniform_sample)) {
        return {0.0,1.0,0.0};
    }
    const double y = 1.0-2.0*saturate(uniform_sample.x);
    const double radius = std::sqrt(std::max(0.0,1.0-y*y));
    const double angle = tau*saturate(uniform_sample.y);
    return {radius*std::cos(angle),y,radius*std::sin(angle)};
}

Vec3 cosine_hemisphere(Vec2 uniform_sample) noexcept {
    const Vec2 disk = concentric_disk(uniform_sample);
    return {disk.x,std::sqrt(std::max(0.0,1.0-dot(disk,disk))),disk.y};
}

Vec3 spherical_to_cartesian(double radius, double azimuth, double elevation) noexcept {
    const double horizontal = radius*std::cos(elevation);
    return {horizontal*std::cos(azimuth),radius*std::sin(elevation),horizontal*std::sin(azimuth)};
}

Vec3 cartesian_to_spherical(Vec3 position) noexcept {
    const double radius = length(position);
    if (!finite(position) || !std::isfinite(radius) || radius == 0.0) {
        return {};
    }
    return {radius,std::atan2(position.z,position.x),std::asin(clamp(position.y/radius,-1.0,1.0))};
}

Vec2 octahedral_encode(Vec3 normal) noexcept {
    normal = normalized(normal,{0.0,0.0,1.0});
    const double sum = std::abs(normal.x)+std::abs(normal.y)+std::abs(normal.z);
    normal /= sum;
    Vec2 result{normal.x,normal.y};
    if (normal.z < 0.0) {
        result = {(1.0-std::abs(normal.y))*sign_nonzero(normal.x),
                  (1.0-std::abs(normal.x))*sign_nonzero(normal.y)};
    }
    return result;
}

Vec3 octahedral_decode(Vec2 encoded) noexcept {
    if (!finite(encoded)) {
        return {0.0,0.0,1.0};
    }
    encoded = clamp(encoded,{-1.0,-1.0},{1.0,1.0});
    Vec3 result{encoded.x,encoded.y,1.0-std::abs(encoded.x)-std::abs(encoded.y)};
    if (result.z < 0.0) {
        const double x = (1.0-std::abs(result.y))*sign_nonzero(result.x);
        const double y = (1.0-std::abs(result.x))*sign_nonzero(result.y);
        result.x = x;
        result.y = y;
    }
    return normalized(result,{0.0,0.0,1.0});
}

} // namespace ge::math
