# VFX Engine Flow

This note records the Phase 3 VFX runtime/render structure and gives Phase 4 a
clear starting point for splitting Particle, Trail, Beam, and Distortion assets,
runtime inputs, and renderer inputs.

## Current Flow

```text
EffectAsset
  -> EffectRuntime
  -> EffectRuntimeFrame
  -> AppVfxRenderPipeline
  -> ParticleRenderer / TrailRenderer / BeamRenderer / DistortionRenderer
```

## Responsibilities

### EffectAsset

`EffectAsset` is the authoring-side definition. One asset can own multiple
`EffectComponentAsset` entries.

`EffectComponentAsset::common` holds data shared by all component types:
component id, type, technique, renderer type, simulation type, pass state,
timing, color, and size.

`EffectAsset` carries authoring defaults as type-specific settings:

- `defaultParticle`
- `defaultTrail`
- `defaultBeam`
- `defaultDistortion`

`EffectComponentAsset` carries one common block plus an explicit payload wrapper:

```cpp
EffectComponentAsset
  common
  payload
```

`EffectComponentPayload` is a `std::variant` over typed settings:

- `EffectParticleSettings`
- `EffectTrailSettings`
- `EffectBeamSettings`
- `EffectDistortionSettings`

The typed asset views, typed collection helpers, typed lookup helpers, and
`EffectComponentAssetBuilder` live in `application/EffectComponentViews.h`.
`EffectSystem.h` keeps the core asset data types, while the helper header is the
compatibility boundary around the current packed component storage.

`EffectAsset` owns components through `EffectAssetComponentStorage`. The storage
is still backed by `packedComponents_`, a compatibility vector of packed
`EffectComponentAsset` entries, but callers should go through
`EffectAsset::Components()` / `EffectAsset::MutableComponents()` and the typed
helpers instead of reaching for the backing container. This keeps the next
storage split local to the asset/helper boundary.

`EffectAssetComponentStorage` exposes typed storage views as the next split
point:

- `ParticleStorageView()` / `MutableParticleStorageView()`
- `TrailStorageView()` / `MutableTrailStorageView()`
- `BeamStorageView()` / `MutableBeamStorageView()`
- `DistortionStorageView()` / `MutableDistortionStorageView()`

Const typed storage views read the typed storage vectors. Mutable typed storage
views are replace handles for authoring and live tuning; they intentionally do
not expose mutable component pointers. New authoring and live-tuning writes
should read through const typed views, copy a typed component asset, and commit
it through the matching replace API. This lets runtime lookup and iteration move
to typed storage while keeping packed mutation behind an explicit
synchronization boundary.

The storage also has empty typed storage members reserved for the physical split:

- `particleComponents_`
- `trailComponents_`
- `beamComponents_`
- `distortionComponents_`

`EffectAssetComponentStorage::Add()` now has typed overloads for
`ParticleComponentAsset`, `TrailComponentAsset`, `BeamComponentAsset`, and
`DistortionComponentAsset`. New component creation should enter through those
typed assets first; the storage then synchronizes the packed compatibility
entry. Const runtime reads can use typed storage, while `packedComponents_`
remains the compatibility storage source until the physical typed storage split
is complete. Loader completion and `EffectSystem` registration normalization
call `SyncTypedStorageFromPackedForNormalization()` through friend-only
normalization access because those layers own asset construction and
normalization.

The formal typed read surface on storage is:

- `ComponentCount()`
- `ForEachComponentCommon(visitor)`
- `ParticleStorageView()` / `TrailStorageView()` / `BeamStorageView()` /
  `DistortionStorageView()`

The loader keeps the component currently being parsed in a typed edit buffer:

```text
ComponentEditBuffer
  std::variant<
    ParticleComponentAsset,
    TrailComponentAsset,
    BeamComponentAsset,
    DistortionComponentAsset>
```

Component keys are applied to that typed buffer, and the buffer is committed
through typed `Add(...)` when the component block ends. This keeps `.effect`
authoring from editing `EffectComponentAsset` in packed storage directly.

`EffectSystem::EnsureDefaultComponent()` follows the same direction for
registered assets that still arrive with packed compatibility components:

```text
EffectComponentAsset (const packed compatibility read)
  -> ComponentNormalizationBuffer
  -> normalize EffectComponentCommon
  -> Replace*ComponentAtForAuthoring(...)
  -> SyncTypedStorageFromPackedForNormalization()
```

