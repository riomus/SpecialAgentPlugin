# SpecialAgent UE5 MCP — Tool Surface Expansion Design

Date: 2026-04-19
Author: brainstorm session
Scope: full expansion of the SpecialAgent MCP plugin to expose all reasonable UE5 editor capabilities as MCP tools, fix existing advertised-but-broken services, and polish descriptions/prompts/instructions.

---

## Problem

The plugin README advertises "71+ purpose-built tools" but reality is:

- **~31 tools** have working schemas in `GetAvailableTools()` and are callable
- **21 World tools** return `{"status":"not_implemented"}` despite being registered
- **7 services** (Lighting, Foliage, Landscape, Streaming, Navigation, Gameplay, Performance) have empty `GetAvailableTools()` — zero tools exposed — and their handlers forward to a Python helper that requires a `code` parameter LLMs never send. These services are dead on arrival.
- Many major UE5 editor capability areas are not surfaced at all: Blueprints, Materials editing, Asset import, PIE control, Console/CVars, Components, Editor modes, Level ops, Log, Data tables, Sequencer, Niagara, Sound, World Partition, PCG, Content Browser, Reflection, Physics, Animation, AI, Post-process, Sky/atmosphere, Decals, HLOD, Rendering, Validation, Source control, Render Queue, Modeling, Input.

Result: an LLM connecting to SpecialAgent today sees a small, misleading surface and must fall back to raw Python for anything beyond actor spawning.

## Goal

Expose **all reasonable UE5 editor capabilities** as first-class, well-typed MCP tools, implemented in direct C++ (the "Fat C++" approach). End state: approximately **~280 callable tools across 30 services**, every registered handler has a schema, every schema has a working implementation.

## Non-goals

The following are explicitly **out of scope** for this design (unstable / too narrow / too platform-specific):

- MetaHuman
- Chaos destruction
- Substrate materials
- Geometry Script advanced nodes
- Fracture Mode
- RigVM / Control Rig
- Hair / Groom / Strands
- Waterline water physics

## Approach — Fat C++

Every tool handler uses UE5 editor APIs directly on the game thread via `FGameThreadDispatcher`. Python delegation is **not** used to implement service tools. The user-facing `python/execute` tool remains for power-user scripting, but no service handler generates or executes Python under the hood.

Rejected alternatives:
- **Python codegen inside handlers** — runtime-fragile, harder to type-check, single failure mode, hides the real error.
- **Hybrid (C++ for hot path, Python for breadth)** — inconsistent mental model, two implementation styles, harder to review.

## Architecture

### Critical prerequisite — fix the task-graph reentrancy crash

A live crash has been observed with the current plugin when a Python tool triggers asset import. Stack:

```
Game thread in UEditorEngine::Tick -> MassEntityEditor::Tick -> 
WaitWithNamedThreadsSupport -> ProcessTasksNamedThread -> ProcessTasksUntilQuit
-> TGraphTask<FAsyncGraphTask>::ExecuteTask         <-- HTTP handler runs here
   -> FSpecialAgentMCPServer::HandleMessage
     -> FMCPRequestRouter::HandleToolsCall
       -> FPythonService::HandleExecute
         -> Python -> ImportAssetsAutomated
           -> UInterchangeManager::ImportAssetWithResult
             -> WaitUntilTasksComplete
               -> FNamedTaskThread::ProcessTasksUntilIdle
                 [assert] ++Queue(QueueIndex).RecursionGuard == 1
```

Root cause: `FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn` uses
`AsyncTask(ENamedThreads::GameThread, ...)` from the HTTP worker. The game
thread executes that inside its own `ProcessTasksUntilIdle` pump. When a tool
invokes an engine subsystem that itself blocks on `WaitUntilTasksComplete`,
UE5's named task thread re-enters `ProcessTasksUntilIdle` on the same queue
and hits its recursion guard. The editor crashes.

**This must be fixed in Phase 0 before any new tools land.** Building ~250 new
tools on top of the current dispatcher multiplies the blast radius across
every handler that might pump the task graph: asset_import, pie start/stop,
blueprint/compile, hlod/build, navigation/rebuild, landscape sculpt, save_level
with auto-reimport, render_queue, source_control/submit, data_table import.

