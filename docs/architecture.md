# Architecture
---

## One header per file

**Rule:** each header includes *only* the next one down the chain.

Every header in `src/` includes exactly the one below it, so `main.cpp`
effectively pulls the entire engine into one compilation unit:

**Why?**
- To keep things simple and efficient.
- No include guards, no link-order problems, and one obvious dependency direction.

---

## Header Chain

dependencies:

| File | Purpose |
|---|---|
| `boilerplate.h` | Includes every external library; the game only needs this file at the start |

src:

| File | Purpose |
|---|---|
| `logger.h` | In-game console log |
| `loader.h` | Generated mesh primitives, texture loading, shader programs |
| `audio.h` | Sound effects and music |
| `window.h` | Window, input, timing, g-buffer, HDR targets, tonemap, audio device |
| `drawer.h` | Draw call preparation, light volumes, forward water |
| `physics.h` | Bullet world, rigid bodies, constraints, soft bodies |
| `player.h` | Capsule movement and the free fly camera |
| `fluids.h` | FFT ocean, foam field, short-chop normals, shallow water patch |
| `world.h` | Voxel world: multi-noise terrain, block volume, face mesher, collision mesh |
| `scene.h` | Scene content, shared builders, physics/render/water/world scenes, scene manager |
| `ux.h` | Console, scene selector, debug tools |
| `main.cpp` | Main loop |

---

## Frame pipeline

1. `GameRenderer::draw_shadows` renders the static world into the sun depth map with one indirect multi-draw per draw mode.
2. `GameRenderer::draw` fills the g-buffer (position, normal, albedo, depth); the flat-colour world is one `glMultiDrawElementsIndirect`.
3. `GameWindow::draw_lighting` runs the sun (Vogel PCF), ambient, specular occlusion, sky and fog into a linear HDR target.
4. `GameRenderer::draw_light_volumes` adds the point lights, one instanced volume per light.
5. `GameWindow::draw_sky_disks` adds the sun and moon over sky pixels.
6. `GameWindow::draw_clouds` raymarches a full-res 3D cloud slab and composites it as premultiplied alpha.
7. `GameRenderer::draw_water` copies the lit frame and draws forward water with refraction, absorption, sky reflection, sun specular, crest scattering, breaking shore foam and a CC0 foam lace mask.
8. `GameWindow::post_process` applies god rays, rain, bloom, exposure, ACES and gamma to the screen.
9. `debug_draw` appends physics, light-volume and scene-guide wires into one colour-coded overlay.

The console has one tab per concern: **Home** (health, frame budget, phase
breakdown, memory headline), **Scene**, **Render** (quality presets, pipeline
toggles, draw stats, mesh list, g-buffer inspector, camera), **Compute** (cloud
noise bake, dispatch size, bake cost, cloud march), **Threads** (worker pool,
water update and FFT timing, frame phases), **Memory** (every render target,
geometry buffer, shadow map and texture, plus totals), **Debug**, **Physics**,
**Player**, **Assets**, **Audio** and **Log**. A compact performance overlay is
drawn over the game view and reports FPS, frame/work milliseconds, all eight
frame phases, draw calls/instances/triangles, GPU targets/buffers/textures, the
process working set, the active worker count and the current FFT cost.

### Draw submission

Every flat-colour mesh (`tx_id == 0`) is packed into a persistent indirect
command buffer and submitted with one `glMultiDrawElementsIndirect` per pass.
The shadow pass does the same, filtering the view-model mesh out of the command
list. Textured props are few and can interleave textures, so they stay explicit
`glDrawElementsInstancedBaseVertexBaseInstance` calls. In the voxel world this
collapses several thousand chunk meshes into two scene draw calls (world +
water).

### Compute : cloud noise

`assets/shaders/cloud_noise.comp` bakes a 128^3 RGBA16F tileable volume once at
startup: R shape, G detail, B coverage mask and A warp, each a three-octave
value-noise fbm. The dispatch is 16 x 16 x 16 groups of 8^3 invocations and costs
about 2 ms on the reference GPU. `clouds.frag` samples the volume with linear
filtering and repeat wrap, which keeps the full-resolution raymarch cheap enough
without evaluating procedural fbm per step. The texture stays on the GPU; there
is no per-frame dispatch and no readback.