This keeps registration normalization from mutating packed components through a
non-const `EffectComponentAsset&`. The index-based authoring replace exists only
to handle legacy components whose id may be assigned during normalization.
Registration normalization batches those index replacements and synchronizes
typed storage once after the batch.

Mutable typed storage migration should use replacement APIs instead of directly
editing packed entries:

- `ReplaceParticleComponentAndSyncPacked(mutableParticleStorageView, component)`
- matching Trail/Beam/Distortion helpers

The typed replace helpers now update the matching typed component vector first
and then synchronize the packed compatibility entry. The old generic packed
replace router has been removed; live editing should enter through typed
component assets. This makes the typed storage members behave like the future
primary component storage instead of passive copies rebuilt after every edit.

Legacy direct-edit helpers such as `ForEachMutable*Component` and
`Mutable*Components` have been removed. The remaining compatibility APIs are:

- `Add(EffectComponentAsset)`: packed import compatibility only.
- `Mutable*StorageView()` plus `Replace*ComponentAndSyncPacked(...)`:
  authoring/live-tuning replacement handles.

The packed normalization API is no longer part of normal storage usage:

- `HasPackedComponentsForNormalization()`
- `PackedComponentCountForNormalization()`
- `PackedComponentAtForNormalization(index)`
- `Replace*ComponentAtForAuthoring(...)`
- `SyncTypedStorageFromPackedForNormalization()`

These are friend-only hooks for `EffectAssetLoader` and `EffectSystem`
normalization. Runtime, renderer, authoring UI, and live tuning should not use
them.

`EffectAssetComponentStorage::Find(uint32_t)` and packed iteration are no
longer public APIs. Runtime queue building already resolves components through
`FindParticleComponent`, `FindTrailComponent`, `FindBeamComponent`, and
`FindDistortionComponent`. Instance construction and lifetime checks also walk
typed storage views, so packed component iteration is confined to
construction/normalization internals.

Typed component asset structs describe the target owned shapes:

- `ParticleComponentAsset`
- `TrailComponentAsset`
- `BeamComponentAsset`
- `DistortionComponentAsset`

Each typed component asset owns `common + settings`. `EffectComponentAssetBuilder`
is the construction boundary used by the loader and `EffectSystem`: it creates
`EffectComponentCommon`, pairs it with the matching typed default settings, and
then calls `ToEffectComponentAsset` only at the final pack step. That pack step
sets `common` and stores the matching settings type in the payload variant.

Typed `.effect` keys are the preferred authoring surface:

- `particle.emissive`, `particle.noiseStrength`, `particle.spawnRadius`
- `trail.depthFadeSoftness`, `trail.tailFade`
- `beam.emissive`
- `distortion.strength`, `distortion.noiseStrength`, `distortion.depthAttenuation`

Legacy flat keys such as `emissive`, `distortion`, `noise`, `uvScroll`,
`depthFadeSoftness`, `particleEdgeSoftness`, and `trailTailFade` are still
parsed as compatibility input. At asset scope they map into typed defaults; at
component scope they are constrained to the current component type.

#### `.effect` Authoring Keys

Asset scope common keys:

- `name`
- `shader`
- `texture`
- `blend`
- `layer`
- `lifetime`
- `renderQueue`
- `size`
- `color`

Component scope common keys:

- `component`
- `technique`
- `name`
- `shader`
- `texture`
- `blend`
- `layer`
- `start` / `startTime`
- `duration` / `lifetime`
- `renderQueue`
- `size`
- `color`

Typed settings keys:

```ini
particle.emissive
particle.distortionStrength
particle.noiseStrength
particle.uvScrollSpeed
particle.pulseSpeed
particle.spawnRadius
particle.depthFadeSoftness
particle.edgeSoftness

trail.depthFadeSoftness
trail.tailFade

beam.emissive

distortion.strength
distortion.noiseStrength
distortion.uvScrollSpeed
distortion.depthFadeSoftness
distortion.depthAttenuation
```

Typed keys are valid at asset scope and component scope. At asset scope they
write `EffectAsset::default*` settings used when components are created. At
component scope they write directly into the current component's typed settings
only when the key scope matches the component type.

Legacy flat settings keys are compatibility only:

```ini
emissive
distortion
noise
uvScroll
pulseSpeed
spawnRadius
depthFadeSoftness
softness
distortionDepthAttenuation
distortionAttenuation
particleEdgeSoftness
edgeSoftness
trailTailFade
tailFade
```

