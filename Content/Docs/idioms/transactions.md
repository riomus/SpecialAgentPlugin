# Idiom — Transactions and slow tasks

Two `with`-blocks every batch script should know.

## `ScopedEditorTransaction` — group undo + coalesce redraws

```python
with unreal.ScopedEditorTransaction("Layout Trees"):
    for loc in tree_positions:
        eas.spawn_actor_from_class(unreal.StaticMeshActor, loc)
```

- One Ctrl-Z undoes the entire batch.
- Editor coalesces viewport redraws / outliner refresh — much faster on UI-heavy ops.
- The label appears in Edit → Undo menu.

## `ScopedSlowTask` — progress UI + cancellable

```python
N = len(tree_positions)
with unreal.ScopedSlowTask(N, "Spawning trees...") as t:
    t.make_dialog(True)                     # show Cancel button
    for i, loc in enumerate(tree_positions):
        t.enter_progress_frame(1, f"Tree {i+1}/{N}")
        if t.should_cancel():
            return
        eas.spawn_actor_from_class(unreal.StaticMeshActor, loc)
```

- One `enter_progress_frame(1, "label")` per logical step. More frames = more UI breathing room.
- `make_dialog(True)` shows the Cancel button.
- Won't un-freeze a single blocking call (one giant shader compile), but stops the script from looking dead.

## Combine them

```python
with unreal.ScopedEditorTransaction("Build forest"):
    with unreal.ScopedSlowTask(N, "Building forest...") as t:
        t.make_dialog(True)
        for i, loc in enumerate(positions):
            t.enter_progress_frame(1, f"Step {i}")
            if t.should_cancel(): return
            ...
```

Outer transaction groups undo. Inner slow task gives feedback. Both are essential for any batch operation that touches > a few actors.

## Gotcha

Don't nest two `ScopedEditorTransaction` blocks — engine flattens them, but the inner label is lost. One transaction per logical operation.