### Threads : water FFT

`water_ifft2` splits the row and column inverse transforms across up to eight
`std::thread` workers (spawn, stride rows, join, no queue or lifetime). Rows and
columns write disjoint cells, so no locking is needed. `fluid_parallel_enabled`
turns the fan-out off for comparison, and `Water::update_ms` / `fluid_fft_ms`
report the split cost in the Threads tab.

## Current scope

The project is now a rendering/physics/water foundation plus the first voxel
world slice: scene content, the deferred renderer, Bullet rigid and soft bodies,
the FFT ocean, the shallow-water patch and a finite block world. The procedural
city, building generator, hub, weapons scene and vehicle scene remain removed
from the build. Their research and source snapshots are under
`megastudy/procgen-city/` and `megastudy/terrain-archive/`.

Dead runtime code was removed with the scenes: the skinned/animation loader and
shaders, the weapon view model and projectiles, the vehicle controls, the static
no-uv draw mode, file mesh loading and the spot-light path. The renderer now has
two draw modes only: static + uv and forward water.

The scene list is: **menu**, **physics**, **render**, **ocean**, **shallow**,
**world**, **terrain**. The game starts in the menu backdrop. The menu has
**Play World**, **Scenes**, **Options** and **Exit**; scenes can be entered from
the menu or from the pause menu.

ESC no longer quits. While playing it opens a pause overlay with **Resume**,
**Options**, **Scenes**, **Main Menu** and **Exit**. The paused game keeps its
last rendered frame while simulation and camera input stop. Options currently
cover field of view, vsync and master volume.

`GameWindow` owns input and timing only; `ux.h` owns the menu/pause state and
scene selection. The scene manager remains the only thing that loads or unloads
a scene. Closing the window or choosing Exit shuts the engine down cleanly.

## Physics scene

`scene_physics` is a bench of labelled stations, one per Bullet feature. The
stations are inspectable, and a compact unit-test runner checks them
automatically after 2.6 s. The HUD shows `unit tests N/15`, and the Scenes tab
has a pass/fail table plus a `rerun tests` button.

Stations:

- primitive shapes (box, sphere, capsule, cylinder, cone)
- a stacked tower
- dominoes driven by a rolling ball
- a linked ragdoll
- a limited hinge
- a cone-twist bag
- a five-link chain
- a see-saw
- a continuous hinge motor
- Newton's cradle
- an inclined ramp
- CCD on/off spheres at a thin wall
- a motorised door
- a linear slider constraint
- a fixed constraint
- a wind-driven flagpole with a soft flag
- a low suspended soft cloth with a dropped ball
- a walk-through soft curtain in a doorway

The physics layer also exposes point-to-point, slider, fixed, hinge, cone-twist
and hinge2 constraints, raycasts, impulses, continuous forces and trigger
sensing. Motorised bodies are kept awake explicitly because Bullet motors do
not wake sleeping bodies themselves.

The flag, cloth and curtain are **real Bullet `btSoftBody` patches** in a
`btSoftRigidDynamicsWorld` with a `btSoftBodyRigidBodyCollisionConfiguration`
and `btDefaultSoftBodySolver`. Only the flag's hoist column is pinned, so the
cloth can billow; wind has a small lift component and no lateral component, so
it flutters without swinging around the pole. The cloth is pinned around its
perimeter, sits low and is deformed by a heavy rigid sphere. The curtain is
pinned along its top edge in a doorway and can be pushed through. Soft-rigid
collision uses the SDF path because `CreatePatch` has no clusters. Soft-body
materials are marked with `mat_b.b = 2` and the geometry shader draws a
distance-faded warp/weft/twill weave, slub and fibre variation, a herringbone
tonal stripe, folded hem, stitched seam and a darker back-face lining from the
grid UVs instead of sampling a texture. Their faces are rebuilt into a dynamic static-buffer mesh
each frame, with normals accumulated from the current triangle positions. The
deferred pass is double-sided so soft sheets are visible from either side.

