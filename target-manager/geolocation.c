#include "geolocation.h"

#include <string.h>

static tm_camera_pose_t g_camera_pose;

void tm_geo_set_camera_pose(const tm_camera_pose_t *pose) {
    if (pose) g_camera_pose = *pose;
}

int tm_geo_project_world_foot(const float xyz[3], tm_geo_point_t *out) {
    (void)xyz;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    return 0;
}