**Fix — replace the dispatcher with a tickable processor.**

Introduce `FMCPGameThreadProcessor` — an `FTickableEditorObject` that drains
a thread-safe queue of work items during `Tick()`, which runs **outside** the
task graph's `ProcessTasksUntilIdle` scope.

```cpp
// Public/MCPCommon/MCPGameThreadProcessor.h
class FMCPGameThreadProcessor : public FTickableEditorObject
{
public:
    static FMCPGameThreadProcessor& Get();

    // Called from any thread. Returns a future that completes after Tick()
    // has executed the task on the game thread.
    template<typename ReturnType>
    TFuture<ReturnType> Enqueue(TFunction<ReturnType()> Task);

    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickableInEditor() const override { return true; }

private:
    struct FWorkItem { TFunction<void()> Run; };
    TQueue<FWorkItem, EQueueMode::Mpsc> Pending;
};
```

All handlers replace:
```cpp
FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<T>(Task)
```
with:
```cpp
FMCPGameThreadProcessor::Get().Enqueue<T>(Task).Get()
```

The caller (HTTP worker thread) still blocks on the future — the key change
is that `Task()` executes during `Tick()`, not inside `ProcessTasksUntilIdle`,
so engine subsystems that pump the task graph from inside `Task()` no longer
recurse on the same queue.

`FGameThreadDispatcher` is kept as a thin compatibility wrapper that forwards
to the processor, so existing call sites continue to compile during the
migration. The fast-path optimization (`if (IsInGameThread()) run directly`)
is **removed** — even on the game thread, direct execution inside a task
graph context is the exact bug. When called from the game thread we instead
enqueue and spin the editor message pump, or assert (the MCP server is never
invoked from the game thread anyway).

### Service contract (strengthened)

`IMCPService` currently specifies `GetAvailableTools()` and `HandleRequest()` with no enforcement that the two agree. The new contract:

1. `GetAvailableTools()` MUST return an entry for every branch in `HandleRequest()` — no unlisted methods, no listed-but-unhandled tools.
2. Every handler returns a standard JSON shape: `{ "success": bool, "error"?: string, ...payload }`.
3. No handler is allowed to return `{"status":"not_implemented"}`. Either ship it or remove from `HandleRequest()` + router registration.
4. Every handler that touches `GEditor`, `UWorld`, or `UObject` state runs on the game thread via `FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn`.
5. Every handler logs at `Log` verbosity on success and `Warning` on failure with a stable `SpecialAgent:` prefix.

### Shared infrastructure

To make ~280 tools tractable, three new helper modules are introduced:

**`MCPCommon/MCPToolBuilder`** — fluent schema builder. Replaces ~20 lines of `SetStringField` boilerplate per parameter.

```cpp
FMCPToolInfo Tool = FMCPToolBuilder("spawn_light", "Spawn a light actor. Direct C++, no Python.")
    .RequiredString("light_type", "point | spot | directional | rect | sky")
    .RequiredVec3("location", "World location [X, Y, Z] in cm")
    .OptionalVec3("rotation", "Pitch, Yaw, Roll in degrees")
    .OptionalNumber("intensity", "Intensity. Units depend on light type (cd for point/spot, lux for directional).")
    .OptionalColor("color", "RGB 0-1, default white")
    .Build();
```

Supported parameter types: `String`, `Bool`, `Number`, `Integer`, `Vec3`, `Color`, `Enum`, `ArrayOf<T>`, `Object`. Every method accepts `description`. `Required*` adds to both `properties` and `required`. `Optional*` adds only to `properties`.

**`MCPCommon/MCPJson`** — shared JSON helpers:

```cpp
bool FMCPJson::ReadVec3(TSharedPtr<FJsonObject> Params, const FString& Field, FVector& Out, bool bRequired);
bool FMCPJson::ReadRotator(...);
bool FMCPJson::ReadColor(...);
void FMCPJson::WriteVec3(TSharedPtr<FJsonObject> Out, const FString& Field, const FVector& V);
void FMCPJson::WriteActor(TSharedPtr<FJsonObject> Out, AActor* Actor);   // standard actor serialization
TSharedPtr<FJsonObject> FMCPJson::MakeError(const FString& Msg);
TSharedPtr<FJsonObject> FMCPJson::MakeSuccess();
```

