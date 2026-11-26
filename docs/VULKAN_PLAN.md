# Vulkan renderer — plan

Goal: a second renderer on Vulkan next to the OpenGL ES one, chosen in the launcher
(**RENDERER: OPENGL / VULKAN (EXPERIMENTAL)**), with OpenGL as the fallback when Vulkan cannot start.

Expected gain (measured baseline, Snapdragon 8 Gen 2, 1080p Ultra, battle): render thread ~12 ms → 5–6 ms a frame
(far less driver work per call), GPU 10–20% less (explicit load/store of render targets on a tiled GPU). The game
stays at its 30 fps; the gain is heat, battery and headroom for weaker phones.

## How the renderer is built today

- `d3d9.cpp`, `d3d9_dev.cpp`, `d3d9_fx.cpp`: the Direct3D 9 / D3DX emulation. Device state lives in `dev`; textures
  and buffers are `Obj`s whose contents are versioned (`ver`) and pulled into the GPU lazily at draw time.
- The renderer's API (`d3d9.h`, namespace `glr`) is small: `init`, `ready`, `clear`, `draw`, `present`,
  `resizeBackBuffer`, `stretchRect`, `destroy`, `rtSize`, `frameNo`, `targetIsBack`.
- `d3d9_gl.cpp` implements it on OpenGL ES 3, and mixes two kinds of code:
  - Direct3D-level logic that any renderer needs: HUD scaling (`uiScaleFor`, `uiUnmapImpl`), DXT decoding and the
    ETC2 path, draw merging decisions, the wide-screen side bars, health reports, frame traces, resource reclaim;
  - OpenGL calls.
- `d3d9_sh.h` translates shader bytecode to GLSL; `glrec.*` records GL calls on the game threads and replays them
  on the render thread.

## Stages

### 0. Separation (no Vulkan yet)
- Renderer choice: `rt::config.renderer` (launcher option), a dispatcher in front of the `glr` API.
- Move the Direct3D-level logic out of `d3d9_gl.cpp` into shared files, so the Vulkan renderer reuses it instead of
  copying it: HUD scaling, texture decoding, draw merging signatures, side bars, health/trace helpers.
- The game must look and run exactly as before (checked with the usual profile runs).

### 1. Vulkan starts
- Instance, device, swapchain on the Android window; clear, present with the letterbox / side bars, the on-screen
  buttons. Falls back to OpenGL when initialisation fails (logged).

### 2. Shaders
- The translator gets a Vulkan dialect (`layout(set, binding)`, y flipped by a negative viewport height instead of
  `u_flip`); GLSL → SPIR-V with glslang built into the app.
- Shader constants in a ring of uniform buffers (dynamic offsets), textures through descriptor sets (push
  descriptors where available). SPIR-V and `VkPipelineCache` cached on disk.

### 3. Resources
- Textures: DXT as BC where supported (Adreno), the existing ETC2 / RGBA8 paths elsewhere (Mali); uploads through a
  staging ring.
- Vertex / index buffers: static buffers, persistently mapped rings for dynamic ones.
- Render targets and depth buffers in the formats the game asks for.

### 4. Pipelines
- Direct3D 9 state (blend, depth, stencil, cull, vertex layout, target formats) → pipeline key → cached pipeline;
  viewport, scissor, depth bias, stencil reference and blend constant as dynamic state, extended dynamic state where
  the driver has it.
- Milestone: menus, the Flash interface and videos render through Vulkan.

### 5. Render passes
- Dynamic rendering with explicit `loadOp` / `storeOp` from what the game clears and what is not needed after a
  frame; StretchRect, mip generation, the bloom half-size copy, queries.
- Milestone: battles render through Vulkan.

### 6. Parity with OpenGL
- Pre-rotation: render in the display's natural orientation (`preTransform` = the surface's current transform) so
  the compositor does not have to turn every frame (stage 1 presents unrotated; the driver reports SUBOPTIMAL).
- HUD scaling, blurred menu side bars, tooltips, screenshots / dumps, GPU timing (timestamp queries).
- Compare the same scenes with OpenGL: bloom, water, shadows, fog of war.

### 7. Speed and testing
- Same battles with `ra3_prof` on both renderers; Mali devices when testers are found; released as an experimental
  option. Vulkan becomes the default only after a few releases without problems.

## Risks
- Vulkan driver bugs on older phones → OpenGL stays available and is the fallback.
- First-run pauses while pipelines are built → disk caches.
- APK a few MB larger (glslang).

## Progress
- [x] Plan
- [x] 0. Separation — HUD scaling, side bars, on-screen buttons (`d3d9_ui.cpp`), DXT decoding and cache files
  (`d3d9_tex.cpp`), renderer choice (`renderer=` in launcher.cfg or `/sdcard/ra3_vulkan`, decided at the first window
  in `gfx_gles.cpp`)
- [ ] 1. Vulkan starts — written (`vkb.cpp`: device, swapchain, black frame, console picture, buttons as clears,
  fallback to OpenGL ES); waiting for the test on a phone. The launcher option comes once the game draws something.
- [ ] 2. Shaders
- [ ] 3. Resources
- [ ] 4. Pipelines
- [ ] 5. Render passes
- [ ] 6. Parity
- [ ] 7. Speed and testing
