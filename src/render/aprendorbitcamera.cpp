#include "render/aprendorbitcamera.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cmath>

// Spherical -> Cartesian offset from the orbit target. yaw is measured
// around world Y from +Z toward +X; pitch is elevation above the XZ plane.
// Also used at distance=1 to get just the eye-to-target direction, e.g. for
// deriving the pan basis.
static glm::vec3 orbit_offset(float yaw, float pitch, float distance) {
	float cp = cosf(pitch);
	return glm::vec3(distance * cp * sinf(yaw), distance * sinf(pitch), distance * cp * cosf(yaw));
}

typedef struct aprend_orbit_camera_t {
	float yaw{0.0f};
	float pitch{0.0f};
	float distance{1.0f};
	glm::dvec3 target{0.0};

	float orbit_sensitivity{0.005f};
	float pan_sensitivity{0.0025f};
	float zoom_step{0.9f};
	float pitch_limit{1.5533f};
	float min_distance{0.01f};
	float max_distance{1000.0f};

	glm::dvec3 eye_world{0.0}; // recomputed by aprend_orbit_camera_update
} aprend_orbit_camera_t;

static void recompute_eye(aprend_orbit_camera_t *cam) {
	cam->eye_world = cam->target + glm::dvec3(orbit_offset(cam->yaw, cam->pitch, cam->distance));
}

extern "C" {

aprend_orbit_camera aprend_orbit_camera_create(const aprend_orbit_camera_desc *desc) {
	if (!desc)
		return nullptr;
	aprend_orbit_camera_t *result = (aprend_orbit_camera_t *)malloc(sizeof(aprend_orbit_camera_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_orbit_camera_t();

	result->yaw = desc->yaw;
	result->pitch = desc->pitch;
	result->distance = desc->distance;
	result->target = to_glm(desc->target);

	result->orbit_sensitivity = desc->orbit_sensitivity;
	result->pan_sensitivity = desc->pan_sensitivity;
	result->zoom_step = desc->zoom_step;
	result->pitch_limit = desc->pitch_limit;
	result->min_distance = desc->min_distance;
	result->max_distance = desc->max_distance;

	recompute_eye(result);
	return result;
}

void aprend_orbit_camera_destroy(aprend_orbit_camera cam) {
	if (!cam)
		return;
	cam->~aprend_orbit_camera_t();
	free(cam);
}

void aprend_orbit_camera_update(aprend_orbit_camera cam, const aprend_orbit_camera_input *input) {
	if (!cam || !input)
		return;

	if (input->orbit_dx != 0.0f || input->orbit_dy != 0.0f) {
		cam->yaw -= input->orbit_dx * cam->orbit_sensitivity;
		cam->pitch = std::clamp(cam->pitch - input->orbit_dy * cam->orbit_sensitivity, -cam->pitch_limit, cam->pitch_limit);
	}

	if (input->pan_dx != 0.0f || input->pan_dy != 0.0f) {
		// Pan basis from the current orbit orientation alone (unit-distance
		// eye offset), not the eye/target position -- same spherical
		// direction used for the eye itself.
		glm::vec3 to_eye = orbit_offset(cam->yaw, cam->pitch, 1.0f);
		glm::vec3 forward = -to_eye;
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
		glm::vec3 up = glm::cross(right, forward);

		// Scaled by distance so a pixel of drag pans by a consistent-feeling
		// amount whether zoomed in or out.
		float pan_scale = cam->distance * cam->pan_sensitivity;
		glm::vec3 pan_delta = (-right * input->pan_dx + up * input->pan_dy) * pan_scale;
		cam->target += glm::dvec3(pan_delta);
	}

	if (input->zoom_ticks != 0.0f) {
		float distance = cam->distance * powf(cam->zoom_step, input->zoom_ticks);
		cam->distance = std::clamp(distance, cam->min_distance, cam->max_distance);
	}

	recompute_eye(cam);
}

ApriDVec3 aprend_orbit_camera_get_eye_world(aprend_orbit_camera cam) {
	if (!cam)
		return ApriDVec3{0.0, 0.0, 0.0};
	return from_glm(cam->eye_world);
}

ApriDVec3 aprend_orbit_camera_get_target(aprend_orbit_camera cam) {
	if (!cam)
		return ApriDVec3{0.0, 0.0, 0.0};
	return from_glm(cam->target);
}

ApriVec3 aprend_orbit_camera_get_forward(aprend_orbit_camera cam) {
	if (!cam)
		return ApriVec3{0.0f, 0.0f, -1.0f};
	glm::vec3 eye = glm::vec3(cam->eye_world);
	glm::vec3 target = glm::vec3(cam->target);
	return from_glm(glm::normalize(target - eye));
}

float aprend_orbit_camera_get_distance(aprend_orbit_camera cam) {
	return cam ? cam->distance : 0.0f;
}

} // extern "C"