Every existing ad-hoc `SerializeActor` static in services collapses to `FMCPJson::WriteActor`.

**`MCPCommon/MCPActorResolver`** — unifies 13 current copies of `FindActor`:

```cpp
AActor* FMCPActorResolver::ByLabel(UWorld* World, const FString& Name);
AActor* FMCPActorResolver::ByLabelOrPath(UWorld* World, const FString& NameOrPath);
TArray<AActor*> FMCPActorResolver::ByClass(UWorld* World, UClass* Class);
TArray<AActor*> FMCPActorResolver::ByTag(UWorld* World, const FName& Tag);
```

### File layout

```
Source/SpecialAgent/Public/MCPCommon/
    MCPToolBuilder.h
    MCPJson.h
    MCPActorResolver.h
Source/SpecialAgent/Private/MCPCommon/
    MCPToolBuilder.cpp
    MCPJson.cpp
    MCPActorResolver.cpp

Source/SpecialAgent/Public/Services/
    (existing) AssetService.h, WorldService.h, ...
    BlueprintService.h, MaterialService.h, AssetImportService.h,
    PIEService.h, ConsoleService.h, ComponentService.h,
    EditorModeService.h, LevelService.h, LogService.h,
    DataTableService.h, AssetDependencyService.h, SequencerService.h,
    NiagaraService.h, SoundService.h, WorldPartitionService.h,
    PCGService.h, ContentBrowserService.h, ProjectService.h,
    ReflectionService.h, PhysicsService.h, AnimationService.h,
    AIService.h, PostProcessService.h, SkyService.h, DecalService.h,
    HLODService.h, RenderingService.h, ValidationService.h,
    SourceControlService.h, RenderQueueService.h, ModelingService.h,
    InputService.h

Source/SpecialAgent/Private/Services/
    (matching .cpp files)
```

---

## Tool Catalog

### Phase 1 — Fix existing 13 services (~83 tools)

| Service | Tools added / fixed |
|---|---|
| `world` | find_actors_by_tag, spawn_actors_batch, delete_actors_batch, set_actor_transform, set_actor_property, set_actor_label, set_actor_material, set_material_parameter, create_folder, move_actor_to_folder, add_actor_tag, remove_actor_tag, measure_distance, find_actors_in_bounds, raycast, get_ground_height, place_in_grid, place_along_spline, place_in_circle, scatter_in_area, set_actor_tick_enabled, set_actor_hidden, set_actor_collision, attach_to, detach → **25** |
| `lighting` | spawn_light, set_light_intensity, set_light_color, set_light_attenuation, set_light_cast_shadows, build_lighting → **6** |
| `foliage` | paint_in_area, remove_from_area, get_density, list_foliage_types, add_foliage_type → **5** |
| `landscape` | get_info, sculpt_height, flatten_area, smooth_area, paint_layer, list_layers → **6** |
| `streaming` | list_levels, load_level, unload_level, set_level_visibility, set_level_streaming_volume → **5** |
| `navigation` | rebuild_navmesh, test_path, get_navmesh_bounds, find_nearest_reachable_point → **4** |
| `gameplay` | spawn_trigger_volume, spawn_player_start, spawn_note, spawn_target_point, spawn_killz_volume, spawn_blocking_volume → **6** |
| `performance` | get_statistics, get_actor_bounds, check_overlaps, get_triangle_count, get_draw_call_estimate → **5** |
| `assets` | sync_to_browser, create_folder, rename, delete, move, duplicate, save, set_metadata, get_metadata, validate → **10** |
| `viewport` | orbit_around_actor, set_fov, set_view_mode, toggle_game_view, bookmark_save, bookmark_restore, set_grid_snap, toggle_realtime → **8** |
| `utility` | focus_asset_in_browser, deselect_all, invert_selection, select_by_class, group_selected, ungroup, begin_transaction, end_transaction, show_notification, show_dialog, focus_tab → **11** |

### Phase 2 — New services (~170 tools across 22 services)

