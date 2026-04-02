#pragma once

#include "vjlink_context.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum vertices per mesh */
#define VJLINK_MAX_VERTICES  65536
#define VJLINK_MAX_INDICES   196608

/* Mesh primitive types */
enum vjlink_mesh_type {
	VJLINK_MESH_CUBE = 0,
	VJLINK_MESH_SPHERE,
	VJLINK_MESH_TORUS,
	VJLINK_MESH_PLANE,
	VJLINK_MESH_CYLINDER,
	VJLINK_MESH_ICOSPHERE,
	VJLINK_MESH_COUNT
};

/* Camera modes */
enum vjlink_camera_mode {
	VJLINK_CAM_ORBIT = 0,
	VJLINK_CAM_FREE,
};

/* Camera state */
struct vjlink_camera {
	enum vjlink_camera_mode mode;

	/* Orbit camera */
	float yaw;         /* radians */
	float pitch;       /* radians */
	float distance;    /* distance from target */
	struct vec3 target; /* look-at target */

	/* Free camera */
	struct vec3 position;
	struct vec3 forward;
	struct vec3 up;

	/* Projection */
	float fov;          /* field of view in degrees */
	float near_plane;
	float far_plane;
	float aspect_ratio;

	/* Auto-rotation */
	float auto_rotate_speed;

	/* Computed matrices */
	struct matrix4 view;
	struct matrix4 proj;
	struct matrix4 view_proj;
};

/* Mesh data */
struct vjlink_mesh {
	gs_vertbuffer_t *vb;
	gs_indexbuffer_t *ib;
	uint32_t vertex_count;
	uint32_t index_count;
	bool created;
};

/* Directional light */
struct vjlink_light {
	struct vec3 direction;
	struct vec4 color;
	float ambient;
	float specular_power;
};

/* 3D Engine */
struct vjlink_engine3d {
	struct vjlink_camera camera;
	struct vjlink_light  light;
	struct vjlink_mesh   meshes[VJLINK_MESH_COUNT];
	bool initialized;
};

/* Create / destroy */
struct vjlink_engine3d *vjlink_engine3d_create(void);
void vjlink_engine3d_destroy(struct vjlink_engine3d *engine);

/* Camera control */
void vjlink_camera_set_orbit(struct vjlink_camera *cam, float yaw,
                             float pitch, float distance);
void vjlink_camera_set_target(struct vjlink_camera *cam,
                              float x, float y, float z);
void vjlink_camera_set_fov(struct vjlink_camera *cam, float fov_degrees);
void vjlink_camera_update(struct vjlink_camera *cam, float aspect,
                          float dt);

/* Get computed matrices for shader uniforms */
void vjlink_camera_get_view_proj(struct vjlink_camera *cam,
                                 struct matrix4 *out);
void vjlink_camera_get_position(struct vjlink_camera *cam,
                                struct vec3 *out);

/* Generate procedural meshes (call from graphics thread) */
void vjlink_engine3d_create_meshes(struct vjlink_engine3d *engine);

/* Get mesh for drawing */
struct vjlink_mesh *vjlink_engine3d_get_mesh(struct vjlink_engine3d *engine,
                                             enum vjlink_mesh_type type);

/* Draw a mesh with current effect (must bind effect first) */
void vjlink_mesh_draw(struct vjlink_mesh *mesh);

/* Light control */
void vjlink_light_set_direction(struct vjlink_light *light,
                                float x, float y, float z);
void vjlink_light_set_color(struct vjlink_light *light,
                            float r, float g, float b, float intensity);

#ifdef __cplusplus
}
#endif
