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
    const double cy_default = get_env_double("CAM_CY", 332.9710);
    return CameraConfig{
        .fx              = get_env_double("CAM_FX",      822.0782),
        .fy              = get_env_double("CAM_FY",      826.7707),
        .cx              = get_env_double("CAM_CX",      610.3624),
        .cy              = cy_default,
        .heading_deg     = get_env_double("CAM_HEADING", 315.0),
        .pitch_deg       = get_env_double("CAM_PITCH",    20.0),
        .roll_deg        = get_env_double("CAM_ROLL",      0.0),
        .altitude_m      = get_env_double("CAM_ALT_M",     1.3383),
        .latitude        = get_env_double("CAM_LAT",       0.0),
        .longitude       = get_env_double("CAM_LON",       0.0),
        .refraction_k    = get_env_double("CAM_REFRACTION_K", 4.0/3.0),
        .target_alt_m    = get_env_double("TARGET_ALT_M",     0.0),
        .horizon_v_at_cx = get_env_double("CAM_HORIZON_V_AT_CX", cy_default),
        .horizon_slope   = get_env_double("CAM_HORIZON_SLOPE",   0.0),
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
    // Apply the calibrated-horizon shift: image-measured horizon is the ground
    // truth for "ray.z = 0" in world. Shift the input v so that the actual
    // horizon at (u, v_horizon(u)) maps to the principal-point row cy, where
    // the rest of the math expects horizontal rays. With defaults
    // (horizon_v_at_cx == cy, slope == 0) this is a no-op.
    const double v_horizon = config_.horizon_v_at_cx
                           + config_.horizon_slope * (u - config_.cx);
    const double v_corrected = v - (v_horizon - config_.cy);

    auto p_cam     = mat3_vec3(K_inv_, {u, v_corrected, 1.0});
    auto ray_world = normalize3(mat3T_vec3(R_, p_cam));

    // Spherical-Earth ray intersection (maritime ISR convention).
    // Local ENU at camera: x=East, y=North, z=Up. Camera at origin.
    // Effective Earth = sphere of radius R_eff centered directly below
    // camera by (R_eff + h_cam). Intersection altitude is target_alt_m
    // (sea level = 0), so we intersect with sphere of radius
    // (R_eff + target_alt_m) about the same center.
    //
    // Ray (origin O=0, dir r) hits sphere when
    //   (t*r - C)·(t*r - C) = R²
    //   t² − 2·t·(r·C) + (C·C − R²) = 0
    // C = (0, 0, -(R_eff + h_cam)).
    constexpr double kEarthRadiusM = 6371000.0;
    const double R_eff = config_.refraction_k * kEarthRadiusM;
    const double cz    = -(R_eff + config_.altitude_m);
    const double Rs    = R_eff + config_.target_alt_m;

    const double rz = ray_world[2];
    const double b  = -2.0 * rz * cz;            // -2*(r·C); r·C = rz*cz
    const double c  = cz * cz - Rs * Rs;
    const double disc = b * b - 4.0 * c;
    if (disc < 0.0) {
        // Ray genuinely misses the sphere — sky / above horizon / steep up.
        return std::nullopt;
    }

    const double sqrt_disc = std::sqrt(disc);
    const double t1 = (-b - sqrt_disc) * 0.5;
    const double t2 = (-b + sqrt_disc) * 0.5;
    double t;
    if      (t1 > 0.0) t = t1;     // near hit (in front of camera)
    else if (t2 > 0.0) t = t2;     // grazing / numerical edge case
    else return std::nullopt;      // both negative → ray points away from Earth

    const double east    = t * ray_world[0];
    const double north   = t * ray_world[1];
    const double range_m = std::sqrt(east * east + north * north);

    // Local ENU → lat/lon (small-angle; valid for ranges up to ~100 km).
    const double lat = config_.latitude  + north / kMetersPerDegreeLat;
    const double lon = config_.longitude + east  / (kMetersPerDegreeLat * std::cos(deg2rad(config_.latitude)));

    return GeoPoint{
        .latitude  = lat,
        .longitude = lon,
        .range_m   = range_m,
        .east_m    = east,
        .north_m   = north,
    };
}

std::optional<GeoPoint> CameraGeolocator::pixel_to_gps_from_size(double u_center,
                                                                double v_center,
                                                                double bbox_w_px,
                                                                double length_m) const {
    if (bbox_w_px <= 0.5 || length_m <= 0.0) return std::nullopt;

    // Range from angular subtense (small-angle pinhole). fx is in pixels per
    // unit length on the normalized image plane, so an object of physical
    // width L at range R subtends L*fx/R pixels.
    const double range = length_m * config_.fx / bbox_w_px;

    // Bearing from bbox center via the standard ray. We don't need the foot
    // row, but we still want the horizontal direction this pixel points to.
    auto p_cam     = mat3_vec3(K_inv_, {u_center, v_center, 1.0});
    auto ray_world = normalize3(mat3T_vec3(R_, p_cam));

    // Reject rays pointing into the upper hemisphere (sky / above horizon).
    // For maritime targets at sea level, the bbox center can be slightly above
    // horizon due to bbox padding; allow a small positive-z slack.
    if (ray_world[2] > 0.05) return std::nullopt;

    // Project the ray into the horizontal plane to get bearing direction.
    const double hx = ray_world[0];
    const double hy = ray_world[1];
    const double hnorm = std::sqrt(hx * hx + hy * hy);
    if (hnorm < 1e-9) return std::nullopt;

    const double east  = (hx / hnorm) * range;
    const double north = (hy / hnorm) * range;

    const double lat = config_.latitude  + north / kMetersPerDegreeLat;
    const double lon = config_.longitude + east  / (kMetersPerDegreeLat * std::cos(deg2rad(config_.latitude)));

    return GeoPoint{
        .latitude  = lat,
        .longitude = lon,
        .range_m   = range,
        .east_m    = east,
        .north_m   = north,
    };
}