The automated tests cover: shapes settle, stack stable, dominoes topple, hinge
swings, cone hang, chain links, see-saw tilt, motor turns, CCD stops, cradle
transfer, ramp roll, flag deformation, cloth deflection, slider travel and fixed
joint integrity.

## Render scene

`scene_render` remains the material lab: a studio preset, a neutral stage, a
metal/roughness matrix, ten named material spheres, local-light stress volumes,
a texture-vs-flat comparison and a live hero material edited in the Scenes tab.
It is a test bed for the deferred g-buffer and lighting, not gameplay content.

The studio preset uses a brighter sun and ambient fill than the old dark set,
and the lab surfaces sit at mid-grey so the material spheres and metal/roughness
matrix stay readable. Four coloured point lights sit just above the material
row; their bulbs are real props, so the lights have a visible source. Light
debug wires are deliberately sparse: a small source marker, a drop line and the
circle where the reach meets the stage top, drawn in the light's own colour. The
Debug tab and the Render scene's Scenes controls can toggle local lighting and
the wires.

## Rendering

The frame is deferred: a g-buffer of world position/metallic, normal/roughness and
albedo/AO, then one lighting pass with a key sun, a cool rim, PCF-shadowed direct
light, hemisphere ambient, specular occlusion and a
procedural environment for ambient and reflections. The environment is also the
sky: a per-pixel ray is reconstructed from the inverse view-projection so the
gradient, sun glow, haze and clouds are correct across the whole screen instead
of being a single sample.

The sun shadow uses a 16-tap Vogel spiral with a normal-offset receiver, so the
soft edge has no visible grid or linear tap pattern. Specular ambient uses its own
occlusion so reflections darken in a fold. SSAO is clamped to a floor and a
constant sky fill is added outside the AO term, so dense forest shade stays
readable instead of collapsing to black. Rough dielectrics suppress their ambient
and direct specular at silhouettes, which removes the bright outline around blocks.

The lit and scene-copy targets are RGBA32F, so the fog and shadow gradients do not
band before the tone map; the final pass adds a white-noise TPDF dither to cover
the remaining 8-bit steps. World blocks use flat albedo (the material flag that
would multiply the default debug checker texture is cleared).

The clouds are a genuine full-resolution 3D slab: domain-warped 3D fbm density,
a flat dense base and rounded tops, three short marches toward the sun for
self-shadowing, powder on thin edges, and drift with time. The quality preset
controls the raymarch step count (32 / 64 / 96 / 128); the default quality setting
is dense enough that the slab is clean without a blur or a visible dither. The
result is composited as premultiplied alpha before water. A weather uniform
expands the deck, darkens the bases and feeds a soft screen-space rain veil, so
storm cells can move in and cast changing god rays. No cloud texture is involved.

The sky is a single-scattering atmosphere: Rayleigh blue plus a Mie forward halo,
with the optical depth growing toward the horizon and a warm dusk grade when the
sun is low. The sun and moon are analytic HDR disks with limb darkening and a wide
halo, drawn over the sky. A final radial god-ray pass marches from each pixel
toward the sun's screen position through the depth buffer, so terrain and trees cut
the shafts.

Post does a small bloom chain before the tone map: a half-resolution bright pass,
a separable Gaussian blur ping-ponged through two HDR targets, then the bloom add
plus ACES, contrast, saturation, a gentle vignette and dither. The world and
terrain scenes use the trade-winds daylight preset (warm sun, blue sky) with a
reduced fog density; the lab presets keep their own skies and exposure.

A waterline pass runs after the water and before bloom: it reconstructs the scene
depth and view ray, finds the water-plane intersection and tints the part of the
frame below it. A fully submerged eye gets the whole frame tinted with a small
screen-space wobble; a half-submerged eye gets the tint only below the seam, so
the above-water half keeps its normal exposure. The underwater amount fades in
over roughly half a metre of eye height, so walking into water does not look like
a different shader switching on.

## Water scenes

There are two water paths:

- **ocean**: a CPU FFT patch tiled into open sea, with a dynamic foam field and
  a short-chop detail normal map.
- **shallow**: a small finite-difference patch in a smooth sand bowl, framed by a
  block beach, rocks and a few wet stones.

