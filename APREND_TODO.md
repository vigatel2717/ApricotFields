# Aprend TODO

`include/render/aprend*.h` is a renderer over SpudGPU: buffers, textures and
their views, samplers, framebuffers, shaders and graphics pipelines, binding
layouts and sets, a
recorded command list that is compiled and then submitted, and a swap chain. It
is the older generation of code in this repo (see `CLAUDE.md`, "Two generations
of code") and is mid-overhaul. This file holds what is known to be wrong or
unfinished in it.

## State of the code

- **The changes of 2026-10-09 are written, not compiled**, on any platform:
  - `aprend_index_buffer_get_stride`, `aprend_buffer_element_type_get_size` and
    `aprend_buffer_layout_get_element_index` defined (the header declared them
    and nothing defined them).
  - `aprend_uniform_buffer_update_by_name`, `aprend_texture2d_resize` and
    `aprend_texture3d_resize` implemented (they returned false).
  - `aprend_send_command` copies the arrays of `SET_VERTEX_BUFFERS`,
    `SET_VIEWPORTS` and `SET_SCISSOR_RECTS` into the command list.
  - Texture update and readback and the framebuffer clears write a texture's
    tracked layout only after their submission succeeds, and leave the texture
    in the layout they found it in.
  - Storage buffers are created host-visible and host-coherent, and
    `aprend_storage_buffer_update` checks its range against the buffer's size.
  - `aprend_graphics_pipeline_desc` takes `_cull_mode` and `_front_face_ccw` in
    place of `_backface_culling`.
- **The changes of 2026-10-10 are written, not compiled**, on any platform:
  - `aprend_uniform_set` is replaced by `aprend_binding_layout` and
    `aprend_binding_set`, over every SpudGPU descriptor type and all four set
    slots (was item 9). A layout owns the pools its sets come from, 32 sets to
    a pool, and a destroyed set's descriptor set is reused.
  - `aprend_sampler`, and `APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE` for a
    view that is sampled.
  - `APREND_COMMAND_SET_BINDING_SET` replaces `SET_UNIFORM_SET`. Sets are bound
    at the draw, and a pass's sampled and storage textures are moved into
    their layouts before the pass begins. Layout tracking covers 3D textures.
  - None of it has run: no texture has been sampled and no storage buffer or
    storage image has been read through a set.
- **No tests.** There is no `tests/aprend_tests.*`: the repo's tests are
  headless with no GPU, and nothing in Aprend runs without a device.

## Wrong things

### 1. A uniform buffer's size is not std140

`aprend_uniform_buffer_create` sizes the buffer by adding up each uniform's byte
size (type size times element count) rounded up to 16. It places nothing:
offsets come from the caller's `aprend_uniform_layout`. So the only thing this
can get wrong is how large the buffer is.

- **Right or harmless** for a layout of plain scalars, vectors and `mat4`s. The
  sum is never smaller than the std140 block, since no such member is larger
  than its 16-byte rounding or aligned to more than 16. It over-allocates (four
  floats get 64 bytes where std140 packs them into 16), which costs a few bytes.
  A block of one `mat4` comes out at exactly 64.
- **Wrong** for an array of anything smaller than 16 bytes: `float[]`, `int[]`,
  `bool[]`, `vec2[]`, `vec3[]` and their integer forms. std140 gives every array
  element a 16-byte stride, so `float[4]` is 64 bytes in the shader and 16 here.
  The buffer is then smaller than the shader's block: an update past its end is
  refused by `aprend_uniform_buffer_update`'s bounds check, and the shader reads
  beyond the range the descriptor binds.
- `aprend_uniform_buffer_update_by_name` has the same limit. It writes an
  array's elements tightly packed, which is not where std140 puts them.

Not urgent: nothing wrong happens until a layout declares such an array. Two
ways it ends.

- **The small fix**, about ten lines, for when an array uniform is needed first.
  Size the buffer from the caller's offsets instead of a running sum: the
  largest `offset` plus that uniform's footprint, rounded up to 16. Give array
  elements a 16-byte stride, in the footprint and in what
  `aprend_uniform_buffer_update_by_name` writes. It changes nothing for a layout
  without arrays.
- **The real fix** is SPIR-V reflection: layouts, offsets and block sizes read
  from the shader, so none of this is computed by hand or declared by the
  caller. That removes this item rather than repairing it, and it belongs with
  the rewrite `aprendbuffers.h` already says it is due (item 18).

### 2. Vertex, index and uniform buffers ask for memory not every device has

They ask for host-visible, host-coherent and device-local memory together.
SpudGPU's Vulkan backend passes all three on as required properties, and a
device with its own memory need not have a type with all three: the buffer then
fails to create. Metal and D3D12 choose from host-visible alone, so it shows on
Vulkan only. Storage buffers asked for device-local alone and were then mapped,
which SpudGPU refuses on every backend; they now ask for host-visible and
host-coherent, which every device has. The other three want the same until
item 13 (staged uploads) replaces mapping.

A storage buffer in host-visible memory can be read by a shader but, on D3D12,
not written by one. A binding set can now hold one, so this is reachable;
GPU-written storage buffers need device-local memory and so item 13.

### 3. `mip_levels = 0` allocates a full chain that is never filled

A texture created with `mip_levels` 0 gets every level, but
`aprend_texture*_update` writes level 0 only and nothing generates the rest, so
the lower levels hold whatever the allocation held. A sampler whose `max_lod`
is above 0 now reads them, so until this is fixed a sampled texture wants
`mip_levels` 1 or `max_lod` 0. Needs mip generation (item 31) and a mip level
and array layer argument on update and readback, which today are fixed at
level 0, layer 0.

### 4. Desc fields that are accepted and ignored

Each of these is read by nothing. Either implement it or remove it from the
desc; a field that silently does nothing is a default hiding a choice.

- `aprend_texture2d_desc` / `aprend_texture3d_desc`: `initial_data` and its
  pitches (a `TODO` in the create calls), and `store_locally`.
- `aprend_graphics_pipeline_desc::_line_width`.
- `aprend_framebuffer_desc::swap_chain_target`.
- `aprend_instance_desc`: `app_name`, `app_version`, `engine_name`,
  `engine_version`.
- The `APREND_TEXTURE_VIEW_TYPE` passed to `aprend_texture_view_create_*` is
  stored and returned but doesn't change the view that is made.

### 5. Choices made inside the pipeline that the caller can't make

- Both shader entry points are fixed to `"main"`. The desc is stored and handed
  back by `aprend_graphics_pipeline_get_desc`, so an entry point name on it has
  to be copied, not kept as the caller's pointer.

Aprend's rule is that such a choice is made here explicitly as a rendering
matter or taken from the caller through the desc; this is neither documented
nor selectable. Front face and cull mode were the same and are now desc fields
(`_front_face_ccw`, `_cull_mode`).

### 6. Immediate operations pick their own queue and block

`aprend_submit_immediate` (texture update and readback, framebuffer clears)
makes a new command allocator and list for every call, submits on
`spudgpu_get_graphics_queue`, and waits for the queue to go idle. Every other
submission goes on a queue the caller passes in. The queue should be the
caller's here too, and the blocking wait is replaced by item 13.

### 7. Dead code and leftovers

- `aprend___internal___load_spirv` in `aprendcontext.cpp` is unused.
- `aprend_instance_t::cmd_list` is created and never used.
- `shaders/apricot_default.vert` reads a `push_constant` block that no Aprend
  command can set (item 10); nothing loads either default shader.
- `aprendpipeline.h` forward-declares `aprend_graphics_pipeline_layout`, which
  doesn't exist.
- `apricotfields.h` includes two of the seven Aprend headers.
- Three unused includes in `aprend_internal.hpp` (`aprendframes.h`, `<string>`,
  `<unordered_map>`).

### 8. Older-generation habits still to convert

Per `CLAUDE.md`, only when the function is already being changed: joined
argument checks (`if (!instance || !desc)`), failure reported by `printf` with no
result code, a few unchecked `malloc`s (the layout copy in
`aprend_vertex_buffer_create`, the file buffer in
`aprend_shader_read_from_file_spirv`), and `std::vector` growth inside C entry
points with nothing catching `std::bad_alloc`.

## Real gaps: what a renderer at Hazel's level has

Hazel (`../.refs/HazelRenderer2025/Hazel/Hazel/src/Hazel/Renderer`) is the bar;
see `CLAUDE.md`, "Reference: Hazel", for how its idioms are written here. Each
item names where Hazel does it. Unless it says otherwise, SpudGPU already
exposes what the item needs.

### 9. Binding: what is left

Binding sets cover every descriptor type and set slot (see "State of the
code"). Left over:

- **A set can't be changed after it is created.** Pointing a slot at another
  resource means a new set. Rewriting one in place is a race with a submitted
  frame, so it waits on item 12.
- **Only a graphics pipeline declares layouts.** A compute pipeline takes the
  same `aprend_binding_layout` when item 14 adds one, and binds with
  `spudgpu_cmd_bind_descriptor_sets_compute`.
- **A sampled view covers every mip and layer** and takes its aspect from
  `aprend_texture2d_view_aspect_mask`, which hasn't been checked for sampling
  a depth-stencil format (item 16).
- **A layout has no debug name**: `aprend_binding_layout_create` takes no desc.
- Nothing checks a layout against the SPIR-V (item 18).

### 10. No push constants

SpudGPU has `spudgpu_cmd_push_constants` and push constant ranges on the
pipeline desc; Aprend has neither a desc field nor a command.

### 11. One vertex binding, never per instance

`aprend_graphics_pipeline_create` builds exactly one binding with
`per_instance = false`. `DRAW_INSTANCED` exists but there is no way to give an
instance its own data. Wanted: several layouts per pipeline, each per vertex or
per instance, and `SET_VERTEX_BUFFERS` slots that mean something. Hazel:
`PipelineSpecification` (`Layout`, `InstanceLayout`, `BoneInfluenceLayout`).

### 12. One frame in flight

A uniform buffer is one persistently mapped allocation, so writing it while a
submitted frame still reads it is a race; the only safe use today is to wait for
the queue to go idle between frames. Nothing defers destruction either: every
destroy call requires the caller to know the GPU is finished with the object.

Wanted: buffer sets with one copy per frame in flight, a per-frame release queue
that destroys once that frame's fence has passed, and fence-paced frames instead
of `spudgpu_queue_wait_idle`. Hazel: `UniformBufferSet`, `StorageBufferSet`,
`Renderer::SubmitResourceFree`, `RendererConfig::FramesInFlight`.

### 13. Uploads are host-visible buffers and blocking one-offs

Every buffer is in host-visible memory and written by map, copy, unmap: storage
buffers ask for host-visible and host-coherent, the other three for those and
device-local together (item 2). Each texture upload creates a staging buffer, a
command allocator and a command list, submits and blocks (item 6).

**What that costs depends on the device's memory.**

- Unified memory: nothing. The buffer is in the one memory there is and is
  written with no extra copy. Map and copy is the best path here.
- A device with its own memory: the buffer lives in system memory and the GPU
  reads it across the bus on every access. Storage buffers, which are large and
  read at random, lose the most. On D3D12 such a buffer also can't be written by
  a shader.

**The same SpudGPU memory flags don't select the same memory everywhere.** As
the backends are written today:

| Flags | Vulkan | D3D12 | Metal |
|---|---|---|---|
| `DEVICE_LOCAL` alone | Required property; the device's own memory | Ignored; default heap | Ignored; private storage |
| `HOST_VISIBLE \| HOST_COHERENT` | Required properties; system memory on a device with its own | Upload heap; coherent ignored | Shared storage; coherent ignored |
| All three | All three required; no such memory type on some devices, and the buffer fails | As the row above | As the row above |

`DEVICE_LOCAL` changes the outcome on Vulkan only. Vulkan is also the only
backend where one set of flags lands differently on unified and non-unified
devices: on unified memory most types are both device-local and host-visible,
so all three rows reach the same memory.

**Wanted:** the upload strategy chosen by the device's memory model.

- Unified: host-visible buffers written by map and copy, as now.
- Not unified: device-local buffers (usage `TRANSFER_DST` added) filled from a
  reused host-visible staging buffer with `spudgpu_cmd_copy_buffer`, recorded
  into the frame's own commands rather than a blocking one-off.

Open, to settle before building it:

- **How the model is known.** `CLAUDE.md` allows a compile-time split by
  platform. That is an approximation: an integrated GPU on a platform built for
  staging pays a copy it doesn't need (correct, slower). A runtime choice needs
  the device to say its memory is unified, and SpudGPU doesn't expose that:
  `dedicated_video_memory` is 0 on unified Metal but is the device-local heap
  total on Vulkan, nonzero on an integrated GPU.
- **When a staged write is finished.** A map and copy is done when the call
  returns; a staged copy is done when its commands have run. This is item 12's
  frames in flight and item 6's queue, so the three are designed together.
- All four buffer types move at once. Staging one type alone builds the path
  twice.

### 14. No compute

`aprend_shader_create_spirv` refuses every stage but vertex and fragment. SpudGPU
has compute pipelines, `spudgpu_cmd_dispatch` and compute descriptor binding.
Wanted: a compute pipeline, a dispatch command, and storage images as outputs
(needs item 9). Hazel: `PipelineCompute`, `ComputePass`,
`Renderer::DispatchCompute`.

### 15. Indirect draws, bundles, mesh shading and bindless are unreachable

SpudGPU exposes `spudgpu_cmd_draw_indirect` / `_indexed_indirect`, bundles
(`APREND_RENDERING_FLAG_EXECUTES_BUNDLES` exists, but no command records or
executes one), mesh shading and bindless registration. Aprend has a command for
none of them. Lower priority than the rest; listed so the flag isn't mistaken
for support.

### 16. No per-mip or per-layer views, no cube textures

A texture view always covers every mip and layer. Rendering to one mip or one
array layer (a shadow cascade, a cube face) isn't possible, and there is no cube
texture type although SpudGPU has cube images and views. The layout tracking in
`aprendcommands.cpp` is per whole texture and has to become per subresource for
it (item 17). Hazel: `ImageViewSpecification`, `TextureCube`.

### 31. No mip generation

Numbered out of order so the other items keep their numbers.

Nothing fills a texture's mips below level 0 (item 3), and now that a texture
can be sampled they are read. Wanted: each level made from the one above it.

- **How.** Per level N, for every array layer: move N to `TRANSFER_SRC` and
  N + 1 to `TRANSFER_DST` with `spudgpu_cmd_image_barrier_subresource`, then
  `spudgpu_cmd_blit_image` from N to N + 1 at half the size, never below 1. A
  3D texture halves its depth too. When the last level is written the whole
  chain goes to one layout.
- **Two forms.** `aprend_texture2d_generate_mips` and the 3D one, immediate
  and blocking like `aprend_texture2d_update`, for a texture filled from the
  CPU. A command for a command list, outside any pass, for a texture that was
  just rendered to.
- **The caller asks for it.** `aprend_texture*_update` doesn't generate mips on
  its own: whether a texture's lower levels come from a blit or from the
  caller's own data is the caller's choice. The filter is an argument.
- **It doesn't wait on item 17.** The levels are in different layouts only
  inside the operation. It finds the texture in one layout and leaves it in
  one, so whole-texture tracking stays true.
- **To settle.** Which formats can be blitted with a linear filter: integer and
  depth formats can't on every backend, and SpudGPU has no query for it, so
  either it gains one or the call refuses what no backend guarantees. The
  immediate form picks its own queue until item 6.

Hazel: `Texture2D::GenerateMips`.

### 17. Layout tracking is per whole texture

The command list tracks a layout for each 2D and 3D texture it uses: pass
attachments, the present source, and the textures of a pass's binding sets. It
is one layout for the whole texture, so a texture can't be sampled at one mip
while another is rendered to, and a texture can't be both in a pass's sets and
one of its targets. Becomes per subresource with item 16. Compute (item 14) has
no pass to move its textures before, and needs its own point to do it.

### 32. Texture layouts: what the command list gets wrong or can't do

Numbered out of order, as item 31. Item 17 is the one limit that needs a
redesign; these are the rest, all in `aprendcommands.cpp`.

- **A set left in a slot counts against the next pass.** At `BEGIN_RENDERING`
  every set in a slot is taken as used by the pass, whether or not a draw in it
  reads that slot. So two passes that swap a pair of textures fail to compile:
  pass 1 samples B into A, and pass 2 samples A into B with its
  `SET_BINDING_SET` sent inside the pass. Pass 1's set is still in the slot
  when pass 2 begins, B is in it, and B is pass 2's target. Sending pass 2's
  `SET_BINDING_SET` before its `BEGIN_RENDERING` avoids it. The fix: walk the
  pass as the compile will, and take only the sets that are in a slot the
  bound pipeline declares at a draw.
- **A depth texture can't be tested against and sampled in one pass.** A
  sampled texture goes to `SHADER_READ_ONLY` and a target to its attachment
  layout, and a texture in both is refused. Reading depth in the shader while
  it is the pass's depth target, with depth writes off, needs a read-only
  depth layout, which `SPUDGPU_IMAGE_LAYOUT` doesn't have, and a rule for when
  it is used.
- **A storage image gets a barrier at every pass**, written or not, because
  the set doesn't know whether the shader writes it. Correct, and more than a
  read-only use needs.
- **A compiled list holds a pointer into each texture it uses**, to the
  texture's tracked layout, and the SpudGPU image it had at compile. Destroying
  a texture before submitting a list compiled against it is a use after free.
  After a resize the submit check usually refuses the list, since the tracked
  layout goes back to `UNDEFINED`, but not if the list also found it
  `UNDEFINED`: then it submits barriers for an image that no longer exists.
  Wanted: a texture knows it has changed (a generation count the list
  compares), or the header's "recompile after a resize" is checked.
- **Outside a pass nothing moves a texture for a set**: there is no draw there.
  Compute dispatches will be outside a pass, so item 14 needs its own point to
  do it (also in item 17).
- **A list leaves a sampled texture in `SHADER_READ_ONLY` and a storage image
  in `GENERAL`.** Nothing puts it back. Right for most uses; written down
  because the immediate operations do put a texture back.

### 18. No shader reflection

Vertex layouts, uniform bindings and uniform offsets are declared by hand and
nothing checks them against the SPIR-V, so a mismatch is found on screen.
Wanted: reflection at shader creation giving sets, bindings, block members with
offsets and sizes, and vertex inputs; pipelines and binding sets validated
against it; inputs addressed by name. This is also the real fix for item 1 and
the rewrite `aprendbuffers.h` asks for. Hazel:
`Platform/Vulkan/ShaderCompiler/VulkanShaderCompiler.h` (`Reflect`),
`Shader.h` (`ShaderBuffer`, `ShaderUniform`), `DescriptorSetManager::Validate`.

### 19. No material

Nothing pairs a shader with its parameter values and textures. Wanted, after
items 9 and 18: an object holding a uniform block's bytes and the bound
textures, set by name. Hazel: `Material.h`.

### 20. No render pass or compute pass object

A pass today is a `BEGIN_RENDERING` command the caller fills in each frame,
followed by its own pipeline, set, viewport and scissor commands. Wanted: a pass
that owns its pipeline, target and inputs, validates them once, and is begun
with one command. Hazel: `RenderPass.h`, `ComputePass.h`,
`Renderer::BeginRenderPass`.

`aprend_framebuffer` belongs here too: nothing connects one to
`BEGIN_RENDERING` or to a pipeline's attachment formats, so the caller reads its
views out and builds the targets by hand.

### 21. No draw list, batching or instancing

The caller issues every draw. Wanted: a caller-fed list of draws (mesh, material,
transform) that is sorted, batched by mesh and material into instanced draws
with transforms in a storage buffer, and replayed into each pass that needs it.
No scene, asset or application concept: the caller hands over what to draw.
Needs items 9, 11 and 12. Hazel: `SceneRenderer` (`MeshKey`, `DrawCommand`,
`m_SBSInstanceTransforms`), `Renderer::RenderMesh`.

### 22. Shaders are precompiled SPIR-V only

No runtime compilation, includes, macros, cache or reload; the build compiles two
GLSL files with glslc on Windows and Linux and nothing on Apple. Whether Aprend
should compile shaders at all, or only take SPIR-V and reflection data from its
caller, is undecided. Hazel: `VulkanShaderCompiler`, `ShaderPreprocessor`,
`VulkanShaderCache`, `ShaderPack`.

### 23. No 2D, line, text or debug drawing

Hazel: `Renderer2D` (batched quads, lines, circles, MSDF text), `DebugRenderer`.
Needs items 9 and 12.

### 24. No lit pipeline

No lighting, shadows or post-processing of any kind; a pipeline today outputs
whatever its two shaders compute from one uniform block. Hazel's chain, in
order: directional and spot shadow maps, pre-depth, tiled light culling
(compute), skybox, PBR geometry, GTAO, SSR, bloom, composite
(`SceneRenderer::FlushDrawList`). Each stage is its own piece of work once
items 9 to 21 exist; this entry is the placeholder for splitting them out.

## Needs something SpudGPU doesn't expose yet

What SpudGPU exposes follows from the GPU APIs it translates and is decided
there, not by this list. These are what Aprend can't do until it does.

### 25. Multiple render targets per pipeline

`BEGIN_RENDERING` takes up to `APREND_MAX_COLOR_TARGETS` color targets, but
`spudgpu_shader_pipeline_desc` has one `color_attachment_format` and one
`blend_attachment`, so a pipeline can't describe more than one. Aprend's own
desc mirrors that. Wanted for any G-buffer or a pass with a second output.

### 26. Depth bias

No depth bias on the pipeline desc. Shadow maps need it.

### 27. MSAA and resolve

No sample count on images or pipelines and no resolve attachment.
`aprend_texture2d_create` refuses `sample_count > 1` and `BEGIN_RENDERING`
refuses a `resolve_view` for this reason. Hazel doesn't use MSAA either, so this
is not part of the bar.

### 28. Stencil state

Stencil load and store ops exist on the depth target, but there is no stencil
test or op state on the pipeline. Hazel leaves stencil off as well.

### 29. GPU timing and debug markers

No timer or pipeline statistics queries and no debug marker calls, so there is
no per-pass GPU time and no labelled captures. Hazel:
`RenderCommandBuffer` (`RT_BeginTimerQuery`, `RT_BeginMarker`).

### 30. Hardware without dynamic rendering

`CLAUDE.md`, "Aprend": SpudGPU's plan is a caller-owned `spudgpu_framebuffer`,
designed and not built. When it exists, `aprend_framebuffer` holds and caches
it.

## Suggested order

Each step is usable on its own, and later ones lean on earlier ones.

1. Items 2, 4, 5, 7: small corrections with no design in them.
2. Item 9, then 10 and 11: what a shader can be given.
3. Items 12, 13 and 6: frames in flight and uploads.
4. Item 31: mip generation, which sampling already needs.
5. Item 32's first point, a small fix; then items 14, 16, 17 and the rest of
   32: compute, per-mip views and texture layouts.
6. Item 18, which settles items 1 and 3's API and unblocks 19.
7. Items 19, 20, 21: material, pass and draw list.
8. Items 25, 26, 29 as SpudGPU gains them; then 23 and 24.

Items 15, 22, 27, 28 and 30 wait until something needs them.
