
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
	default:
		return SPUDGPU_FORMAT_UNKNOWN;
	}
}

aprend_shader_t::~aprend_shader_t() { spudgpu_destroy_shader_module(this->shader_module); }
aprend_graphics_pipeline_t::~aprend_graphics_pipeline_t() {
	// Pipeline first: it was created against the layout.
	spudgpu_destroy_shader_pipeline(this->pipeline);
	spudgpu_destroy_descriptor_set_layout(this->uniform_layout);
}
aprend_uniform_set_t::~aprend_uniform_set_t() { spudgpu_destroy_descriptor_pool(this->pool); }

static bool aprend_uniform_bindings_valid(const aprend_graphics_pipeline_desc &desc) {
	if (desc._uniform_binding_count > APREND_MAX_UNIFORM_BINDINGS)
		return false;
	for (uint32_t i = 0; i < desc._uniform_binding_count; ++i)
		for (uint32_t j = i + 1; j < desc._uniform_binding_count; ++j)
			if (desc._uniform_bindings[i]._binding == desc._uniform_bindings[j]._binding)
				return false;
	return true;
}

static bool aprend_shader_stage_supported(SPUDGPU_SHADER_STAGE shader_stage) {
	switch (shader_stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return true;
	default:
		printf("apricot: only vertex and fragment shaders are supported now!\n");
		return false;
	}
}

// SpudGPU copies the SPIR-V into its own shader module, so [code] only has to
// live for this call.
static SPUDRESULT aprend_shader_create_module(
    aprend_shader_t *shader,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    const char *debug_name) {
	spudgpu_shader_module_desc smd = {};
	smd.stage                      = shader_stage;
	smd.spirv_code                 = code;
	smd.spirv_size                 = size;
#if _DEBUG
	smd.debug_name = debug_name;
#else
	(void)debug_name;
#endif
	return spudgpu_create_shader_module(shader->instance->desc.device, &smd, &shader->shader_module);
}

extern "C" {

aprend_shader aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage) {
	if (!instance || !spirv_code || !spirv_size || spirv_size % 4 != 0)
		return nullptr;
	if (!aprend_shader_stage_supported(shader_stage))
		return nullptr;

	APREND_MALLOC__T(result, aprend_shader_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_shader_t);
	result->instance = instance;

	SPUDRESULT sr = aprend_shader_create_module(result, spirv_code, spirv_size, shader_stage, "aprend_shader_create_spirv");
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_shader_create_spirv failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return nullptr;
	}
	return result;
}

aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage) {
	if (!instance || !filename)
		return nullptr;
	if (!aprend_shader_stage_supported(shader_stage))
		return nullptr;

	if (!sfs_file_exists(filename))
		return nullptr;

	APREND_MALLOC__T(result, aprend_shader_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_shader_t);

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

	// The raw file bytes aren't needed once SpudGPU has its own copy.
	sr = aprend_shader_create_module(result, data, data_size, shader_stage, filename);
	free(data);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_shader_read_from_file_spirv failed ('%s'): %s\n", filename, spudresult_str(sr));
	APREND_DESTRUCT__T(result, aprend_shader_t);
	return nullptr;
}
void aprend_shader_destroy(aprend_shader shader) {
	if (shader)
		APREND_DESTRUCT__T(shader, aprend_shader_t);
}

aprend_graphics_pipeline aprend_graphics_pipeline_create(
    aprend_instance instance,
    aprend_graphics_pipeline_desc desc) {
	if (!instance || !desc.vertex_shader || !desc.fragment_shader)
		return nullptr;
	if (desc._vertex_layout.count > SPUDGPU_MAX_VERTEX_ATTRIBUTES)
		return nullptr;
	if (desc._depth_test && desc._depth_compare_op == SPUDGPU_COMPARE_OP_NEVER) {
		// NEVER would discard every fragment - almost certainly an unset field.
		printf("apricot: aprend_graphics_pipeline_create: _depth_test is set but _depth_compare_op is NEVER\n");
		return nullptr;
	}
	if (!aprend_uniform_bindings_valid(desc)) {
		printf("apricot: aprend_graphics_pipeline_create: invalid or duplicate uniform bindings\n");
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_graphics_pipeline_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_graphics_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	if (desc._uniform_binding_count > 0) {
		spudgpu_descriptor_set_layout_desc ld{};
		for (uint32_t i = 0; i < desc._uniform_binding_count; ++i) {
			ld.bindings[i].binding         = desc._uniform_bindings[i]._binding;
			ld.bindings[i].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			ld.bindings[i].count           = 1;
			ld.bindings[i].stage_flags     = desc._uniform_bindings[i]._stages;
		}
		ld.binding_count = desc._uniform_binding_count;
#if _DEBUG
		ld.debug_name = desc._debug_name;
#endif
		SPUDRESULT lsr = spudgpu_create_descriptor_set_layout(instance->desc.device, &ld, &result->uniform_layout);
		if (SPUDFAIL(lsr)) {
			printf("apricot: aprend_graphics_pipeline_create: uniform layout failed: %s\n", spudresult_str(lsr));
			APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
			return nullptr;
		}
	}

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
	pd.front_face_ccw     = false; // GLM Y-flip inverts winding: world-CCW becomes screen-CW
	pd.wireframe          = desc._wireframe;
	pd.depth_test_enable  = desc._depth_test;
	pd.depth_write_enable = desc._depth_write;
	pd.depth_compare_op   = desc._depth_compare_op;

	pd.color_attachment_format = desc.color_attachment_format;
	pd.depth_format            = desc.depth_format;
	if (result->uniform_layout) {
		pd.descriptor_set_layouts[0]    = result->uniform_layout;
		pd.descriptor_set_layout_count = 1;
	}
#if _DEBUG
	pd.debug_name = desc._debug_name;
#endif

	SPUDRESULT sr = spudgpu_create_shader_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_graphics_pipeline_create failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return nullptr;
	}

	return result;
}
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p) {
	if (p)
		APREND_DESTRUCT__T(p, aprend_graphics_pipeline_t);
}
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p) { return p ? p->desc : aprend_graphics_pipeline_desc{}; }

