#ifndef TARGET_MANAGER_GEOLOCATION_HPP
#define TARGET_MANAGER_GEOLOCATION_HPP

#include <array>
#include <optional>

struct CameraConfig {
    double fx;
    double fy;
    double cx;
    double cy;
    double heading_deg;
    double pitch_deg;
    double roll_deg;
    double altitude_m;
    double latitude;
    double longitude;
};

CameraConfig load_camera_config_from_env();

struct GeoPoint {
    double latitude;
    double longitude;
    double range_m;
    double east_m;
    double north_m;
};

class CameraGeolocator {
public:
    explicit CameraGeolocator(const CameraConfig &config);

    std::optional<GeoPoint> pixel_to_gps(double u, double v) const;

    const CameraConfig &config() const { return config_; }

private:
    CameraConfig config_;
    std::array<double, 9> K_inv_;
    std::array<double, 9> R_;
};

#endif
