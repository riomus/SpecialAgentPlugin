# Idiom — Shader compile progress

After material edits, the slowest remaining work is shader permutations compiling in the background. The script returns "done" but the editor keeps churning. Two pieces of guidance:

## Always probe `ShaderCompilingManager` defensively

```python
scm = getattr(unreal, "ShaderCompilingManager", None)
if scm is None:
    # Not exposed on this build (UE 5.7 macOS, headless, etc.)
    return

mgr = scm.get()
remaining = mgr.get_num_remaining_jobs() if hasattr(mgr, "get_num_remaining_jobs") else 0
```

`unreal.ShaderCompilingManager` is **not always exposed to the Python API**. Calling `unreal.ShaderCompilingManager.get()` directly raises `AttributeError` on those builds. Always `getattr(unreal, "ShaderCompilingManager", None)` first.

## Use the project helper

`Content/Python/_shader_progress.py` provides `wait_for_shaders(label)`. It:

- Polls the compile queue.
- Wraps the wait in `ScopedSlowTask` so the editor shows the live count.
- Degrades to a no-op when `ShaderCompilingManager` is absent.

```python
import sys, importlib
sys.path.append(unreal.Paths.project_content_dir() + "Python")
import _shader_progress
_shader_progress.wait_for_shaders("Material refresh")
```

Reuse it. Don't re-implement the polling loop.

## When to wait

- After `set_material_instance_parent` (always).
- After bulk material parameter edits + `save_asset`.
- Before screenshotting a freshly authored material — pixels show the previous shader until compile completes.

## When to skip

- Read-only scripts (no material edits).
- Scripts that only spawn actors with already-compiled materials.

## Gotcha

A single huge shader compile blocks the game thread, so `wait_for_shaders` won't make the editor responsive during that one job — only between jobs. For consistent responsiveness, batch material edits + wait once at the end rather than wait-after-each.
