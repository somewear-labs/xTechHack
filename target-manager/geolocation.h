#ifndef TARGET_MANAGER_GEOLOCATION_H
#define TARGET_MANAGER_GEOLOCATION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double latitude;
    double longitude;
    double altitude_m;
    double heading_deg;
} tm_camera_pose_t;

typedef struct {
    double latitude;
    double longitude;
    double altitude_m;
} tm_geo_point_t;

void tm_geo_set_camera_pose(const tm_camera_pose_t *pose);

int tm_geo_project_world_foot(const float xyz[3], tm_geo_point_t *out);

#ifdef __cplusplus
}
#endif

#endif
