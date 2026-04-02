
#include "engine3d.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <obs-module.h>

/* ---- Camera ---- */

void vjlink_camera_set_orbit(struct vjlink_camera *cam, float yaw,
                             float pitch, float distance)
{
	cam->mode = VJLINK_CAM_ORBIT;
	cam->yaw = yaw;
	cam->pitch = pitch;
	cam->distance = distance;
}

void vjlink_camera_set_target(struct vjlink_camera *cam,
                              float x, float y, float z)
{
	vec3_set(&cam->target, x, y, z);
}

void vjlink_camera_set_fov(struct vjlink_camera *cam, float fov_degrees)
{
	cam->fov = fov_degrees;
}

void vjlink_camera_update(struct vjlink_camera *cam, float aspect, float dt)
{
	cam->aspect_ratio = aspect;

	/* Auto-rotation */
	if (cam->auto_rotate_speed != 0.0f)
		cam->yaw += cam->auto_rotate_speed * dt;

	if (cam->mode == VJLINK_CAM_ORBIT) {
		/* Clamp pitch to avoid gimbal lock */
		if (cam->pitch > 1.5f) cam->pitch = 1.5f;
		if (cam->pitch < -1.5f) cam->pitch = -1.5f;

		/* Compute camera position from spherical coords */
		float cos_pitch = cosf(cam->pitch);
		cam->position.x = cam->target.x + cam->distance * cos_pitch * sinf(cam->yaw);
		cam->position.y = cam->target.y + cam->distance * sinf(cam->pitch);
		cam->position.z = cam->target.z + cam->distance * cos_pitch * cosf(cam->yaw);

		vec3_set(&cam->up, 0.0f, 1.0f, 0.0f);
	}

	/* Compute view matrix (look-at) */
	struct vec3 eye = cam->position;
	struct vec3 center = (cam->mode == VJLINK_CAM_ORBIT)
		? cam->target : cam->forward;
	struct vec3 up = cam->up;

	/* Manual look-at matrix construction */
	struct vec3 f, s, u;
	vec3_sub(&f, &center, &eye);
	vec3_norm(&f, &f);

	vec3_cross(&s, &f, &up);
	vec3_norm(&s, &s);

	vec3_cross(&u, &s, &f);

	memset(&cam->view, 0, sizeof(cam->view));
	cam->view.x.x = s.x;  cam->view.y.x = s.y;  cam->view.z.x = s.z;
	cam->view.x.y = u.x;  cam->view.y.y = u.y;  cam->view.z.y = u.z;
	cam->view.x.z = -f.x; cam->view.y.z = -f.y; cam->view.z.z = -f.z;
	cam->view.t.x = -vec3_dot(&s, &eye);
	cam->view.t.y = -vec3_dot(&u, &eye);
	cam->view.t.z = vec3_dot(&f, &eye);
	cam->view.t.w = 1.0f;

	/* Compute perspective projection matrix */
	float fov_rad = cam->fov * (float)M_PI / 180.0f;
	float tan_half_fov = tanf(fov_rad * 0.5f);
	float near = cam->near_plane;
	float far = cam->far_plane;

	memset(&cam->proj, 0, sizeof(cam->proj));
	cam->proj.x.x = 1.0f / (aspect * tan_half_fov);
	cam->proj.y.y = 1.0f / tan_half_fov;
	cam->proj.z.z = -(far + near) / (far - near);
	cam->proj.z.w = -1.0f;
	cam->proj.t.z = -(2.0f * far * near) / (far - near);

	/* Combined view-projection */
	matrix4_mul(&cam->view_proj, &cam->view, &cam->proj);
}

void vjlink_camera_get_view_proj(struct vjlink_camera *cam,
                                 struct matrix4 *out)
{
	*out = cam->view_proj;
}

void vjlink_camera_get_position(struct vjlink_camera *cam,
                                struct vec3 *out)
{
	*out = cam->position;
}

/* ---- Mesh Generation ---- */

/* Temporary vertex data for mesh building */
struct build_vertex {
	float pos[3];
	float norm[3];
	float uv[2];
};

