
#include "render/aprendpipeline.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"
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
	// A pipeline that failed to create has nothing to destroy.
	if (this->pipeline)
		spudgpu_destroy_shader_pipeline(this->pipeline);
	free(this->entry_points);
}
aprend_binding_layout_t::~aprend_binding_layout_t() {
	// Pools, then the layout their sets were allocated against.
	for (uint32_t i = 0; i < this->pool_count; ++i)
		spudgpu_destroy_descriptor_pool(this->pools[i]);
	free(this->pools);
	free(this->free_sets);
	spudgpu_destroy_descriptor_set_layout(this->layout);
}
aprend_binding_set_t::~aprend_binding_set_t() {
	free(this->image_uses);
	free(this->staged_stores);
	// A set that failed to create gives back what it got as far as acquiring.
	for (uint32_t frame = 0; frame < this->frame_set_count; ++frame)
		this->layout->free_sets[this->layout->free_set_count++] = this->sets[frame];
}

static_assert(APREND_DESCRIPTOR_TYPE_COUNT <= SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES);

// Adds one more pool to [layout], and room on its free list for the sets that
// pool will hold.
static SPUDRESULT aprend_binding_layout_add_pool(aprend_binding_layout_t *layout) {
	const uint32_t pool_count = layout->pool_count + 1;

	spudgpu_descriptor_pool *pools = (spudgpu_descriptor_pool *)realloc(layout->pools, sizeof(spudgpu_descriptor_pool) * pool_count);
	if (!pools)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->pools = pools;

	spudgpu_descriptor_set *free_sets =
	    (spudgpu_descriptor_set *)realloc(layout->free_sets, sizeof(spudgpu_descriptor_set) * pool_count * APREND_BINDING_SETS_PER_POOL);
	if (!free_sets)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->free_sets = free_sets;

	// One pool size per descriptor type the layout uses.
	uint32_t per_set[APREND_DESCRIPTOR_TYPE_COUNT]{};
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		per_set[layout->bindings[i]._type] += layout->bindings[i]._count;

	spudgpu_descriptor_pool_desc pool_desc{};
	pool_desc.max_sets = APREND_BINDING_SETS_PER_POOL;
	for (uint32_t type = 0; type < APREND_DESCRIPTOR_TYPE_COUNT; ++type) {
		if (per_set[type] == 0)
			continue;
		pool_desc.pool_sizes[pool_desc.pool_size_count].descriptor_type = type;
		pool_desc.pool_sizes[pool_desc.pool_size_count].count           = APREND_BINDING_SETS_PER_POOL * per_set[type];
		++pool_desc.pool_size_count;
	}
	SPUDRESULT sr = spudgpu_create_descriptor_pool(layout->instance->desc.device, &pool_desc, &layout->pools[pool_count - 1]);
	if (SPUDFAIL(sr))
		return sr;

	layout->pool_count        = pool_count;
	layout->sets_in_last_pool = 0;
	return SPUD_SUCCESS;
}

// A descriptor set of [layout]: one given back by a destroyed
// aprend_binding_set if there is one, else a new one from the last pool,
// adding a pool when that is full.
static SPUDRESULT aprend_binding_layout_acquire_set(aprend_binding_layout_t *layout, spudgpu_descriptor_set *out_set) {
	if (layout->free_set_count > 0) {
		*out_set = layout->free_sets[--layout->free_set_count];
		return SPUD_SUCCESS;
	}
	if (layout->pool_count == 0 || layout->sets_in_last_pool == APREND_BINDING_SETS_PER_POOL) {
		SPUDRESULT sr = aprend_binding_layout_add_pool(layout);
		if (SPUDFAIL(sr))
			return sr;
	}

	spudgpu_descriptor_set_desc set_desc{};
	set_desc.pool           = layout->pools[layout->pool_count - 1];
	set_desc.set_layouts[0] = layout->layout;
	set_desc.set_count      = 1;
	SPUDRESULT sr = spudgpu_create_descriptor_sets(layout->instance->desc.device, &set_desc, out_set);
	if (SPUDFAIL(sr))
		return sr;
	++layout->sets_in_last_pool;
	return SPUD_SUCCESS;
}