aprend_uniform_set aprend_uniform_set_create(
    aprend_graphics_pipeline pipeline,
    const aprend_uniform_set_entry *entries,
    uint32_t entry_count) {
	if (!pipeline || !pipeline->uniform_layout) {
		printf("apricot: aprend_uniform_set_create: pipeline declares no uniform bindings\n");
		return nullptr;
	}
	const aprend_graphics_pipeline_desc &pdesc = pipeline->desc;
	if (!entries || entry_count != pdesc._uniform_binding_count) {
		printf("apricot: aprend_uniform_set_create: expected %u entries, got %u\n", pdesc._uniform_binding_count, entry_count);
		return nullptr;
	}
	// Every entry must name one of the pipeline's bindings, with a buffer.
	// The count matches and pipeline bindings are unique, so no entry
	// repeating a binding also means every binding is filled.
	for (uint32_t i = 0; i < entry_count; ++i) {
		bool declared = false;
		for (uint32_t b = 0; b < pdesc._uniform_binding_count; ++b)
			declared |= pdesc._uniform_bindings[b]._binding == entries[i]._binding;
		bool repeated = false;
		for (uint32_t j = 0; j < i; ++j)
			repeated |= entries[j]._binding == entries[i]._binding;
		if (!declared || repeated || !entries[i]._buffer) {
			printf("apricot: aprend_uniform_set_create: entry %u (binding %u) is undeclared, repeated, or has no buffer\n", i,
			       entries[i]._binding);
			return nullptr;
		}
	}

	spudgpu_device device = pipeline->instance->desc.device;

	APREND_MALLOC__T(result, aprend_uniform_set_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_uniform_set_t);
	result->pipeline = pipeline;

	SPUDRESULT sr = SPUD_SUCCESS;
	{
		spudgpu_descriptor_pool_desc pool_desc{};
		pool_desc.max_sets                  = 1;
		pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		pool_desc.pool_sizes[0].count           = entry_count;
		pool_desc.pool_size_count           = 1;
		sr = spudgpu_create_descriptor_pool(device, &pool_desc, &result->pool);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}
	{
		spudgpu_descriptor_set_desc set_desc{};
		set_desc.pool           = result->pool;
		set_desc.set_layouts[0] = pipeline->uniform_layout;
		set_desc.set_count      = 1;
		sr = spudgpu_create_descriptor_sets(device, &set_desc, &result->set);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}
	{
		spudgpu_descriptor_buffer_info infos[APREND_MAX_UNIFORM_BINDINGS]{};
		spudgpu_write_descriptor_set writes[APREND_MAX_UNIFORM_BINDINGS]{};
		for (uint32_t i = 0; i < entry_count; ++i) {
			aprend_uniform_buffer buffer = entries[i]._buffer;
			infos[i].buffer              = buffer->buffer;
			infos[i].offset              = buffer->buffer_view_desc.offset_from_parent_buffer;
			infos[i].range               = buffer->total_size;

			writes[i].dst_set          = result->set;
			writes[i].dst_binding      = entries[i]._binding;
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writes[i].buffer_info      = &infos[i];
		}
		spudgpu_update_descriptor_sets(device, writes, entry_count);
	}

	return result;
failedattempt:
	printf("apricot: aprend_uniform_set_create failed: %s\n", spudresult_str(sr));
	APREND_DESTRUCT__T(result, aprend_uniform_set_t);
	return nullptr;
}
void aprend_uniform_set_destroy(aprend_uniform_set set) {
	if (set)
		APREND_DESTRUCT__T(set, aprend_uniform_set_t);
}

} // Extern "C"
