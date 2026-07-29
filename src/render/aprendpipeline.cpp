
#include "render/aprendpipeline.h"
#include "aprend_internal.hpp"
#include <spudfiles.h>

// Maps an Aprend vertex-attribute element type onto the SpudGPU format used
// to describe it in a vertex_attribute_desc.
static SPUDGPU_FORMAT aprend_buffer_element_type_to_format(APREND_BUFFER_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_BUFFER_ELEMENT_TYPE_FLOAT:
		return SPUDGPU_FORMAT_R32_FLOAT;
	case APREND_BUFFER_ELEMENT_TYPE_VEC2:
		return SPUDGPU_FORMAT_R32G32_FLOAT;
	case APREND_BUFFER_ELEMENT_TYPE_VEC3:
		return SPUDGPU_FORMAT_R32G32B32_FLOAT;
	case APREND_BUFFER_ELEMENT_TYPE_VEC4:
		return SPUDGPU_FORMAT_R32G32B32A32_FLOAT;
	case APREND_BUFFER_ELEMENT_TYPE_INT:
		return SPUDGPU_FORMAT_R32_SINT;
	case APREND_BUFFER_ELEMENT_TYPE_INT2:
		return SPUDGPU_FORMAT_R32G32_SINT;
	case APREND_BUFFER_ELEMENT_TYPE_INT3:
		return SPUDGPU_FORMAT_R32G32B32_SINT;
	case APREND_BUFFER_ELEMENT_TYPE_INT4:
		return SPUDGPU_FORMAT_R32G32B32A32_SINT;
	case APREND_BUFFER_ELEMENT_TYPE_UNIQUE_ID:
		return SPUDGPU_FORMAT_R32_UINT; // treated as uint32 behind the scenes
	default:
		return SPUDGPU_FORMAT_UNKNOWN;
	}
}

aprend_shader_t::~aprend_shader_t() { spudgpu_destroy_shader_module(this->shader_module); }
aprend_graphics_pipeline_t::~aprend_graphics_pipeline_t() { spudgpu_destroy_shader_pipeline(this->pipeline); }

extern "C" {

aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage) {
	if (!instance || !filename)
		return nullptr;

	switch (shader_stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		break;
	default:
		printf("apricot: only vertex and fragment shaders are supported now!\n");
		return nullptr;
	}

	if (!sfs_file_exists(filename))
		return nullptr;

	aprend_shader_t *result = (aprend_shader_t *)malloc(sizeof(aprend_shader_t));
	if (!result)
		return nullptr;
	result           = new (result) aprend_shader_t();
	result->instance = instance;

	SPUDRESULT sr      = SPUD_SUCCESS;
	uint8_t *data      = nullptr;
	uint64_t data_size = 0;

	SFS_FILE_OPEN_ATTRIBUTES ofa = {};
	ofa.str_file_path            = filename;
	ofa.access_mode              = SFS_EFILE_ACCESS_MODE_READ;
	sfs_file file;
	sr = sfs_file_open(ofa, &file);
	if (SPUDFAIL(sr))
		goto failedattempt;
	sr = sfs_file_get_size(file, &data_size);
	if (SPUDFAIL(sr)) {
		sfs_file_release(file);
		goto failedattempt;
	}
	data = (uint8_t *)malloc(data_size);
	sr   = sfs_file_read(file, data, data_size);
	if (SPUDFAIL(sr)) {
		sfs_file_release(file);
		goto failedattempt;
	}
	sr = sfs_file_release(file);
	if (SPUDFAIL(sr))
		goto failedattempt;

	// Create the SpudGPU shader module; the raw file bytes aren't needed once
	// SpudGPU has consumed them into its own copy.
	{
		spudgpu_shader_module_desc smd = {};
		smd.stage                      = shader_stage;
		smd.spirv_code                 = data;
		smd.spirv_size                 = data_size;
#if _DEBUG
		smd.debug_name = filename;
#endif

		sr = spudgpu_create_shader_module(instance->desc.device, &smd, &result->shader_module);
	}
	free(data);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_shader_read_from_file_spirv failed ('%s'): %s\n", filename, spudresult_str(sr));
	result->~aprend_shader_t();
	free(result);
	return nullptr;
}
void aprend_shader_destroy(aprend_shader shader) {
	if (shader) {
		shader->~aprend_shader_t();
		free(shader);
	}
}

aprend_graphics_pipeline aprend_graphics_pipeline_create(
    aprend_instance instance,
    aprend_graphics_pipeline_desc desc) {
	if (!instance || !desc.vertex_shader || !desc.fragment_shader)
		return nullptr;
	if (desc._vertex_layout.count > SPUDGPU_MAX_VERTEX_ATTRIBUTES)
		return nullptr;

	aprend_graphics_pipeline_t *result = (aprend_graphics_pipeline_t *)malloc(sizeof(aprend_graphics_pipeline_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_graphics_pipeline_t();

	result->desc     = desc;
	result->instance = instance;

	spudgpu_shader_pipeline_desc pd{};
	pd.vertex_module        = desc.vertex_shader->shader_module;
	pd.vertex_entry_point   = "main";
	pd.fragment_module      = desc.fragment_shader->shader_module;
	pd.fragment_entry_point = "main";

	pd.vertex_bindings[0].binding      = 0;
	pd.vertex_bindings[0].stride       = aprend_buffer_layout_get_total_size(&desc._vertex_layout);
	pd.vertex_bindings[0].per_instance = false;
	pd.vertex_binding_count            = desc._vertex_layout.count ? 1 : 0;

	for (uint32_t i = 0; i < desc._vertex_layout.count; ++i) {
		const aprend_buffer_element &el  = desc._vertex_layout.elements[i];
		pd.vertex_attributes[i].location = i;
		pd.vertex_attributes[i].binding  = 0;
		pd.vertex_attributes[i].format   = aprend_buffer_element_type_to_format(el.type);
		pd.vertex_attributes[i].offset   = el.offset;
	}
	pd.vertex_attribute_count = desc._vertex_layout.count;

	pd.primitive_topology = desc._topology;
	pd.cull_mode          = desc._backface_culling ? SPUDGPU_CULL_MODE_BACK : SPUDGPU_CULL_MODE_NONE;
	pd.front_face_ccw     = false; // GLM Y-flip inverts winding: world-CCW becomes screen-CW (see aprendscene.cpp's default pipeline)
	pd.wireframe          = desc._wireframe;
	pd.depth_test_enable  = desc._depth_test;
	pd.depth_write_enable = desc._depth_write;
	pd.depth_compare_op   = SPUDGPU_COMPARE_OP_LESS;

	pd.color_attachment_format = desc.color_attachment_format;
	pd.depth_format            = desc.depth_format;
#if _DEBUG
	pd.debug_name = desc._debug_name;
#endif

	SPUDRESULT sr = spudgpu_create_shader_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_graphics_pipeline_create failed: %s\n", spudresult_str(sr));
		result->~aprend_graphics_pipeline_t();
		free(result);
		return nullptr;
	}

	return result;
}
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p) {
	if (p) {
		p->~aprend_graphics_pipeline_t();
		free(p);
	}
}
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p) { return p ? p->desc : aprend_graphics_pipeline_desc{}; }

} // Extern "C"