static const aprend_binding_desc *aprend_binding_layout_find(const aprend_binding_layout_t *layout, uint32_t binding) {
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		if (layout->bindings[i]._binding == binding)
			return &layout->bindings[i];
	return nullptr;
}

static bool aprend_descriptor_type_reads_view(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static bool aprend_descriptor_type_reads_sampler(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER || type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Whether [offset] and [range] (0 = to the end) stay inside a buffer of
// [size] bytes and cover at least one of them.
static bool aprend_binding_range_valid(uint64_t offset, uint64_t range, uint64_t size) {
	if (offset >= size)
		return false;
	if (range > size - offset)
		return false;
	return true;
}

// Whether entry [index] of a set holds what its _type reads. The type has
// already been checked against the layout's.
static bool aprend_binding_set_entry_resource_valid(aprend_instance instance, const aprend_binding_set_entry &entry, uint32_t index) {
	switch (entry._type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
		const auto &r = entry._resource._uniform;
		if (!r._buffer && !r._set) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has no uniform buffer and no uniform buffer set\n", index, entry._binding);
			return false;
		}
		if (r._buffer && r._set) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has both a uniform buffer and a uniform buffer set\n", index, entry._binding);
			return false;
		}
		if (r._set && r._set->instance != instance) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has a uniform buffer set of another instance\n", index, entry._binding);
			return false;
		}
		// Every copy of a set is the same size.
		const aprend_uniform_buffer buffer = r._set ? r._set->buffers[0] : r._buffer;
		if (!aprend_binding_range_valid(r._offset, r._range, buffer->total_size)) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has an _offset and _range outside its buffer\n", index, entry._binding);
			return false;
		}
		return true;
	}
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
		const auto &r = entry._resource._storage;
		if (!r._buffer && !r._set) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has no storage buffer and no storage buffer set\n", index, entry._binding);
			return false;
		}
		if (r._buffer && r._set) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has both a storage buffer and a storage buffer set\n", index, entry._binding);
			return false;
		}
		if (r._set && r._set->instance != instance) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has a storage buffer set of another instance\n", index, entry._binding);
			return false;
		}
		const aprend_storage_buffer buffer = r._set ? r._set->buffers[0] : r._buffer;
		if (!aprend_binding_range_valid(r._offset, r._range, buffer->size)) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has an _offset and _range outside its buffer\n", index, entry._binding);
			return false;
		}
		return true;
	}
	default:
		break;
	}

	const auto &r = entry._resource._image;
	if (aprend_descriptor_type_reads_view(entry._type)) {
		if (!r._view) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has no texture view\n", index, entry._binding);
			return false;
		}
		const APREND_TEXTURE_VIEW_TYPE wanted = entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE
		                                            ? APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS
		                                            : APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		if (r._view->view_type != wanted) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has a view of type %u, its slot takes %u\n", index, entry._binding,
			       (unsigned)r._view->view_type, (unsigned)wanted);
			return false;
		}
	}
	if (aprend_descriptor_type_reads_sampler(entry._type)) {
		if (!r._sampler) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has no sampler\n", index, entry._binding);
			return false;
		}
	}
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

