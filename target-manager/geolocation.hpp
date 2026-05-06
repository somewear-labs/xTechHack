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
    // Effective Earth radius scale factor for atmospheric refraction.
    // 1.0 = geometric (no refraction); 4/3 ≈ 1.333 = standard atmosphere
    // (Snell's law averaged over typical lapse rates). Maritime ISR convention.
    double refraction_k;
    // Target altitude above the WGS-84 ellipsoid in meters. 0 for sea level
    // (ships). Set higher for ground vehicles on terrain — though for true
    // terrain you'd want a DEM, not a single constant.
    double target_alt_m;
    // Calibrated horizon line in image space: v_horizon(u) = v_at_cx + slope*(u-cx).
    // Defaults to (cy, 0) — i.e. "horizon at principal point, no roll" — which
    // matches the geometric expectation for pitch=0 well-leveled cameras.
    // In practice, mount errors and lens nonlinearity put the real horizon a
    // few pixels off. Calibrate once per install (a Sobel+Hough fit on a
    // clean frame is enough). Values are pre-rectification image coordinates.
    double horizon_v_at_cx;
    double horizon_slope;
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

    // Range from angular subtense, given a real-world size prior. Bearing is
    // taken from u_center via the standard pinhole ray; range is solved from
    //   range ≈ length_m * fx / bbox_w_px
    // (small-angle pinhole). Robust at horizon-grazing pitch where foot-pixel
    // geolocation is ill-conditioned, because it doesn't depend on the bbox
    // foot row at all. v_center is used only to disambiguate above-vs-below
    // horizon (rays pointing strictly up return nullopt).
    //
    // Caller picks bbox_w_px = bounding-box pixel width (horizontal extent).
    // For side-view maritime targets that's a reasonable length proxy; for
    // bow-on views the result will overestimate range. Length_m comes from
    // the VLM size prior (class median).
    std::optional<GeoPoint> pixel_to_gps_from_size(double u_center,
                                                  double v_center,
                                                  double bbox_w_px,
                                                  double length_m) const;

    const CameraConfig &config() const { return config_; }

private:
    CameraConfig config_;
    std::array<double, 9> K_inv_;
    std::array<double, 9> R_;
};

#endif