| Service | Tools |
|---|---|
| `blueprint` | create, compile, add_variable, add_function, set_default_value, list_functions, list_variables, open_in_editor, duplicate, reparent → **10** |
| `material` | create, create_instance, set_scalar_parameter, set_vector_parameter, set_texture_parameter, set_static_switch, list_parameters, get_parameters → **8** |
| `asset_import` | import_fbx, import_texture, import_sound, import_folder, create_data_table_from_csv, get_import_settings_template → **6** |
| `pie` | start, stop, pause, resume, step_frame, toggle_simulate, is_playing, get_world_context → **8** |
| `console` | execute, list_commands, set_cvar, get_cvar → **4** |
| `component` | add, remove, list, get_properties, set_property, attach, detach → **7** |
| `editor_mode` | activate, get_current, configure_brush → **3** |
| `level` | open, new, save_as, get_current_path, list_templates → **5** |
| `log` | tail, clear, list_categories, set_category_verbosity → **4** |
| `data_table` | list_tables, get_row, set_row, add_row, delete_row, list_rows, get_row_struct → **7** |
| `asset_deps` | get_references, get_referencers, find_unused, get_dependency_graph → **4** |
| `sequencer` | create, add_actor_binding, add_transform_track, add_keyframe, set_playback_range, play → **6** |
| `niagara` | spawn_emitter, set_parameter, activate, deactivate, set_user_float, set_user_vec3 → **6** |
| `sound` | play_2d, play_at_location, spawn_ambient_actor, set_volume_multiplier → **4** |
| `world_partition` | list_cells, load_cell, unload_cell, get_loaded_cells, force_load_region → **5** |
| `pcg` | list_graphs, execute_graph, spawn_pcg_actor → **3** |
| `content_browser` | sync_to_folder, create_folder, rename, delete, move, duplicate, save, set_metadata, get_metadata → **9** |
| `project` | get_setting, set_setting, get_version, list_plugins, enable_plugin, disable_plugin, get_content_path, get_project_path → **8** |
| `reflection` | list_classes, get_class_info, list_properties, list_functions, call_function → **5** |
| `physics` | set_simulate_physics, apply_impulse, apply_force, set_linear_velocity, set_angular_velocity, set_mass, set_collision_enabled → **7** |
| `animation` | play, stop, set_anim_blueprint, list_animations, set_pose → **5** |
| `ai` | spawn_ai_pawn, assign_controller, run_behavior_tree, set_blackboard_value, stop_ai → **5** |
| `post_process` | spawn_volume, set_exposure, set_bloom, set_dof, set_color_grading, set_gi → **6** |
| `sky` | spawn_sky_atmosphere, spawn_height_fog, spawn_cloud, spawn_sky_light, set_sun_angle → **5** |
| `decal` | spawn, set_material, set_size → **3** |
| `hlod` | build, clear, set_setting → **3** |
| `rendering` | set_scalability, set_view_mode, high_res_screenshot, toggle_nanite, toggle_lumen → **5** |
| `validation` | validate_selected, validate_level, list_errors → **3** |
| `source_control` | get_status, check_out, revert, submit, list_modified → **5** |
| `render_queue` | queue_sequence, set_output, get_status → **3** |
| `modeling` | boolean_union, boolean_subtract, extrude, simplify → **4** |
| `input` | list_mappings, add_action, add_axis, remove_mapping → **4** |

### Totals

- ~31 existing (kept as-is)
- ~83 Phase 1 (fidelity fixes + expansion)
- ~170 Phase 2 (new services)
- **~280 total advertised tools, 100% callable with working handlers**

---

## Description / Prompt Polish

### Tool description standard

Every tool description follows this shape:

```
<verb> <object>. <effect, in one sentence>.
Params: <param> (type, unit/range, role).
Workflow: <cross-reference to related tool>
Warning: <caveat, if any>
```

Example (good):
```
Spawn a point/spot/directional/rect/sky light. Returns the new actor label.
Params: light_type (string, one of 5 values), location ([X,Y,Z] cm world), 
        intensity (number, cd or lux by type), color (RGB 0-1).
Workflow: After spawning, call lighting/build_lighting to bake statics.
Warning: Directional lights should be unique per level.
```