spudgpu_blend_attachment_desc aprend_blend_premultiplied(void) {
	spudgpu_blend_attachment_desc blend{};
	blend.blend_enable           = true;
	blend.src_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.color_blend_op         = SPUDGPU_BLEND_OP_ADD;
	blend.src_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.alpha_blend_op         = SPUDGPU_BLEND_OP_ADD;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_alpha(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.src_color_blend_factor        = SPUDGPU_BLEND_FACTOR_SRC_ALPHA;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_additive(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.dst_color_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	return blend;
}

aprend_graphics_pipeline aprend_graphics_pipeline_create(
    aprend_instance instance,
    aprend_graphics_pipeline_desc desc) {
	if (!instance)
		return nullptr;
	if (!desc.vertex_shader)
		return nullptr;
	if (!desc.fragment_shader)
		return nullptr;
	if (!desc._vertex_entry_point) {
		printf("apricot: aprend_graphics_pipeline_create: _vertex_entry_point is NULL\n");
		return nullptr;
	}
	if (!desc._vertex_entry_point[0]) {
		printf("apricot: aprend_graphics_pipeline_create: _vertex_entry_point is an empty string\n");
		return nullptr;
	}
	if (!desc._fragment_entry_point) {
		printf("apricot: aprend_graphics_pipeline_create: _fragment_entry_point is NULL\n");
		return nullptr;
	}
	if (!desc._fragment_entry_point[0]) {
		printf("apricot: aprend_graphics_pipeline_create: _fragment_entry_point is an empty string\n");
		return nullptr;
	}
	if (desc._vertex_binding_count > APREND_MAX_VERTEX_BINDINGS) {
		printf("apricot: aprend_graphics_pipeline_create: _vertex_binding_count %u is above APREND_MAX_VERTEX_BINDINGS\n", desc._vertex_binding_count);
		return nullptr;
	}
	uint32_t vertex_attribute_count = 0;
	for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
		const aprend_buffer_layout &layout = desc._vertex_bindings[i]._layout;
		if (layout.count == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has no elements\n", i);
			return nullptr;
		}
		if (!layout.elements) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has a NULL elements array\n", i);
			return nullptr;
		}
		if (aprend_buffer_layout_get_total_size(&layout) == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has a stride of 0\n", i);
			return nullptr;
		}
		if (layout.count > SPUDGPU_MAX_VERTEX_ATTRIBUTES - vertex_attribute_count) {
			printf("apricot: aprend_graphics_pipeline_create: the vertex bindings hold more than SPUDGPU_MAX_VERTEX_ATTRIBUTES elements\n");
			return nullptr;
		}
		vertex_attribute_count += layout.count;
	}
	if (desc._push_constant_range_count > APREND_MAX_PUSH_CONSTANT_RANGES) {
		printf("apricot: aprend_graphics_pipeline_create: _push_constant_range_count %u is above APREND_MAX_PUSH_CONSTANT_RANGES\n",
		       desc._push_constant_range_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		const spudgpu_push_constant_range_desc &range = desc._push_constant_ranges[i];
		if (range.stage_flags == SPUDGPU_SHADER_STAGE_NONE) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has no stages\n", i);
			return nullptr;
		}
		if (range.size == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has a size of 0\n", i);
			return nullptr;
		}
		if (range.offset % 4 != 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has an offset that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size % 4 != 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has a size that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size > UINT32_MAX - range.offset) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] runs past what 32 bits can address\n", i);
			return nullptr;
		}
	}
	if (desc._cull_mode > SPUDGPU_CULL_MODE_BACK) {
		printf("apricot: aprend_graphics_pipeline_create: _cull_mode %u is not a SPUDGPU_CULL_MODE\n", (unsigned)desc._cull_mode);
		return nullptr;
	}
	if (desc._depth_test && desc._depth_compare_op == SPUDGPU_COMPARE_OP_NEVER) {
		// NEVER would discard every fragment - almost certainly an unset field.
		printf("apricot: aprend_graphics_pipeline_create: _depth_test is set but _depth_compare_op is NEVER\n");
		return nullptr;
	}
	if (desc._binding_layout_count > APREND_MAX_BINDING_LAYOUTS) {
		printf("apricot: aprend_graphics_pipeline_create: _binding_layout_count %u is above APREND_MAX_BINDING_LAYOUTS\n", desc._binding_layout_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i) {
		if (!desc._binding_layouts[i]) {
			printf("apricot: aprend_graphics_pipeline_create: _binding_layouts[%u] is NULL\n", i);
			return nullptr;
		}
		if (desc._binding_layouts[i]->instance != instance) {
			printf("apricot: aprend_graphics_pipeline_create: _binding_layouts[%u] belongs to another instance\n", i);
			return nullptr;
		}
	}

	APREND_MALLOC__T(result, aprend_graphics_pipeline_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_graphics_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	// The names are copied, so the desc aprend_graphics_pipeline_get_desc
	// hands back never points at a string the caller has released.
	const size_t vertex_entry_size   = strlen(desc._vertex_entry_point) + 1;
	const size_t fragment_entry_size = strlen(desc._fragment_entry_point) + 1;
	result->entry_points             = (char *)malloc(vertex_entry_size + fragment_entry_size);
	if (!result->entry_points) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return nullptr;
	}
	memcpy(result->entry_points, desc._vertex_entry_point, vertex_entry_size);
	memcpy(result->entry_points + vertex_entry_size, desc._fragment_entry_point, fragment_entry_size);
	result->desc._vertex_entry_point   = result->entry_points;
	result->desc._fragment_entry_point = result->entry_points + vertex_entry_size;

	spudgpu_shader_pipeline_desc pd{};
	pd.vertex_module        = desc.vertex_shader->shader_module;
	pd.vertex_entry_point   = result->desc._vertex_entry_point;
	pd.fragment_module      = desc.fragment_shader->shader_module;
	pd.fragment_entry_point = result->desc._fragment_entry_point;

	// Locations run on through the bindings in order.
	for (uint32_t binding = 0; binding < desc._vertex_binding_count; ++binding) {
		const aprend_vertex_binding_desc &vb = desc._vertex_bindings[binding];
		result->vertex_strides[binding]      = aprend_buffer_layout_get_total_size(&vb._layout);

		pd.vertex_bindings[binding].binding      = binding;
		pd.vertex_bindings[binding].stride       = result->vertex_strides[binding];
		pd.vertex_bindings[binding].per_instance = vb._per_instance;

		for (uint32_t i = 0; i < vb._layout.count; ++i) {
			const aprend_buffer_element &el         = vb._layout.elements[i];
			spudgpu_vertex_attribute_desc &attribute = pd.vertex_attributes[pd.vertex_attribute_count];
			attribute.location                       = pd.vertex_attribute_count;
			attribute.binding                        = binding;
			attribute.format                         = aprend_buffer_element_type_to_format(el.type);
			attribute.offset                         = el.offset;
			++pd.vertex_attribute_count;
		}
	}
	pd.vertex_binding_count = desc._vertex_binding_count;

	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		pd.push_constant_ranges[i] = desc._push_constant_ranges[i];
		const uint32_t range_end   = desc._push_constant_ranges[i].offset + desc._push_constant_ranges[i].size;
		if (range_end > result->push_constant_end)
			result->push_constant_end = range_end;
	}
	pd.push_constant_range_count = desc._push_constant_range_count;

	pd.primitive_topology = desc._topology;
	pd.cull_mode          = desc._cull_mode;
	pd.front_face_ccw     = desc._front_face_ccw;
	pd.wireframe          = desc._wireframe;
	pd.depth_test_enable  = desc._depth_test;
	pd.depth_write_enable = desc._depth_write;
	pd.depth_compare_op   = desc._depth_compare_op;
	pd.blend_attachment   = desc._blend;

	pd.color_attachment_format = desc.color_attachment_format;
	pd.depth_format            = desc.depth_format;
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i)
		pd.descriptor_set_layouts[i] = desc._binding_layouts[i]->layout;
	pd.descriptor_set_layout_count = desc._binding_layout_count;
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

