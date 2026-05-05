# SpecialAgent — UE5 Python Cheat Sheet

You are driving Unreal Editor 5.6/5.7 via MCP. Read this once at session start. Then favor specific service tools (`world/*`, `assets/*`, `blueprint/*`, ...) before reaching for `python/execute`.

## 1. Modern subsystems (use these — the Library classes are deprecated)

| Use this                                  | Instead of                          |
|-------------------------------------------|-------------------------------------|
| `unreal.EditorActorSubsystem`             | `unreal.EditorLevelLibrary`         |
| `unreal.EditorAssetSubsystem`             | `unreal.EditorAssetLibrary`         |
| `unreal.LevelEditorSubsystem`             | misc level-edit Library calls       |
| `unreal.UnrealEditorSubsystem`            | viewport / camera Library calls     |
| `unreal.EditorUtilitySubsystem`           | running Editor Utility widgets      |
| `unreal.AssetEditorSubsystem`             | open / close asset editors          |

Acquire via `unreal.get_editor_subsystem(unreal.EditorActorSubsystem)`.
If you're not sure which subsystem to use, call `python/list_subsystems` — it returns the live catalog.

## 2. Idempotency

Scripts get re-run. Make sure the second run does no real work.

- Check `unreal.EditorAssetLibrary.does_asset_exist(path)` before re-creating an asset. If it exists, **skip the expensive path** — never `delete_asset` + re-create just to re-run.
- Default `save_asset(only_if_is_dirty=True)`. Pass `False` only for a concrete reason (forces disk write + recompile even when nothing changed).
- Before `set_material_instance_parent`, compare `mi.get_editor_property('parent')`. Reparenting to the same asset still triggers a full MI shader rebuild.
- Prefer modifying the existing parent (e.g. add a static switch) over duplicating + editing. Duplicating a 30-expression material recompiles all 30 every run.
- Don't `EAL.delete_asset` things with referencers without fixing referencers first — deletion cascades force every referencer to recompile.

## 3. Game thread + shader compile

- `IPythonScriptPlugin` runs on the **game thread**. Anything blocking freezes the editor UI. Slowest operations: shader compiles, `set_material_instance_parent`, asset duplication, saves.
- `unreal.ShaderCompilingManager` is **not always exposed** (missing on UE 5.7 macOS, for one). Always:

  ```python
  scm = getattr(unreal, "ShaderCompilingManager", None)
  if scm is not None:
      ...
  ```

- For shader-compile progress, use `Content/Python/_shader_progress.py:wait_for_shaders("label")` — it polls the compile queue inside a `ScopedSlowTask` and degrades to no-op when `ShaderCompilingManager` is absent.
- Wrap slow work in `unreal.ScopedSlowTask` so the UI stays responsive and is cancellable:

  ```python
  with unreal.ScopedSlowTask(N, "Doing X...") as t:
      t.make_dialog(True)
      for i, step in enumerate(steps):
          t.enter_progress_frame(1, f"Step {i}")
          if t.should_cancel(): return
          ...
  ```

- Batch UI-visible edits in `with unreal.ScopedEditorTransaction("..."):` so the editor coalesces redraws and undo groups them as one entry.
- Logs: prefer `unreal.log` / `unreal.log_warning` / `unreal.log_error` (they route through LogPython in the Output Log) over `print`.

## 4. Sampler type table

`TextureSample` / `TextureObjectParameter` `sampler_type` MUST match the texture's `compression_settings`. Mismatches fail compile on Metal/SM6 with "Sampler type is X, should be Y".

| Compression                | Correct `SAMPLERTYPE_*`        |
|----------------------------|--------------------------------|
| `TC_DEFAULT`, sRGB on      | `COLOR`                        |
| `TC_DEFAULT`, sRGB off     | `LINEAR_COLOR`                 |
| `TC_NORMALMAP`             | `NORMAL`                       |
| `TC_MASKS`                 | `MASKS`                        |
| `TC_GRAYSCALE`, sRGB on    | `GRAYSCALE`                    |
| `TC_GRAYSCALE`, sRGB off   | `LINEAR_GRAYSCALE`             |
| `TC_ALPHA`                 | `ALPHA`                        |
| any above + virtual texture| matching `VIRTUAL_*` variant   |

When you set a default texture on a parameter, `load_asset` it first and read `compression_settings` + `srgb` so the sampler choice is **driven by the texture, not guessed**. Megascan heightmaps shipping as `TC_ALPHA` need `SAMPLERTYPE_ALPHA`, not `LINEAR_COLOR`.

## 5. Viewport redraw

Camera writes do **not** repaint until next tick. After any of:

- `viewport/set_*`, `viewport/focus_actor`, `viewport/orbit_around_actor`, `viewport/set_fov`, `viewport/set_view_mode`, `viewport/toggle_game_view`, `viewport/bookmark_restore`
- **or any `python/execute` that touches `UnrealEditorSubsystem.set_level_viewport_camera_info` or other viewport-client state**

call `viewport/force_redraw` before `screenshot/capture` or `screenshot/save`. Otherwise the captured frame still shows the previous view.

## 6. Common idioms

```python
# Load an asset (does_asset_exist still lives on the Library; load lives on the subsystem)
easset = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
asset = easset.load_asset("/Game/Foo/Bar")

# Spawn an actor (modern)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actor = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, 0))

# Set a property
actor.set_editor_property("hidden_in_game", False)

# Transactional batch edit (single undo entry, coalesced redraws)
with unreal.ScopedEditorTransaction("Layout Trees"):
    for loc in locations:
        eas.spawn_actor_from_class(unreal.StaticMeshActor, loc)
```

## 7. Discovery-first protocol

Before writing Python that calls a class / function you're not 100% certain exists on **this** build:

1. `python/search_symbol substring=...` — list matching names in `dir(unreal)`.
2. `python/get_function_signature class_name=... method=...` — confirm parameter list + types.
3. `python/list_subsystems` — see which `EditorSubsystem` / `EngineSubsystem` classes are loaded right now.
4. `python/diff_against_deprecated snippet=...` — paste your draft to flag deprecated calls before executing.
5. `python/help symbol=...` — full docstring for any class or callable.

These tools are cheap and stop wrong-arg / wrong-API failures cold. Use them. Do not guess.