Both use the forward water pass with refraction, per-scene absorption, sky
reflection, sun specular, crest scattering and foam. The shallow path adds a fine
procedural ripple layer that needs no normal texture and a narrow contact-foam
line where the depth buffer meets the surface. The water is clear until the path
through it is long: the refracted bottom is attenuated per channel, then the body
colour is added by in-scattering. A shallow pool therefore shows its sand almost
unchanged, while deeper water gradually turns blue. Water absorption and chop are
uniforms, so a lagoon, a shoreline and the open sea each get their own clarity
without a second shader.

Depth is communicated by the material itself rather than by a decal: per-channel
absorption removes red first, the in-scattering colour ramps from turquoise in the
shallows to the authored deep blue, fine ripples calm down as the water deepens,
and the underwater fog closes in as the eye drops. The surface also traces a
screen-space reflection through the opaque depth buffer, so a pool reflects the
wall and rocks around it instead of only the sky; a miss falls back to the
environment. That reflection, the refracted bottom and the absorption gradient
all read as depth at once.

The reflection is physical Fresnel: nearly transparent when looked into from
above, glassy toward the horizon. The screen-space hit is weighted by a
thickness/edge confidence and rejected if it lands below the water line, so the
submerged floor cannot smear across the surface; misses fall back to the analytic
sky. A small amount of the body colour is mixed back into the reflection. Fine
capillary normals break the reflection into facets instead of one smooth sheet.
Crest scattering adds the blue-green glow where the sun backlights an ocean wave;
the shallow patch keeps it very low.

Caustics are intentionally not part of the water pass. The refracted bottom is
the sand's own lit colour, not a pattern drawn over it.

The forward water pass is double-sided: the sea is one layer of triangles, so
from below and wherever a choppy crest folds over itself the back faces are
still water. Culling them punched holes that showed the sky disk or the dark
seabed through the surface. Surface normals are clamped above the horizon, so a
steep facet can never reflect the dark ground hemisphere into the sea, and the
reflection environment contains no hard sun/moon disk: the disk belongs to the
sky pass, while water gets a broad scatter plus a GGX glint that widens with
distance. Fog over water always samples the sky hemisphere.

Both water scenes feed the same water uniforms into the renderer and keep their
own Scenes controls.

## Voxel world

`world.h` is the ground zero for the voxel game. The playable scene streams
16 x 16 chunks in a 33 x 33 view around the player (a 256 m radius, 1089 loaded
chunks), so the world has no finite edge. Each chunk stores a 16 x 96 x 16
`uint8` block volume and is generated from layered noise:

- **continentalness** : low-frequency land/ocean mask
- **erosion** : high values flatten the land, low values leave relief
- **ridges** : folded `1 - |noise|` bands for peaks and valleys
- **detail** : small-scale roughness, scaled down by erosion
- **domain warp** : warps the sample point so the grid does not show

This follows the shape of Minecraft's modern multi-noise terrain (continentalness,
erosion, weirdness/ridges and peaks-and-valleys) without copying its density
functions, aquifers or cave carvers. The broader ideas come from fBm/ridged-noise
terrain papers and terrain mods such as TerraForged, Tectonic and ReTerraForged:
large landmasses, visible erosion contrast, long ridges and sparse high peaks.

Blocks are a flat palette with one colour and roughness each: grass, dirt,
stone, sand, snow, wood, leaves, planks, dry grass, red sand, pine leaves, jungle
leaves, tall grass and flowers. There are no textures. Each chunk meshes only its
exposed faces and groups them by block type. Tall grass and flowers are non-solid
crossed quads, so they neither block movement nor cull the cube faces behind
them. The sea uses the forward water pass with a flat plane over the loaded view;
it refracts the opaque frame, absorbs with depth and reflects the sky.

Biomes sit on top of the same height, temperature and humidity fields: ocean,
beach, plains, forest, taiga, jungle, savanna, swamp, desert, badlands, snowy and
mountain. Surface blocks, tree type and tree density follow the biome: pines in
the taiga, tall jungle trees, flat acacia-like crowns in the savanna, sparse
swamp oaks, no trees in the desert or badlands. Two intersecting 3-D noise fields
carve winding caves below the surface. Tall grass and flowers are seeded on the
grassy surfaces, with a density that follows the biome.