aprend_binding_layout aprend_binding_layout_create(
    aprend_instance instance,
    const aprend_binding_desc *bindings,
    uint32_t binding_count) {
	if (!instance) {
		printf("apricot: aprend_binding_layout_create: instance is NULL\n");
		return nullptr;
	}
	if (!bindings) {
		printf("apricot: aprend_binding_layout_create: bindings is NULL\n");
		return nullptr;
	}
	if (binding_count == 0) {
		printf("apricot: aprend_binding_layout_create: binding_count is 0\n");
		return nullptr;
	}
	if (binding_count > APREND_MAX_BINDINGS_PER_LAYOUT) {
		printf("apricot: aprend_binding_layout_create: binding_count %u is above APREND_MAX_BINDINGS_PER_LAYOUT\n", binding_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < binding_count; ++i) {
		if (bindings[i]._type >= APREND_DESCRIPTOR_TYPE_COUNT) {
			printf("apricot: aprend_binding_layout_create: entry %u has a _type that is not a SPUDGPU_DESCRIPTOR_TYPE\n", i);
			return nullptr;
		}
		if (bindings[i]._count == 0) {
			printf("apricot: aprend_binding_layout_create: entry %u has a _count of 0\n", i);
			return nullptr;
		}
		if (bindings[i]._stages == SPUDGPU_SHADER_STAGE_NONE) {
			printf("apricot: aprend_binding_layout_create: entry %u has no _stages\n", i);
			return nullptr;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (bindings[j]._binding == bindings[i]._binding) {
				printf("apricot: aprend_binding_layout_create: entries %u and %u share binding %u\n", j, i, bindings[i]._binding);
				return nullptr;
			}
		}
	}

	APREND_MALLOC__T(result, aprend_binding_layout_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_binding_layout_t);
	result->instance      = instance;
	result->binding_count = binding_count;

	spudgpu_descriptor_set_layout_desc ld{};
	for (uint32_t i = 0; i < binding_count; ++i) {
		result->bindings[i] = bindings[i];
		result->descriptor_count += bindings[i]._count;

		ld.bindings[i].binding         = bindings[i]._binding;
		ld.bindings[i].descriptor_type = bindings[i]._type;
		ld.bindings[i].count           = bindings[i]._count;
		ld.bindings[i].stage_flags     = bindings[i]._stages;
	}
	ld.binding_count = binding_count;

	SPUDRESULT sr = spudgpu_create_descriptor_set_layout(instance->desc.device, &ld, &result->layout);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_binding_layout_create failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return nullptr;
	}
	return result;
}
void aprend_binding_layout_destroy(aprend_binding_layout layout) {
	if (layout) {
		APREND_DESTRUCT__T(layout, aprend_binding_layout_t);
	}
}