static gs_vertbuffer_t *create_vb(struct build_vertex *verts, uint32_t count)
{
	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = count;
	vbd->points = bmalloc(sizeof(struct vec3) * count);
	vbd->normals = bmalloc(sizeof(struct vec3) * count);
	vbd->num_tex = 1;
	vbd->tvarray = bmalloc(sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 2;
	vbd->tvarray[0].array = bmalloc(sizeof(struct vec2) * count);

	for (uint32_t i = 0; i < count; i++) {
		struct vec3 *p = (struct vec3 *)vbd->points + i;
		struct vec3 *n = (struct vec3 *)vbd->normals + i;
		struct vec2 *t = (struct vec2 *)vbd->tvarray[0].array + i;

		vec3_set(p, verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
		vec3_set(n, verts[i].norm[0], verts[i].norm[1], verts[i].norm[2]);
		vec2_set(t, verts[i].uv[0], verts[i].uv[1]);
	}

	return gs_vertexbuffer_create(vbd, GS_DYNAMIC);
}

static void generate_cube(struct vjlink_mesh *mesh)
{
	/* 24 vertices (4 per face for proper normals), 36 indices */
	static const float cube_v[] = {
		/* Front face (z=+0.5) */
		-0.5f,-0.5f, 0.5f,  0,0,1,  0,0,
		 0.5f,-0.5f, 0.5f,  0,0,1,  1,0,
		 0.5f, 0.5f, 0.5f,  0,0,1,  1,1,
		-0.5f, 0.5f, 0.5f,  0,0,1,  0,1,
		/* Back face (z=-0.5) */
		 0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,
		-0.5f,-0.5f,-0.5f,  0,0,-1,  1,0,
		-0.5f, 0.5f,-0.5f,  0,0,-1,  1,1,
		 0.5f, 0.5f,-0.5f,  0,0,-1,  0,1,
		/* Right face (x=+0.5) */
		 0.5f,-0.5f, 0.5f,  1,0,0,  0,0,
		 0.5f,-0.5f,-0.5f,  1,0,0,  1,0,
		 0.5f, 0.5f,-0.5f,  1,0,0,  1,1,
		 0.5f, 0.5f, 0.5f,  1,0,0,  0,1,
		/* Left face (x=-0.5) */
		-0.5f,-0.5f,-0.5f,  -1,0,0,  0,0,
		-0.5f,-0.5f, 0.5f,  -1,0,0,  1,0,
		-0.5f, 0.5f, 0.5f,  -1,0,0,  1,1,
		-0.5f, 0.5f,-0.5f,  -1,0,0,  0,1,
		/* Top face (y=+0.5) */
		-0.5f, 0.5f, 0.5f,  0,1,0,  0,0,
		 0.5f, 0.5f, 0.5f,  0,1,0,  1,0,
		 0.5f, 0.5f,-0.5f,  0,1,0,  1,1,
		-0.5f, 0.5f,-0.5f,  0,1,0,  0,1,
		/* Bottom face (y=-0.5) */
		-0.5f,-0.5f,-0.5f,  0,-1,0,  0,0,
		 0.5f,-0.5f,-0.5f,  0,-1,0,  1,0,
		 0.5f,-0.5f, 0.5f,  0,-1,0,  1,1,
		-0.5f,-0.5f, 0.5f,  0,-1,0,  0,1,
	};

	uint32_t nv = 24;
	struct build_vertex *verts = malloc(sizeof(struct build_vertex) * nv);
	for (uint32_t i = 0; i < nv; i++) {
		const float *v = &cube_v[i * 8];
		verts[i].pos[0] = v[0]; verts[i].pos[1] = v[1]; verts[i].pos[2] = v[2];
		verts[i].norm[0] = v[3]; verts[i].norm[1] = v[4]; verts[i].norm[2] = v[5];
		verts[i].uv[0] = v[6]; verts[i].uv[1] = v[7];
	}

	uint32_t indices[] = {
		0,1,2, 0,2,3,     4,5,6, 4,6,7,
		8,9,10, 8,10,11,   12,13,14, 12,14,15,
		16,17,18, 16,18,19, 20,21,22, 20,22,23
	};

	mesh->vb = create_vb(verts, nv);
	mesh->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, 36, 0);
	mesh->vertex_count = nv;
	mesh->index_count = 36;
	mesh->created = true;

	free(verts);
}

static void generate_sphere(struct vjlink_mesh *mesh)
{
	int slices = 32;
	int stacks = 16;
	uint32_t nv = (slices + 1) * (stacks + 1);
	uint32_t ni = slices * stacks * 6;

	struct build_vertex *verts = malloc(sizeof(struct build_vertex) * nv);
	uint32_t *indices = malloc(sizeof(uint32_t) * ni);

	uint32_t vi = 0;
	for (int j = 0; j <= stacks; j++) {
		float v = (float)j / stacks;
		float phi = v * (float)M_PI;
		for (int i = 0; i <= slices; i++) {
			float u = (float)i / slices;
			float theta = u * 2.0f * (float)M_PI;

			float x = cosf(theta) * sinf(phi);
			float y = cosf(phi);
			float z = sinf(theta) * sinf(phi);

			verts[vi].pos[0] = x * 0.5f;
			verts[vi].pos[1] = y * 0.5f;
			verts[vi].pos[2] = z * 0.5f;
			verts[vi].norm[0] = x;
			verts[vi].norm[1] = y;
			verts[vi].norm[2] = z;
			verts[vi].uv[0] = u;
			verts[vi].uv[1] = v;
			vi++;
		}
	}

	uint32_t ii = 0;
	for (int j = 0; j < stacks; j++) {
		for (int i = 0; i < slices; i++) {
			uint32_t a = j * (slices + 1) + i;
			uint32_t b = a + slices + 1;

			indices[ii++] = a;
			indices[ii++] = b;
			indices[ii++] = a + 1;

			indices[ii++] = a + 1;
			indices[ii++] = b;
			indices[ii++] = b + 1;
		}
	}

	mesh->vb = create_vb(verts, nv);
	mesh->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, ni, 0);
	mesh->vertex_count = nv;
	mesh->index_count = ni;
	mesh->created = true;

	free(verts);
	free(indices);
}