Example (bad — what to avoid):
```
Spawn a light.
```

A polish agent reviews every tool description against this standard at Phase 3.

### System instructions

`BuildSpecialAgentInstructions()` expands from a 4-sentence blurb to a compact service map, keeping total under ~1500 tokens. Format:

```
SpecialAgent controls Unreal Editor. 
WORKFLOW: screenshot -> inspect -> act -> screenshot to verify.
SCREEN COORDS: 0-1 normalized.
SERVICES: 
  world (actors), lighting (lights), foliage, landscape,
  blueprint (BP assets), material, asset_import, pie (play),
  console (cvars/commands), component, editor_mode, level, log,
  data_table, sequencer, niagara, sound, world_partition, pcg,
  content_browser, project, reflection, physics, animation,
  ai, post_process, sky, decal, hlod, rendering, validation,
  source_control, render_queue, modeling, input.
KEY TOOLS: [same as current]
```

### MCP prompts/list expansion

Current: 4 prompts (`explore_level`, `find_actor`, `inspect_selection`, `place_objects`).

Expanded: add the following high-leverage prompt templates.

- `build_scene` — multi-step populate with lighting, foliage, streaming.
- `create_blueprint` — spec a Blueprint, compile, spawn an instance, verify.
- `import_assets` — point at a folder or files, import, set up materials, validate.
- `build_sequence` — Sequencer timeline with cameras and keyframes.
- `setup_lighting` — spawn sky atmosphere, height fog, sun, sky light, build.
- `populate_foliage` — pick foliage types, paint at density, screenshot.
- `build_landscape` — create landscape, sculpt regions, paint layers.
- `configure_postprocess` — spawn PP volume, tune exposure/bloom/DoF.
- `setup_navigation` — add nav bounds, rebuild navmesh, test a path.
- `wire_gameplay` — spawn player start, kill-z, triggers, player area.
- `debug_performance` — grab stats, find overlaps, triangle hot spots.
- `run_pie_test` — start PIE, capture log tail, stop, summarize.

---

## Phasing and Parallelization

### Phase 0 — Sequential foundation (1 agent)

1. **Fix the task-graph reentrancy crash.** Implement `FMCPGameThreadProcessor` (tickable editor object with MPSC queue). Migrate every existing call site of `FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn` to `FMCPGameThreadProcessor::Get().Enqueue(...).Get()`. Keep `FGameThreadDispatcher` as a thin forwarder (or delete it) so the rest of the codebase compiles.
2. Implement `FMCPToolBuilder`, `FMCPJson`, `FMCPActorResolver`.
3. Stub every new service header + empty .cpp file so Phase 1 agents have targets.
4. Update `IMCPService.h` with the strengthened contract comments.
5. Compile-check the foundation.
6. **Verify the fix**: reproduce the original crash (have Python trigger an asset import via `ImportAssetsAutomated`) and confirm it no longer asserts.

### Phase 1 — Parallel service implementation (12 agents)

| Agent | Services |
|---|---|
| A. Core Actor | `world` stub-fixes + `utility`+11 + `viewport`+8 |
| B. Level Design | `lighting` + `foliage` + `landscape` |
| C. Structure / Streaming | `streaming` + `navigation` + `world_partition` |
| D. Gameplay / Perf | `gameplay` + `performance` + `assets`+10 |
| E. Blueprint / Reflection | `blueprint` + `reflection` |
| F. Materials / Visuals | `material` + `post_process` + `sky` + `decal` |
| G. Asset Pipeline | `asset_import` + `content_browser` + `asset_deps` + `data_table` + `validation` |
| H. Editor Control | `pie` + `console` + `log` + `level` + `editor_mode` + `project` |
| I. Components / Physics | `component` + `physics` + `animation` |
| J. AI / Input / Sound | `ai` + `input` + `sound` |
| K. Cinematics / Rendering | `sequencer` + `niagara` + `render_queue` + `rendering` |
| L. Finishing | `pcg` + `modeling` + `hlod` + `source_control` |

Each agent is briefed with: the service headers (already stubbed), the helper infra, the tool catalog entry from this doc, the description standard, and the requirement that every tool be directly callable with a full working handler. Agents work in worktrees to avoid conflicts.

