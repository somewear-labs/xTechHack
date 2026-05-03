#include "geolocation.hpp"

#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>

namespace {

constexpr double kMetersPerDegreeLat = 111320.0;
constexpr double kPi = std::numbers::pi_v<double>;

double get_env_double(const char *name, double default_val) {
    const char *s = std::getenv(name);
    if (!s || !*s) return default_val;
    try {
        return std::stod(s);
    } catch (...) {
        return default_val;
    }
}

double deg2rad(double deg) { return deg * (kPi / 180.0); }

std::array<double, 3> mat3_vec3(const std::array<double, 9> &m, const std::array<double, 3> &v) {
    return {
        m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
        m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
        m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
    };
}

std::array<double, 3> mat3T_vec3(const std::array<double, 9> &m, const std::array<double, 3> &v) {
    return {
        m[0] * v[0] + m[3] * v[1] + m[6] * v[2],
        m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
        m[2] * v[0] + m[5] * v[1] + m[8] * v[2],
    };
}

std::array<double, 3> cross3(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

std::array<double, 3> normalize3(const std::array<double, 3> &v) {
    double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n == 0.0) return v;
    return {v[0] / n, v[1] / n, v[2] / n};
}

}  // namespace

CameraConfig load_camera_config_from_env() {
    return CameraConfig{
        .fx          = get_env_double("CAM_FX",      822.0782),
        .fy          = get_env_double("CAM_FY",      826.7707),
        .cx          = get_env_double("CAM_CX",      610.3624),
        .cy          = get_env_double("CAM_CY",      332.9710),
        .heading_deg = get_env_double("CAM_HEADING", 315.0),
        .pitch_deg   = get_env_double("CAM_PITCH",    20.0),
        .roll_deg    = get_env_double("CAM_ROLL",      0.0),
        .altitude_m  = get_env_double("CAM_ALT_M",     1.3383),
        .latitude    = get_env_double("CAM_LAT",       0.0),
        .longitude   = get_env_double("CAM_LON",       0.0),
    };
}

CameraGeolocator::CameraGeolocator(const CameraConfig &config)
    : config_(config)
{
    K_inv_ = {
        1.0 / config_.fx, 0.0,              -config_.cx / config_.fx,
        0.0,              1.0 / config_.fy, -config_.cy / config_.fy,
        0.0,              0.0,               1.0,
    };

    double h = deg2rad(config_.heading_deg);
    double p = deg2rad(config_.pitch_deg);

    std::array<double, 3> fwd = {
        std::sin(h) * std::cos(p),
        std::cos(h) * std::cos(p),
        -std::sin(p),
    };
    std::array<double, 3> world_up = {0.0, 0.0, 1.0};
    auto right  = normalize3(cross3(fwd, world_up));
    auto cam_up = normalize3(cross3(right, fwd));

    R_ = {
         right[0],   right[1],   right[2],
        -cam_up[0], -cam_up[1], -cam_up[2],
         fwd[0],     fwd[1],     fwd[2],
    };
}

std::optional<GeoPoint> CameraGeolocator::pixel_to_gps(double u, double v) const {
    auto p_cam     = mat3_vec3(K_inv_, {u, v, 1.0});
    auto ray_world = normalize3(mat3T_vec3(R_, p_cam));

    if (ray_world[2] >= 0.0) return std::nullopt;

    double t       = -config_.altitude_m / ray_world[2];
    double east    = t * ray_world[0];
    double north   = t * ray_world[1];
    double range_m = std::sqrt(east * east + north * north);

    double lat = config_.latitude  + north / kMetersPerDegreeLat;
    double lon = config_.longitude + east  / (kMetersPerDegreeLat * std::cos(deg2rad(config_.latitude)));

    return GeoPoint{
        .latitude  = lat,
        .longitude = lon,
        .range_m   = range_m,
        .east_m    = east,
        .north_m   = north,
    };
}