At asset scope, legacy flat keys preserve old broad behavior and may map to
more than one typed default. At component scope, they are resolved only through
the current component type, so `noise` in a distortion block updates
`distortion.noiseStrength`, while `noise` in a particle block updates
`particle.noiseStrength`.

`EffectComponentPayload` stores exactly one typed settings object. Code should
access it through asset-side typed views:

- `ParticleComponentAssetView`
- `TrailComponentAssetView`
- `BeamComponentAssetView`
- `DistortionComponentAssetView`

`EffectAssetComponentStorage` is still backed by packed component entries, but
code that wants a typed slice should use lightweight typed iteration helpers
instead of open-coding type checks:

- `ForEachParticleComponent(asset.Components().ParticleStorageView(), visitor)`
- `ForEachTrailComponent(asset.Components().TrailStorageView(), visitor)`
- `ForEachBeamComponent(asset.Components().BeamStorageView(), visitor)`
- `ForEachDistortionComponent(asset.Components().DistortionStorageView(), visitor)`
- `FindParticleComponent(asset.Components().ParticleStorageView(), componentId)` and matching
  Trail/Beam/Distortion lookup helpers
- `Has*Components(asset)` helpers for UI section checks

The older `*Components(asset)` collection view wrappers remain for compatibility
and are implemented over the same iteration helpers. Prefer the `ForEach*`
helpers for hot paths and authoring loops that do not need a materialized list.
These typed iteration helpers are the pre-split boundary before `EffectAsset`
grows separate typed component storage.

Mutable component pointer views have been removed. Loader and `EffectSystem`
may still use packed compatibility paths while constructing or normalizing an
asset, but authoring/debug tools and live reload preservation should use typed
const views plus `Replace*ComponentAndSyncPacked` so typed storage stays
synchronized immediately.

#### Full Component Boundary

Full `EffectComponentAsset` access is allowed only in layers that own or gather
asset data:

- `EffectAssetLoader`: creates components, applies common keys, typed keys, and
  legacy compatibility keys.
- `EffectSystem`: stores registered assets, creates instances, assigns missing
  component ids/names, and owns default component creation. Runtime-facing
  instance component lists and lifetime checks use typed component views rather
  than packed component iteration.
- `EffectRuntime` gather step: resolves an instance component id directly to the
  matching typed `*ComponentAssetView`, then builds `ActiveComponentCore` from
  that view's `EffectComponentCommon`.
- Authoring/debug tools such as `AppImGuiLayer` and live reload preservation in
  `AppRunLoop`: may read typed asset views for editing, but should mutate typed
  data by replacing a typed component through `Replace*ComponentAndSyncPacked`.

Full `EffectComponentAsset` should not appear in renderer-facing surfaces:

- no runtime queue item should store it
- no `*RenderInput` should expose it
- no specialized renderer should receive it
- no RenderGraph pass routing should interpret its typed payload directly

When a new system needs component data, prefer the narrowest available shape in
this order:

```text
EffectComponentCommon + typed settings pointer
  -> *ComponentAssetView
  -> *ActiveComponent
  -> *RenderItem
  -> *RenderInput
```

### EffectRuntime

`EffectRuntime` reads registered assets and live instances from `EffectSystem`
and builds the renderer-facing `EffectRuntimeFrame`.

Its runtime-private queue building path is:

- `ForEachActiveInstanceComponent`: performs shared instance/component active
  filtering.
- `ForEachParticleAssetComponent` / `ForEachTrailAssetComponent` /
  `ForEachBeamAssetComponent` / `ForEachDistortionAssetComponent`: resolve the
  matching asset-side typed component view once, then call
  `BuildActiveComponentCore`.
- `ForEachParticleComponent` / `ForEachTrailComponent` / `ForEachBeamComponent`
  / `ForEachDistortionComponent`: convert asset-side typed views into
  type-specific active views.
- `BuildParticleQueue` / `BuildTrailQueue` / `BuildBeamQueue` / `BuildDistortionQueue`:
  build typed runtime queues directly.
- `FindParticleComponent` / `FindTrailComponent` / `FindBeamComponent` /
  `FindDistortionComponent`: asset-side typed lookup by component id.

`EffectSystem` should stay closer to asset registry and instance storage.
Runtime frame construction belongs to `EffectRuntime`.

### EffectRuntimeFrame

