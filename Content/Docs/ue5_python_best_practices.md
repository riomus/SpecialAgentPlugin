# UE5.7 Python Best Practices (for `python/execute`)

Read this before writing non-trivial `python/execute` scripts. It covers the
golden rules, the modern subsystem map (what to call), units/conventions, and
how to **discover the exact available methods at runtime** instead of guessing.

The `unreal` module is Python 3.11 and runs on the **Game Thread** — a blocking
call freezes the editor UI. Keep scripts short and non-blocking.

---

## Discover the API at runtime (don't guess method names)

This plugin ships live introspection tools — prefer them over recalling APIs:

| Tool | Use it to |
|------|-----------|
| `python/list_subsystems` | list every editor/engine subsystem you can get |
| `python/inspect_class` | dump a UClass's methods + properties |
| `python/search_symbol` | fuzzy-find a class/function/enum by name |
| `python/get_function_signature` | exact args + return type of one method |
| `python/list_enum_values` | valid values for an enum (e.g. sampler types) |
| `python/get_asset_class_for_path` | the UClass an asset path resolves to |
| `python/diff_against_deprecated` | flag deprecated APIs in a script before running |
| `python/help` | this docs index + the cheat sheet |

From Python directly: `dir(obj)`, `unreal.<Class>.__doc__`, and
`help(unreal.<Class>.<method>)` also work.

---

## Golden rules

1. **Use Editor Subsystems, not the deprecated `*Library` classes.** Get them
   with `unreal.get_editor_subsystem(...)` (or `get_engine_subsystem(...)` for
   engine-scoped ones). See the map below. The legacy `EditorLevelLibrary`,
   `EditorAssetLibrary`, `EditorFilterLibrary`, `EditorLevelUtils` are
   deprecated — see `Content/Docs/deprecations.md`.
2. **Wrap edits in a transaction** so they're undoable:
   `with unreal.ScopedEditorTransaction("My Edit"): ...`. Call `obj.modify()`
   **before** mutating an existing object's properties (registers undo + dirties
   the package). New spawns don't need `modify()`.
3. **Persist explicitly.** Editing memory ≠ saving. Mark dirty (`modify()` does
   this) then save: actors → save the level
   (`LevelEditorSubsystem.save_current_level()` /
   `EditorLoadingAndSavingUtils.save_dirty_packages(...)`); assets →
   `EditorAssetSubsystem.save_asset(path)`. State whether your script persists.