`scene_world` is the playable scene. The view is an infinite square of chunks:
33 x 33 chunks (1089) at a radius of sixteen. Crossing a chunk boundary unloads
the far strip and loads the near one. Chunk geometry lives in a byte arena at the
end of the static buffer, with one reserved mesh_info block per chunk pool slot
and separate free lists for vertices and indices. A crossing therefore rebuilds
only the new chunks and the boundary chunks whose neighbour set changed; the rest
of the world keeps the geometry already on the gpu. Generation and meshing are
budgeted to a couple of chunks per frame, so the frame time stays flat while a
new strip fills in. A full rebuild (opening the scene, or an arena exhaustion)
still regenerates everything in one go.

The player spawns on a level 3 x 3 site with a walkable surface and no tree in
it, then faces the lowest cardinal direction so the first view looks out across
the landscape. The spawn clearing is kept tree-free. Collision is a custom voxel
AABB sweep against the live block grid, with a one-block step-up, so the player
never runs off the generated area and edits collide immediately.

`scene_terrain` is a free-fly view over the same infinite chunk world. It uses
the identical 1 m block grid, biome rules, caves and trees, with live controls
for seed, sea level, continental scale, erosion, ridges, amplitudes and domain
warp. The whole world is shifted down by the sea level for this scene, so the
water line sits at world y = 0. Flying with WASD streams chunks around the
camera exactly like the playable scene; a wide water plane carries the horizon
and light haze only softens the far edge. A slider change drops every chunk and
regenerates the streamed volume with the new settings. The world scene reuses the
same generator.

The world scene also carries the first playable layer. The first-person view is
one right arm at Steve's own 4 x 12 x 4 pixel proportions (0.25 m square, 0.75 m
long) with the upper sleeve section; F5 shows the full 32 pixel
Steve model (head 8^3, torso 8 x 12 x 4, arms and legs 4 x 12 x 4). The inventory
on E reproduces the vanilla 176 x 166 slot layout with the 2 x 2 crafting grid
and result, four armour slots, an offhand slot, 27 storage slots and the hotbar.
It is drawn flat and dark with the pause menu's warm orange accents, and it does
not include a 3D player preview. Slots can be dragged: click to pick up a stack, release over
another slot to move or merge it. One log in the crafting grid yields four
planks. Number keys select a hotbar slot. Left click breaks the block under the
crosshair and spawns a small item cube that falls, rests on the ground and is
picked up when the player walks over it; right click places the selected block if
the cell is empty and does not intersect the player. A wire box previews the
placement cell. Both edits mark only the edited chunk and any neighbour across
the border and remesh those, so a block change does not rebuild the view.

Torches and light blocks are ordinary hotbar blocks. Every world frame, the scene
scans a 12 m box around the eye for emissive blocks, adds each one as a local
`Point_Light` volume and draws an emissive cube overlay, so the local lighting can
be tested immediately even before the chunk remesh lands.

The underwater composite is gated on the eye actually being in water: the world
scenes compare the eye against the generated terrain height, and the sea plane is
hidden when the camera drops below sea level over dry land. Flying down over a
valley therefore no longer looks like a dive.

## Removed systems

Removed from the active code and project files:

- procedural city graph, zoning, occupancy plan and streaming
- procedural building generator and preview
- city and building shaders, city draw modes and mesh entry points
- hydraulic/traffic/pedestrian/signal simulation
- hub, weapons and vehicle scenes
- terrain heightfield world generation

`src/world.h` is now the active voxel world. The old heightfield terrain is
archived under `megastudy/terrain-archive/world.h`. The city snapshot, shaders,
diff and notes are under `megastudy/procgen-city/`.

## Conventions

- Plain structs, free functions, tables and explicit switches. No inheritance or virtual functions.
- Scenes own their content and are rebuilt from scratch on a switch.
- Every scene has its own HUD and its own debug controls.
- Comments are short and standalone; they describe what the code does now.