`EffectRuntimeFrame` is the renderer input for one frame.

Current queues:

- `particleQueue`: `ParticleRenderQueue`
- `trailQueue`: `TrailRenderQueue`
- `beamQueue`: `BeamRenderQueue`
- `distortionQueue`: `DistortionRenderQueue`

These queues now own type-specific runtime items:

- `ParticleRenderItem`
- `TrailRenderItem`
- `BeamRenderItem`
- `DistortionRenderItem`

Each typed queue item contains `EffectRenderItemCommon` plus a pointer to its
matching type-specific settings struct. `EffectRenderItemCommon` carries
`componentCommon` for shared component data. `BuildParticleQueue`,
`BuildTrailQueue`, `BuildBeamQueue`, and `BuildDistortionQueue` now build those
typed items directly from `ParticleActiveComponent`, `TrailActiveComponent`,
`BeamActiveComponent`, and `DistortionActiveComponent`.

The runtime handoff is intentionally staged:

```text
ParticleComponentAssetView
  -> ParticleActiveComponent
  -> ParticleRenderItem
```

Trail, Beam, and Distortion follow the same asset view -> active view -> queue
item pattern.

`EffectRenderItemCommon` no longer stores `EffectComponentAsset*`.
`EffectRuntime` resolves active components to typed asset views once, then uses
the view's `EffectComponentCommon` to build shared runtime data. Type-specific
active views consume only common runtime data plus the matching typed settings
pointer.

### AppVfxRenderPipeline

`AppVfxRenderPipeline` owns RenderGraph pass registration and VFX pass routing.

It should stay focused on routing:

- declare pass dependencies
- choose the queue for the pass
- build `VfxRenderContext`
- call the matching renderer

It should avoid owning component matrix, color, material, or type-specific draw
logic when that logic can live inside the specialized renderer.

### VFX Renderers

The renderer-facing input shape is:

```cpp
Draw(commandList, VfxRenderContext, ParticleRenderInput / TrailRenderInput / BeamRenderInput / DistortionRenderInput)
```

Phase 4 has introduced renderer input wrappers:

- `VfxComponentInputCommon`
- `ParticleRenderInput`
- `TrailRenderInput`
- `BeamRenderInput`
- `DistortionRenderInput`

`VfxComponentInputCommon` owns the shared primary item references:
asset/component-common/instance/component-instance, normalized age, render
queue, and the original `EffectRenderItemCommon` pointer. Type-specific inputs
embed this common block and add only the extra data they need.

Each type-specific input now carries its direct settings pointer:

- `ParticleRenderInput::settings`
- `TrailRenderInput::settings`
- `BeamRenderInput::settings`
- `DistortionRenderInput::settings`

`ParticleRenderInput` also carries `fallbackSettings` and `fallbackCommon` for
the primary particle fallback path. The fallback itself is represented by
`ParticleRenderFallback`, not by an `EffectComponentAsset*`.

Renderer inputs do not expose queue pointers. Runtime queues stay in
`EffectRuntimeFrame` for pass culling and future resource planning, while each
renderer consumes its type-specific input assembled from the primary component
item.

Renderer inputs no longer expose `EffectComponentAsset*` as the component
surface. Renderers consume `EffectComponentCommon` for shared data and direct
type-specific settings pointers for specialized data.

`ParticleRenderInput` now also exposes the primary particle item as expanded
asset/component-common/instance references plus direct particle settings.

`TrailRenderInput` and `DistortionRenderInput` now follow the same primary item
pattern. Their renderers resolve type-specific draw parameters from direct
settings instead of interpreting the component asset or queue front.

`EffectRuntimeFrame` owns the small factory accessors for these inputs:

- `PrimaryParticleFallback()`
- `ParticleInput(fallback)`
- `TrailInput()`
- `BeamInput()`
- `DistortionInput()`

This keeps `AppVfxRenderPipeline` focused on pass routing instead of knowing how
each renderer input is assembled.

`VfxRenderContext` carries frame resources, pipelines, descriptors, textures,
depth handles, time, and related render resources. It is render context, not
asset/runtime ownership.

## Phase 4 Direction

Phase 4 should split common component data from type-specific data.
The asset and component shape now follows that split: `EffectAsset` owns typed
defaults, and `EffectComponentAsset` owns `common` plus an explicit typed
payload wrapper.

Current component shape:

```cpp
EffectComponentAsset
  common
  payload

EffectComponentPayload
  std::variant<
    EffectParticleSettings,
    EffectTrailSettings,
    EffectBeamSettings,
    EffectDistortionSettings>
```

Current asset default shape:

```cpp
EffectAsset
  defaultParticle
  defaultTrail
  defaultBeam
  defaultDistortion
```

Suggested split targets:

- Particle asset/input: spawn data, GPU simulation controls, sprite draw data,
  emissive, edge softness.
- Trail asset/input: ribbon data, tail fade, timeline data, trail mesh input.
- Beam asset/input: beam geometry, width, color, time, lightning/noise controls.
- Distortion asset/input: distortion strength, depth attenuation, scene color
  and depth sampling data.

Runtime should follow the same direction: keep `EffectRenderItemCommon` for
shared identity, common component data, and sorting data, while adding
type-specific queue item fields when a component type needs dedicated data or
resources. Full component asset access should stay confined to asset lookup and
authoring/registry code instead of renderer-facing data.

`EffectComponentPayload` is now variant-backed. The code routes asset access
through `*ComponentAssetView`, keeping runtime, loader, and debug UI call sites
close to the eventual shape where each component can become a separate typed
asset object.

New component construction should go through `EffectComponentAssetBuilder`.
The builder keeps construction close to the future typed storage model by
creating owned typed structs first, then packing into the current intermediate
storage:

```text
EffectComponentAssetBuilder::MakeCommon(...)
  -> ParticleComponentAsset / TrailComponentAsset / BeamComponentAsset / DistortionComponentAsset
  -> ToEffectComponentAsset(...)
  -> EffectComponentAsset { common, payload variant }
```

This keeps authoring code close to the eventual split without forcing the
registry storage to split into separate vectors in the same step.

The target shape is for `AppVfxRenderPipeline` to route passes, while each
specialized renderer owns how its component input and resources become draw or
compute work.

## Phase 5 Entry

Phase 5 starts by separating the resource vocabulary for each effect type before
physically splitting every GPU buffer. The first code boundary is
`application/include/vfx/VfxResources.h`.

Each VFX type now has three resource groups:

- simulation resources: compute state, output buffers, indirect argument data
- renderer resources: scene inputs, type output buffers, accumulation target
- pass routing resources: simulation pass name, draw pass name, depth target

The current resource set is intentionally named as a legacy shared indirect
layout:

```cpp
vfx::MakeLegacySharedIndirectVfxResources()
```

It describes the current implementation truth:

- Particle owns the active compute simulation path.
- Particle, Trail, and Distortion still draw through shared
  `ParticleRenderBuffer` and `ParticleIndirectArgs`.
- Beam draws without the shared indirect sprite path.

Distortion now has its own renderer resource intent names:

- `DistortionRenderBuffer`
- `DistortionIndirectArgs`

Those names are currently registered as aliases to the particle output buffer
and particle indirect args, so behavior stays unchanged while RenderGraph pass
debugging and future resource planning can already see the Distortion boundary.
`RenderGraph::Validate()` resolves resource aliases through `stateKey`, matching
the execution-time transition path.

`AppVfxRenderPipeline` now builds RenderGraph access declarations from this
typed resource set instead of hard-coding the same resource list in each pass.
That makes the next Phase 5 steps local and type-specific:

- give Trail its own renderer buffer or CPU/GPU trail mesh stream
- replace the Distortion aliases with a real distortion field or intermediate
  texture
- give Beam its own resource declaration if beam geometry moves out of renderer
  private buffers
- split Particle simulation resources away from the shared sprite draw path

`VfxRenderContext` also carries the active `VfxTypedResourceSet`, so renderers
can start consulting type-specific resource intent without owning pass routing.
`ParticleRenderer`, `TrailRenderer`, and `DistortionRenderer` now read their
own `context.typedResources->*.renderer` entries and pass that intent to the
indirect sprite draw helper. The helper still uses `AppGpuParticleSystem` for
the actual shared implementation, but it resolves the SRV and indirect args by
resource intent name:

- `SrvHandleForResource(rendererResources.renderBuffer)`
- `IndirectArgsForResource(rendererResources.indirectArgs)`

`DistortionRenderBuffer` and `DistortionIndirectArgs` currently resolve to the
same underlying particle resources. The renderer-side and resource lookup
boundaries are now in place, so those names can be swapped to real dedicated
resources later without changing pass routing or renderer input shape.
