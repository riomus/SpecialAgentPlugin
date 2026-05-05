# Idiom — Material instance parameters

## Edit a Material Instance Constant (MIC) without double-compiling

```python
mi = easset.load_asset("/Game/Materials/MI_Wood_Oak")

# Set parameters
unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(mi, "Roughness", 0.7)
unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(mi, "BaseColorTint", unreal.LinearColor(0.6, 0.4, 0.2))
unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(mi, "AlbedoMap", easset.load_asset("/Game/Tex/T_Wood"))

# Save once at the end (default only_if_is_dirty=True)
easset.save_asset(mi.get_path_name())
```

**Do not** call `MaterialEditingLibrary.update_material_instance(mi)` after each set — `save_asset` already triggers the recompile. Calling both forces a second compile with no benefit.

## Reparenting

```python
new_parent = easset.load_asset("/Game/Materials/M_PBR_Wood_v2")

# CRITICAL: guard before reparenting
if mi.get_editor_property("parent") != new_parent:
    unreal.MaterialEditingLibrary.set_material_instance_parent(mi, new_parent)
    easset.save_asset(mi.get_path_name())
```

Reparenting to the same parent **still** triggers a full MI shader rebuild. The equality check is not a micro-optimization — it's the difference between a one-second no-op and a 30-second recompile.

## Don't duplicate to "tweak"

Duplicating a 30-expression material (`duplicate_asset`) means recompiling all 30 every run. Prefer:

- Add a `Static Switch Parameter` to the **existing** parent.
- Use a Material Instance to flip the switch.

This recompiles permutations on demand instead of every script execution.

## After material edits — wait for shaders

```python
# Reuse the project helper
import sys, importlib
sys.path.append(unreal.Paths.project_content_dir() + "Python")
import _shader_progress
_shader_progress.wait_for_shaders("MI parameter pass")
```

The helper polls the compile queue inside a `ScopedSlowTask` and degrades to no-op when `unreal.ShaderCompilingManager` is absent (UE 5.7 macOS). See `idioms/shader_compile_progress.md`.