4. **Be idempotent.** Before an expensive rebuild, check current state and skip
   if unchanged (`EditorAssetSubsystem.does_asset_exist`, compare the property
   you're about to set). Never delete-and-recreate just to "re-run".
5. **Don't double-compile.** `save_asset` already recompiles; reparenting an MI
   already refreshes it. Don't add an explicit recompile on top.
6. **Guard optional engine classes.** Some classes aren't bound on every build
   (e.g. `unreal.ShaderCompilingManager` is missing on UE 5.7 macOS). Use
   `getattr(unreal, "ShaderCompilingManager", None)` and degrade gracefully —
   never call `unreal.ShaderCompilingManager.get()` directly.
7. **Show progress for slow work** with `unreal.ScopedSlowTask` (one
   `enter_progress_frame` per step, `make_dialog(True)`, `should_cancel()`).

---

## Subsystem map (what to call for what)

Get with `unreal.get_editor_subsystem(unreal.<Name>)` unless noted *(engine)* →
`unreal.get_engine_subsystem(...)`.

| Need | Subsystem | Key methods |
|------|-----------|-------------|
| Spawn / destroy / duplicate / select actors | `EditorActorSubsystem` | `spawn_actor_from_object/class`, `destroy_actor(s)`, `duplicate_actor(s)`, `get_all_level_actors`, `set_selected_level_actors`, `get_selected_level_actors` |
| Editor world, camera info, editor/PIE world | `UnrealEditorSubsystem` | `get_editor_world`, `get_game_world`, `get_level_viewport_camera_info`, `set_level_viewport_camera_info` |
| Viewport invalidate, PIE/simulate, save level | `LevelEditorSubsystem` | `editor_invalidate_viewports`, `editor_request_begin_play/end_play`, `editor_play_simulate`, `save_current_level`, `build_light_maps` |
| Asset CRUD / save / duplicate / exists | `EditorAssetSubsystem` | `does_asset_exist`, `load_asset`, `save_asset`, `duplicate_asset`, `delete_asset`, `rename_asset`, `set/get_metadata_tag` |
| Loaded selection in Content Browser | `EditorUtilitySubsystem` / `EditorUtilityLibrary.get_selected_assets()` | — |
| Static-mesh edits (collision/LOD/Nanite/sockets) | `StaticMeshEditorSubsystem` | `add_simple_collisions`, `set_nanite_settings`, `set_lods`, `set_generate_lightmap_uvs` |
| Add a persistent component to a BP | `SubobjectDataSubsystem` *(engine)* | `k2_gather_subobject_data_for_blueprint`, `add_new_subobject`, `attach_subobject` |
| Editor Layers | `LayersSubsystem` | `add_actor_to_layer`, `remove_actors_from_layer`, `select_actors_in_layer` |

Non-subsystem helper library classes that ARE current (classmethods):
`AssetToolsHelpers.get_asset_tools()`, `AssetRegistryHelpers.get_asset_registry()`,
`MaterialEditingLibrary`, `BlueprintEditorLibrary`, `DataTableFunctionLibrary`,
`SystemLibrary`, `GameplayStatics`.

---

## Units & conventions

- **1 unit = 1 cm.** Left-handed, Z-up: +X forward, +Y right, +Z up.
- Locations/extents/radii in **cm**; rotations in **degrees**.
- `unreal.Rotator(pitch, yaw, roll)` — positional order is **(pitch, yaw, roll)**,
  but the Details panel labels them X=Roll / Y=Pitch / Z=Yaw. Be explicit.
- `unreal.Vector` defaults 0.0; `Transform` scale defaults 1.0 (unitless).
- Asset paths are **virtual** (`/Game/...`, `/Engine/...`), never OS paths.
  Object path is `/Game/Folder/Asset.Asset`; package name omits the `.Asset`.
- Sky Atmosphere / Volumetric Cloud distances are in **kilometers**; light
  intensity is **lux** (directional) or **candela** (point/spot/rect).
- Material **vector** params are `LinearColor(r,g,b,a)` in linear 0..1.
- `AssetRegistry` ARFilter class filters need a full
  `unreal.TopLevelAssetPath('/Script/Engine', 'StaticMesh')`, not a short name;
  set `recursive_paths`/`recursive_classes=True` to descend.

---

## Common gotchas

- **Material instance params**: edit `MaterialInstanceConstant` assets with
  `MaterialEditingLibrary.set_material_instance_*_parameter_value(...)` (returns
  **False** if the param name is wrong — check it). `MaterialInstanceDynamic`
  setters are runtime-only and never persist. Static-switch sets force a full
  shader recompile — batch with `update_material_instance=False`, then one
  `update_material_instance(mi)`.
- **TextureSample `sampler_type` must match the texture's `compression_settings`
  + `srgb`** or the shader fails to compile on Metal/SM6. Read those off the
  loaded texture; don't guess. See `Content/Docs/idioms/sampler_types.md`.
- **Skeletal anim preview** doesn't tick in-editor by default — after
  `play_animation` call `set_update_animation_in_editor(True)`.
- **Niagara user params** must be exposed (`User.` namespace) and set with the
  typed setter (`set_niagara_variable_float/vec3/...`) — a wrong name/type is a
  silent no-op.
- **Foliage**: there is no `EditorFoliageLibrary`; use
  `InstancedFoliageActor.add_instances(world, foliage_type, transforms)`.

---

## Where to look next

- `Content/Docs/ue5_python_cheatsheet.md` — the always-on quick rules.
- `Content/Docs/deprecations.md` — deprecated → modern API table.
- `Content/Docs/idioms/*.md` — copy-paste recipes (spawn_actor, material_params,
  transactions, sampler_types, shader_compile_progress, load_asset).
- And remember: when in doubt, `python/inspect_class` /
  `python/get_function_signature` give you the *exact* current API.