static void generate_torus(struct vjlink_mesh *mesh)
{
	int major_seg = 32;
	int minor_seg = 16;
	float major_r = 0.35f;
	float minor_r = 0.15f;

	uint32_t nv = (major_seg + 1) * (minor_seg + 1);
	uint32_t ni = major_seg * minor_seg * 6;

	struct build_vertex *verts = malloc(sizeof(struct build_vertex) * nv);
	uint32_t *indices = malloc(sizeof(uint32_t) * ni);

	uint32_t vi = 0;
	for (int j = 0; j <= minor_seg; j++) {
		float v = (float)j / minor_seg;
		float phi = v * 2.0f * (float)M_PI;
		for (int i = 0; i <= major_seg; i++) {
			float u = (float)i / major_seg;
			float theta = u * 2.0f * (float)M_PI;

			float cx = major_r * cosf(theta);
			float cz = major_r * sinf(theta);

			float x = (major_r + minor_r * cosf(phi)) * cosf(theta);
			float y = minor_r * sinf(phi);
			float z = (major_r + minor_r * cosf(phi)) * sinf(theta);

			float nx = cosf(phi) * cosf(theta);
			float ny = sinf(phi);
			float nz = cosf(phi) * sinf(theta);

			verts[vi].pos[0] = x;
			verts[vi].pos[1] = y;
			verts[vi].pos[2] = z;
			verts[vi].norm[0] = nx;
			verts[vi].norm[1] = ny;
			verts[vi].norm[2] = nz;
			verts[vi].uv[0] = u;
			verts[vi].uv[1] = v;
			vi++;
		}
	}

	uint32_t ii = 0;
	for (int j = 0; j < minor_seg; j++) {
		for (int i = 0; i < major_seg; i++) {
			uint32_t a = j * (major_seg + 1) + i;
			uint32_t b = a + major_seg + 1;

			indices[ii++] = a;
			indices[ii++] = b;
			indices[ii++] = a + 1;
			indices[ii++] = a + 1;
			indices[ii++] = b;
			indices[ii++] = b + 1;
		}
	}

	mesh->vb = create_vb(verts, nv);
	mesh->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, ni, 0);
	mesh->vertex_count = nv;
	mesh->index_count = ni;
	mesh->created = true;

	free(verts);
	free(indices);
}

static void generate_plane(struct vjlink_mesh *mesh)
{
	struct build_vertex verts[4] = {
		{{-0.5f, 0, -0.5f}, {0,1,0}, {0,0}},
		{{ 0.5f, 0, -0.5f}, {0,1,0}, {1,0}},
		{{ 0.5f, 0,  0.5f}, {0,1,0}, {1,1}},
		{{-0.5f, 0,  0.5f}, {0,1,0}, {0,1}},
	};
	uint32_t indices[] = {0,1,2, 0,2,3};

	mesh->vb = create_vb(verts, 4);
	mesh->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, 6, 0);
	mesh->vertex_count = 4;
	mesh->index_count = 6;
	mesh->created = true;
}

static void generate_cylinder(struct vjlink_mesh *mesh)
{
	int slices = 24;
	uint32_t nv = (slices + 1) * 2;
	uint32_t ni = slices * 6;

	struct build_vertex *verts = malloc(sizeof(struct build_vertex) * nv);
	uint32_t *indices = malloc(sizeof(uint32_t) * ni);

	/* Side vertices */
	for (int i = 0; i <= slices; i++) {
		float u = (float)i / slices;
		float theta = u * 2.0f * (float)M_PI;
		float nx = cosf(theta);
		float nz = sinf(theta);

		/* Bottom ring */
		verts[i].pos[0] = nx * 0.5f;
		verts[i].pos[1] = -0.5f;
		verts[i].pos[2] = nz * 0.5f;
		verts[i].norm[0] = nx;
		verts[i].norm[1] = 0;
		verts[i].norm[2] = nz;
		verts[i].uv[0] = u;
		verts[i].uv[1] = 0;

		/* Top ring */
		int ti = slices + 1 + i;
		verts[ti].pos[0] = nx * 0.5f;
		verts[ti].pos[1] = 0.5f;
		verts[ti].pos[2] = nz * 0.5f;
		verts[ti].norm[0] = nx;
		verts[ti].norm[1] = 0;
		verts[ti].norm[2] = nz;
		verts[ti].uv[0] = u;
		verts[ti].uv[1] = 1;
	}

	uint32_t ii = 0;
	for (int i = 0; i < slices; i++) {
		uint32_t a = i;
		uint32_t b = i + slices + 1;
		indices[ii++] = a;
		indices[ii++] = b;
		indices[ii++] = a + 1;
		indices[ii++] = a + 1;
		indices[ii++] = b;
		indices[ii++] = b + 1;
	}

	mesh->vb = create_vb(verts, nv);
	mesh->ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, ni, 0);
	mesh->vertex_count = nv;
	mesh->index_count = ni;
	mesh->created = true;

	free(verts);
	free(indices);
}