aprend_binding_set aprend_binding_set_create(
    aprend_binding_layout layout,
    const aprend_binding_set_entry *entries,
    uint32_t entry_count) {
	if (!layout) {
		printf("apricot: aprend_binding_set_create: layout is NULL\n");
		return nullptr;
	}
	if (!entries) {
		printf("apricot: aprend_binding_set_create: entries is NULL\n");
		return nullptr;
	}
	if (entry_count != layout->descriptor_count) {
		printf("apricot: aprend_binding_set_create: expected %u entries, got %u\n", layout->descriptor_count, entry_count);
		return nullptr;
	}
	// Every entry must name an array element the layout declares, once. The
	// count matches, so nothing repeated also means nothing left unfilled.
	uint32_t image_use_count = 0;
	bool has_buffer_set      = false;
	for (uint32_t i = 0; i < entry_count; ++i) {
		const aprend_binding_set_entry &entry = entries[i];
		const aprend_binding_desc *binding    = aprend_binding_layout_find(layout, entry._binding);
		if (!binding) {
			printf("apricot: aprend_binding_set_create: entry %u names binding %u, which the layout doesn't declare\n", i, entry._binding);
			return nullptr;
		}
		if (entry._array_element >= binding->_count) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has _array_element %u of %u\n", i, entry._binding, entry._array_element,
			       binding->_count);
			return nullptr;
		}
		if (entry._type != binding->_type) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has _type %u, the layout declares %u\n", i, entry._binding,
			       (unsigned)entry._type, (unsigned)binding->_type);
			return nullptr;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (entries[j]._binding == entry._binding && entries[j]._array_element == entry._array_element) {
				printf("apricot: aprend_binding_set_create: entries %u and %u both fill binding %u element %u\n", j, i, entry._binding,
				       entry._array_element);
				return nullptr;
			}
		}
		if (!aprend_binding_set_entry_resource_valid(layout->instance, entry, i))
			return nullptr;
		if (aprend_descriptor_type_reads_view(entry._type))
			++image_use_count;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && entry._resource._uniform._set)
			has_buffer_set = true;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && entry._resource._storage._set)
			has_buffer_set = true;
	}
	// One descriptor set if nothing in it changes with the frame; one for
	// each frame in flight if an entry is a buffer set.
	const uint32_t frame_set_count = has_buffer_set ? layout->instance->desc.frames_in_flight : 1;

	// What the writes point at; only needed until the set is written.
	struct write_info {
		spudgpu_descriptor_buffer_info buffer;
		spudgpu_descriptor_image_info image;
	};
	write_info *infos = (write_info *)calloc(entry_count, sizeof(write_info));
	if (!infos)
		return nullptr;
	spudgpu_write_descriptor_set *writes = (spudgpu_write_descriptor_set *)calloc(entry_count, sizeof(spudgpu_write_descriptor_set));
	if (!writes) {
		free(infos);
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_binding_set_t);
	if (!result) {
		free(writes);
		free(infos);
		return nullptr;
	}
	APREND_CONSTRUCT__T(result, aprend_binding_set_t);
	result->layout = layout;

	if (image_use_count > 0) {
		result->image_uses = (aprend_binding_set_t::image_use *)calloc(image_use_count, sizeof(aprend_binding_set_t::image_use));
		if (!result->image_uses) {
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return nullptr;
		}
	}

	// Room for every entry of every frame's set: at most each one is a
	// staged storage buffer.
	result->staged_store_capacity = entry_count;
	result->staged_stores         = (aprend_buffer_store **)calloc((size_t)entry_count * frame_set_count, sizeof(aprend_buffer_store *));
	if (!result->staged_stores) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		free(writes);
		free(infos);
		return nullptr;
	}

	// frame_set_count goes up with each set acquired, so the destructor
	// gives back exactly those if a later one fails.
	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		SPUDRESULT sr = aprend_binding_layout_acquire_set(layout, &result->sets[frame]);
		if (SPUDFAIL(sr)) {
			printf("apricot: aprend_binding_set_create failed: %s\n", spudresult_str(sr));
			result->sets[frame] = nullptr;
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return nullptr;
		}
		++result->frame_set_count;
	}

	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		aprend_buffer_store **frame_stores = result->staged_stores + (size_t)frame * entry_count;
		for (uint32_t i = 0; i < entry_count; ++i) {
			const aprend_binding_set_entry &entry = entries[i];
			spudgpu_write_descriptor_set &write   = writes[i];
			write                                 = {};
			infos[i]                              = {};
			write.dst_set                         = result->sets[frame];
			write.dst_binding                     = entry._binding;
			write.dst_array_element               = entry._array_element;
			write.descriptor_count                = 1;
			write.descriptor_type                 = entry._type;

			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
				const auto &r                      = entry._resource._uniform;
				const aprend_uniform_buffer buffer = r._set ? r._set->buffers[frame] : r._buffer;
				infos[i].buffer.buffer             = buffer->buffer;
				infos[i].buffer.offset             = buffer->buffer_view_desc.offset_from_parent_buffer + r._offset;
				infos[i].buffer.range              = r._range ? r._range : buffer->total_size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				continue;
			}
			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				const auto &r                      = entry._resource._storage;
				const aprend_storage_buffer buffer = r._set ? r._set->buffers[frame] : r._buffer;
				infos[i].buffer.buffer             = buffer->store.buffer;
				infos[i].buffer.offset             = buffer->buffer_view_desc.offset_from_parent_buffer + r._offset;
				infos[i].buffer.range              = r._range ? r._range : buffer->size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				// The same buffer in two slots is listed twice; a command
				// list takes each store once.
				if (buffer->store.staging)
					frame_stores[result->staged_store_counts[frame]++] = &buffer->store;
				continue;
			}

			const auto &r = entry._resource._image;
			if (aprend_descriptor_type_reads_view(entry._type)) {
				// A storage image is read and written in GENERAL; anything
				// sampled is read in SHADER_READ_ONLY.
				const SPUDGPU_IMAGE_LAYOUT image_layout =
				    entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ? SPUDGPU_IMAGE_LAYOUT_GENERAL : SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY;
				infos[i].image.image_view   = r._view->image_view;
				infos[i].image.image_layout = image_layout;
				write.image_info            = &infos[i].image;

				// The same textures in every frame's set: recorded once.
				if (frame == 0)
					result->image_uses[result->image_use_count++] = {r._view, image_layout};
			}
			if (aprend_descriptor_type_reads_sampler(entry._type))
				write.sampler = r._sampler->sampler;
		}
		spudgpu_update_descriptor_sets(layout->instance->desc.device, writes, entry_count);
	}

	free(writes);
	free(infos);
	return result;
}
void aprend_binding_set_destroy(aprend_binding_set set) {
	if (set) {
		APREND_DESTRUCT__T(set, aprend_binding_set_t);
	}
}

} // Extern "C"