### Phase 2 — Integration (1 agent, sequential)

1. Register all new services in `FMCPRequestRouter::FMCPRequestRouter()`.
2. Assert every registered service returns non-empty `GetAvailableTools()`.
3. Full build; fix linker/header issues.
4. Run `tools/list` in a live editor; assert ≥ 250 tools returned.

### Phase 3 — Polish (2 agents, parallel)

- **Agent M (descriptions):** review every tool description against the standard; rewrite as needed.
- **Agent N (prompts & instructions):** rewrite `BuildSpecialAgentInstructions()`, add the 12 new prompt templates, update `README.md` tool count and service table.

### Phase 4 — Verification (sequential)

1. Full solution build on Windows + Mac (user builds locally; CI optional).
2. In live editor: call `tools/list`, spot-check ~20 handlers from varied services.
3. Commit final review report.

---

## Testing Strategy

The plugin has no existing test harness. This design adds a lightweight verification approach rather than a new test framework:

1. **Compile-time enforcement** — `IMCPService.h` comments + code review enforce the contract. A simple check in `FMCPRequestRouter` at startup logs a warning if any registered service returns empty `GetAvailableTools()`.
2. **Live smoke tests** — Phase 4 runs a documented manual checklist: start editor, call `tools/list`, then call one handler per service with a known-good request, verify success.
3. **Schema-vs-handler consistency** — a tiny helper, `FMCPRequestRouter::ValidateServices()`, called once at startup, logs all tool names for each service and prints a warning for any handler branch that doesn't appear in the listed tools. Non-fatal; aids development.

Full unit tests are explicitly out of scope — editor-dependent APIs are hard to unit-test, and the value is better spent on the verification checklist.

---

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Task-graph reentrancy crash on any handler that pumps tasks (asset import, PIE, blueprint compile, HLOD build, navmesh rebuild, landscape ops, save, render queue, source_control submit, data_table CSV import) | Replace `FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn` with `FMCPGameThreadProcessor` (tickable drain) in Phase 0 before any new tools ship. Verify by repro'ing the original crash. |
| Some Phase 2 services depend on modules not in `Build.cs` (Sequencer, Niagara, etc.) | Phase 0 agent verifies each new service header compiles with minimum required module additions to `Build.cs`. Service agents report any missing modules. |
| Scope creep — 280 tools is a lot | Strict per-agent catalog limits. Agents that finish early do description polish or pick up Group L overflow. No new services added mid-stream. |
| Description inconsistency | Dedicated Phase 3 description polish agent applying the standard uniformly. |
| API churn between UE 5.6 and 5.7 | Target UE 5.6+ (plugin already claims this). Avoid experimental APIs. |
| Niagara / Sequencer / PCG handlers being fragile | Limit tool surface to the most stable, well-known APIs per subsystem (as catalogued). Mark any truly experimental tool with a "BETA:" prefix in its description. |
| Registering ~30 services in router balloons startup work | Registration is cheap; services are lightweight constructors with no heavy init. |

---

## Success Criteria

1. **Task-graph reentrancy crash no longer reproduces.** A handler that invokes `ImportAssetsAutomated` (or any task-graph-pumping subsystem) completes without the `RecursionGuard` assert.
2. `tools/list` returns ≥ 250 tools, all with non-empty schemas.
3. Zero handlers return `{"status":"not_implemented"}`.
4. Every registered service returns a non-empty `GetAvailableTools()`.
5. Every tool description matches the documented standard.
6. `BuildSpecialAgentInstructions()` enumerates all 30 services in compact form.
7. `prompts/list` returns at least 16 prompt templates (4 existing + 12 new).
8. README tool count claim updated to match reality.
9. Full clean build on Mac (user's platform).
10. Live smoke test: at least one handler per service successfully invoked.

---

## Open Items (deferred to writing-plans)

- Exact module additions required to `Build.cs` per new service (e.g. `MovieSceneTools`, `Niagara`, `WorldPartitionEditor`, `PCG`, `SourceControl`).
- Exact class names for each editor-subsystem entry point per new service.
- Per-tool parameter schemas (the implementation plan enumerates these by agent).