/* ---- Engine3D ---- */

struct vjlink_engine3d *vjlink_engine3d_create(void)
{
	struct vjlink_engine3d *engine = calloc(1, sizeof(*engine));
	if (!engine)
		return NULL;

	/* Default camera */
	engine->camera.mode = VJLINK_CAM_ORBIT;
	engine->camera.yaw = 0.0f;
	engine->camera.pitch = 0.3f;
	engine->camera.distance = 3.0f;
	vec3_set(&engine->camera.target, 0, 0, 0);
	vec3_set(&engine->camera.up, 0, 1, 0);
	engine->camera.fov = 60.0f;
	engine->camera.near_plane = 0.1f;
	engine->camera.far_plane = 100.0f;
	engine->camera.aspect_ratio = 16.0f / 9.0f;
	engine->camera.auto_rotate_speed = 0.3f;

	/* Default light */
	vec3_set(&engine->light.direction, -0.5f, -1.0f, -0.3f);
	vec3_norm(&engine->light.direction, &engine->light.direction);
	vec4_set(&engine->light.color, 1.0f, 0.95f, 0.9f, 1.0f);
	engine->light.ambient = 0.15f;
	engine->light.specular_power = 32.0f;

	engine->initialized = true;
	blog(LOG_INFO, "[VJLink] 3D engine created");
	return engine;
}

void vjlink_engine3d_destroy(struct vjlink_engine3d *engine)
{
	if (!engine)
		return;

	for (int i = 0; i < VJLINK_MESH_COUNT; i++) {
		if (engine->meshes[i].vb)
			gs_vertexbuffer_destroy(engine->meshes[i].vb);
		if (engine->meshes[i].ib)
			gs_indexbuffer_destroy(engine->meshes[i].ib);
	}

	free(engine);
	blog(LOG_INFO, "[VJLink] 3D engine destroyed");
}

void vjlink_engine3d_create_meshes(struct vjlink_engine3d *engine)
{
	if (!engine)
		return;

	if (!engine->meshes[VJLINK_MESH_CUBE].created)
		generate_cube(&engine->meshes[VJLINK_MESH_CUBE]);
	if (!engine->meshes[VJLINK_MESH_SPHERE].created)
		generate_sphere(&engine->meshes[VJLINK_MESH_SPHERE]);
	if (!engine->meshes[VJLINK_MESH_TORUS].created)
		generate_torus(&engine->meshes[VJLINK_MESH_TORUS]);
	if (!engine->meshes[VJLINK_MESH_PLANE].created)
		generate_plane(&engine->meshes[VJLINK_MESH_PLANE]);
	if (!engine->meshes[VJLINK_MESH_CYLINDER].created)
		generate_cylinder(&engine->meshes[VJLINK_MESH_CYLINDER]);

	blog(LOG_INFO, "[VJLink] Procedural meshes generated");
}

struct vjlink_mesh *vjlink_engine3d_get_mesh(struct vjlink_engine3d *engine,
                                             enum vjlink_mesh_type type)
{
	if (!engine || type >= VJLINK_MESH_COUNT)
		return NULL;
	return &engine->meshes[type];
}

void vjlink_mesh_draw(struct vjlink_mesh *mesh)
{
	if (!mesh || !mesh->vb)
		return;

	gs_load_vertexbuffer(mesh->vb);
	if (mesh->ib) {
		gs_load_indexbuffer(mesh->ib);
		gs_draw(GS_TRIS, 0, mesh->index_count);
	} else {
		gs_draw(GS_TRIS, 0, mesh->vertex_count);
	}
}

void vjlink_light_set_direction(struct vjlink_light *light,
                                float x, float y, float z)
{
	vec3_set(&light->direction, x, y, z);
	vec3_norm(&light->direction, &light->direction);
}

void vjlink_light_set_color(struct vjlink_light *light,
                            float r, float g, float b, float intensity)
{
	vec4_set(&light->color, r * intensity, g * intensity,
	         b * intensity, 1.0f);
}
