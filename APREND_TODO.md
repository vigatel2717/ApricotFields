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
- **A second set of changes on 2026-10-10 is written, not compiled**, on any
  platform. The numbers are the items they closed:
  - Vertex, index and uniform buffers ask for host-visible and host-coherent
    memory, as storage buffers do (was item 2).
  - Desc fields nothing read are gone (was item 4): `store_locally`,
    `initial_data` and its pitches on both texture descs, `_line_width` on the
    pipeline desc, `swap_chain_target` on the framebuffer desc, and the app
    and engine names and versions on `aprend_instance_desc`, which is now the
    device alone. A view's `APREND_TEXTURE_VIEW_TYPE` is checked against the
    texture's usage bits, and a sampled view of a depth and stencil format is
    of the depth alone.
  - `_vertex_entry_point` and `_fragment_entry_point` on the pipeline desc,
    required and copied (was item 5).
  - The texture update and readback calls and the framebuffer clears take the
    queue they submit on (part of item 6).
  - Removed: the unused SPIR-V loader, the instance's unused command list, the
    forward declaration of a pipeline layout type, three unused includes.
    `apricotfields.h` includes all seven Aprend headers (part of item 7).
  - Push constants (was item 10): `_push_constant_ranges` on the pipeline
    desc and `APREND_COMMAND_PUSH_CONSTANTS`.
  - Vertex bindings (was item 11): `_vertex_bindings` replaces
    `_vertex_layout`, each binding per vertex or per instance, read from the
    vertex buffer slot of the same number. A draw fails the compile if a slot
    its pipeline reads is empty or holds a buffer of another stride.
  - A pipeline is set inside a pass and lasts until the pass ends.
    `SET_SHADER_PIPELINE` outside a pass, and a draw in a pass that has set
    none, fail the compile. Before, a pipeline carried from one pass to the
    next in Aprend's tracking while the backend had none bound.
  - A pass takes only the binding sets its draws read (was item 32's first
    point).
  - A uniform buffer is sized from its uniforms' offsets, with std140's array
    stride, and `aprend_uniform_buffer_update_by_name` writes an array's
    elements at that stride (item 1's small fix).
  - Texture update and readback take a mip level, and the 2D ones an array
    layer (part of item 3).
  - Uploads follow the device's memory (item 13). Vertex, index and
    storage buffers are host-visible and written directly on a device whose
    memory is the system's. On a device with its own they are device-local,
    with a host-visible staging buffer beside each; a write goes to staging,
    and `aprend_command_list_submit` copies the dirty range across in an
    upload list submitted ahead of the list's own. Uniform buffers are
    host-visible on every device. `aprend_instance_create` reads
    `SPUDGPU_DEVICE_PROPERTIES::unified_memory`, and the copy uses
    `SPUDGPU_RESOURCE_STATE_COPY_DEST`: both are new in SpudGPU and equally
    unbuilt. The vertex and index update calls check their range.
  - Frame pacing (item 12). The instance has a fence; every submission
    Aprend makes (command lists and the immediate operations) signals it
    through `spudgpu_queue_submit`, and `aprend_instance_next_frame` waits on
    it for the frame that last used the index. `aprend_instance_wait_idle`
    waits for everything submitted so far. It rests on SpudGPU's fence being a counter on every backend,
    which was rewritten the same day and is equally unbuilt.
  - Buffer sets (item 12). `aprend_instance_desc::frames_in_flight`, a frame
    index moved on by `aprend_instance_next_frame`, and
    `aprend_uniform_buffer_set` and `aprend_storage_buffer_set` with one copy
    for each frame. A binding set entry takes a set in place of a buffer; such
    a binding set keeps a descriptor set for each frame, and a command list
    binds the one of the frame it is compiled in and is refused at submit in
    any other.
  - None of it has run: no push constant has been set, no per-instance buffer
    read, no mip below 0 written, and no staged buffer copied: every device
    this has been near is unified, so the staged path is the least tested
    code in Aprend.
- **No tests.** There is no `tests/aprend_tests.*`: the repo's tests are
  headless with no GPU, and nothing in Aprend runs without a device.

## Wrong things

### 1. Uniform layouts are declared by hand

`aprend_uniform_buffer_create` sizes the buffer from the caller's offsets, with
std140's 16-byte stride for an array, and places nothing. What is left is that
the offsets, types and counts are the caller's to get right, and nothing checks
them against the shader:

- A uniform declared at the wrong offset is read from the wrong bytes.
- A `mat3`, a struct, or an array of structs has no `APREND_UNIFORM_TYPE`.
- A `vec3` followed by a scalar shares its 16 bytes in std140, which a caller
  working from sizes alone gets wrong.

The fix is SPIR-V reflection: layouts, offsets and block sizes read from the
shader. It belongs with the rewrite `aprendbuffers.h` already says it is due
(item 18).

### 2. A host-visible storage buffer on D3D12

On a device whose memory is the system's, a storage buffer is host-visible
(see "State of the code"). SpudGPU's D3D12 backend can't give a buffer both
storage usage and host-visible memory, so on an integrated GPU under D3D12 a
storage buffer either fails to create or can't be written by a shader. Aprend
can't tell: it doesn't branch on the backend, and SpudGPU reports nothing for
it. It needs SpudGPU to say whether a host-visible storage buffer is possible
on the device, and then such a device takes the staged path for storage
buffers alone.

### 3. Nothing fills a texture's mips but the caller

A texture created with `mip_levels` 0 gets every level, and each starts
undefined. `aprend_texture*_update` writes any one level, so a caller with its
own mip data can fill the chain. A caller without it can't: nothing generates
a level from the one above (item 31). Until then such a texture wants
`mip_levels` 1, or a sampler whose `max_lod` is 0.

### 6. Immediate operations block

`aprend_submit_immediate` (texture update and readback, framebuffer clears)
submits on the queue its caller passes in, as one of the instance's counted
submissions, and waits on the instance's fence for it. It still makes a new
command allocator and list for every call, and it still blocks: a readback
has to, but an update or a clear could be left to finish with the frame once
there is a release queue (item 12) to destroy its staging buffer and list
afterwards.

### 7. The default shaders are unused

`shaders/apricot_default.vert` and `.frag` are compiled by the build on Windows
and Linux and loaded by nothing. Either they go, with the glslc block in
`CMakeLists.txt`, or something uses them; which depends on item 22.

### 8. Older-generation habits still to convert

Per `CLAUDE.md`, only when the function is already being changed: joined
argument checks (`if (!instance || !desc)`), failure reported by `printf` with no
result code, a few unchecked `malloc`s (the layout copy in
`aprend_vertex_buffer_create`, the file buffer in
`aprend_shader_read_from_file_spirv`), and `std::vector` growth inside C entry
points with nothing catching `std::bad_alloc`.

Unchecked in the command list: `SET_INDEX_BUFFER` with a NULL buffer is
dereferenced at compile. Vertex buffers, the index buffer, viewports and
scissor rects sent before a pass are passed to SpudGPU where they stand,
outside the pass, and whether they are still set inside it is the backend's.

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
- **A combined image sampler slot doesn't work on SpudGPU's Metal backend**,
  which writes the texture half only. A layout that has to run there declares
  a sampled image slot and a sampler slot instead. Aprend accepts the combined
  type on every backend and says nothing.

### 10. Push constants: what is left

A pipeline declares ranges and `APREND_COMMAND_PUSH_CONSTANTS` writes them (see
"State of the code"). The bytes are written for the pipeline bound then and
are not tracked: after another `SET_SHADER_PIPELINE` the caller sends them
again. Only a graphics pipeline has them until item 14.

### 11. Vertex bindings: what is left

A pipeline declares up to `APREND_MAX_VERTEX_BINDINGS` bindings, each per vertex
or per instance (see "State of the code"). Left over:

- **No matrix element type.** A per-instance `mat4` is four `VEC4` elements,
  at four locations.
- **A per-instance binding steps every instance.** SpudGPU's binding has no
  step rate, so one element for every N instances can't be asked for.
- **Locations are positional**: element N across the bindings is location N.
  A shader that skips a location can't be described. Reflection (item 18)
  replaces this.

### 12. Frames in flight: what is left

Buffer sets give each frame in flight its own copy of a uniform or storage
buffer (see "State of the code"), so the frame being written doesn't share a
buffer with one the GPU is reading. Left over:

- **Pacing counts only what goes through Aprend**, on one queue. Every
  submission is made by `aprend_instance_submit`: command lists, and the
  immediate operations, which then wait for their own serial. Each signals
  the instance's fence to the next serial, and `aprend_instance_next_frame`
  waits for the last serial of the frame that used the index before. Work a
  caller submits to SpudGPU itself is outside it. Submitting one instance's
  work on two queues breaks the order the serials assume, and nothing checks
  for it.
- **The swap chain still waits for its queue to go idle** before a resize or
  a destroy. That is every submission on the queue, the caller's included,
  which is what replacing back buffers needs; `aprend_instance_wait_idle`
  would cover Aprend's alone.
- **The wait has no time limit and no way to report failure**:
  `aprend_instance_next_frame` returns the index either way and prints.
- **Nothing defers destruction.** Every destroy call still requires the
  caller to know the GPU is finished with the object. Wanted: a release queue
  on the instance, each entry tagged with the submit serial current when it
  was retired and destroyed once the fence has passed it. The fence is there
  now; the queue isn't. It also frees a staged buffer's staging copy
  (item 13) and lets a binding set be rewritten in place (item 9).
- **Single buffers are as they were.** A uniform or storage buffer that isn't
  in a set, and every vertex and index buffer, is still one allocation that
  must not be written while a submitted frame reads it. There is no vertex or
  index buffer set; a caller that rewrites geometry every frame keeps one
  buffer for each frame itself.
- **A copy is not carried forward.** A copy holds what was written to it
  `frames_in_flight` frames ago. A caller that updates part of a set each
  frame reads stale bytes in the rest. Hazel's sets behave the same; a set
  that copied forward what wasn't rewritten would hide the cost.
- **A list that binds a per-frame set is compiled every frame.** It reads the
  frame's copies through that frame's descriptor set, fixed at compile. A set
  bound through a dynamic offset would lift this; SpudGPU has none.
- **Descriptor sets multiply**: a binding set with a buffer set in it takes
  `frames_in_flight` sets from the layout's pool, 32 to a pool.

Hazel: `UniformBufferSet`, `StorageBufferSet`, `Renderer::SubmitResourceFree`,
`RendererConfig::FramesInFlight`.

### 13. Uploads: what is left

Vertex, index and storage buffers follow the device's memory (see "State of
the code"). Left over:

- **A staged buffer keeps its staging copy for life**, so on a device with
  its own memory a buffer written once costs its size twice. The staging
  buffer can go once its copy has run, which nothing can know until item 12's
  fences. A shared staging buffer for many small writes waits on the same.
- **A write must not be made while submitted work that uses the buffer is
  unfinished.** On a unified device the write lands under the GPU; on a staged
  one the copy that hasn't run yet reads the new bytes. The same rule either
  way, and item 12 is what lifts it.
- **One dirty range per buffer.** Two small writes far apart copy everything
  between them. Correct, and more than is needed.
- **Uniform buffers stay host-visible on every device**, on purpose: they are
  small and rewritten every frame, and a staged copy per buffer per frame
  costs more than the bus read it saves. A large uniform buffer written once
  would be better staged; nothing asks for it.
- **Texture uploads are untouched**: each still creates a staging buffer, a
  command allocator and a command list, submits and blocks (item 6).
- **A compiled list holds a pointer to each staged buffer it uses**, as it
  does to each texture (item 32). Destroying the buffer and then submitting
  the list is a use after free.
- **How SpudGPU's memory flags land**, for whoever touches this next:

| Flags | Vulkan | D3D12 | Metal |
|---|---|---|---|
| `DEVICE_LOCAL` alone | Required property; the device's own memory | Ignored; default heap | Ignored; private storage |
| `HOST_VISIBLE \| HOST_COHERENT` | Required properties; system memory on a device with its own | Upload heap; coherent ignored | Shared storage; coherent ignored |
| All three | All three required; no such memory type on some devices, and the buffer fails | As the row above | As the row above |

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

### 17. Layout tracking is per whole texture

The command list tracks a layout for each 2D and 3D texture it uses: pass
attachments, the present source, and the textures of a pass's binding sets. It
is one layout for the whole texture, so a texture can't be sampled at one mip
while another is rendered to, and a texture can't be both in a pass's sets and
one of its targets. Becomes per subresource with item 16. Compute (item 14) has
no pass to move its textures before, and needs its own point to do it.

### 32. Texture layouts: what the command list gets wrong or can't do

Numbered out of order, as item 31. Item 17 is the one limit that needs a
redesign; these are the rest, all in `aprendcommands.cpp`. A pass takes only
the sets its draws read (see "State of the code").

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

### 31. Mip generation

Numbered out of order so the other items keep their numbers. It was listed as
a gap Aprend could close with `spudgpu_cmd_blit_image`, level N into level
N + 1 at half the size. It can't yet: only SpudGPU's Vulkan backend resamples
in a blit. The Metal and D3D12 backends copy regions of one size and refuse
the rest, so a call built on it would do nothing on two backends of three.

Two ways it gets built:

- **SpudGPU's blit resamples on every backend.** Then, per level N and for
  every array layer: N to `TRANSFER_SRC` and N + 1 to `TRANSFER_DST` with
  `spudgpu_cmd_image_barrier_subresource`, a blit at half the size and never
  below 1 (a 3D texture halves its depth too), and the whole chain to one
  layout at the end. Integer and depth formats can't be blitted with a linear
  filter everywhere, and SpudGPU has no query for it.
- **Aprend draws each level**: a pass per level that samples the one above.
  It needs a view of one mip (item 16), layout tracking per mip (item 17) and
  a shader Aprend ships, which is item 22's open question.

Either way there are two forms, both asked for by the caller and never done
by `aprend_texture*_update` on its own: an immediate one for a texture filled
from the CPU, and a command outside any pass for one just rendered to. The
filter is an argument.

Hazel: `Texture2D::GenerateMips`.

## Suggested order

Each step is usable on its own, and later ones lean on earlier ones. Items 4
and 5 are closed and their numbers are not reused.

1. Compile what is written and run it: nothing since 2026-10-09 has been
   built, in Aprend or in the SpudGPU changes it now depends on. Run it on a
   device with its own memory as well, for the staged path.
2. Item 12's release queue, on the fence that is now there. That closes
   most of what is left of 13, and item 6's blocking texture uploads follow
   from it.
3. Items 14, 16, 17 and the rest of 32: compute, per-mip views and texture
   layouts.
4. Item 18, which settles item 1 and unblocks 19.
5. Items 19, 20, 21: material, pass and draw list.
6. Items 25, 26, 29 and 31 as SpudGPU gains them, and item 2; then 23 and 24.

Items 7, 15, 22, 27, 28 and 30 wait until something needs them or decides
them.
