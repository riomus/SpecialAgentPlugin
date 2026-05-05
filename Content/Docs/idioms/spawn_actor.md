# Idiom — Spawn an actor

## Modern path

```python
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# From a class (no asset needed)
actor = eas.spawn_actor_from_class(
    unreal.StaticMeshActor,
    unreal.Vector(0, 0, 0),
    unreal.Rotator(0, 0, 0),
)

# From a loaded asset (Blueprint, StaticMesh, etc.)
easset = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
bp = easset.load_asset("/Game/BP/BP_Tree")
actor = eas.spawn_actor_from_object(bp, unreal.Vector(100, 0, 0))
```

## Configure post-spawn

```python
actor.set_actor_label("MyTree_01")            # Show in World Outliner
actor.set_editor_property("hidden_in_game", False)
actor.tags = ["procedural", "vegetation"]     # Searchable

# Components
mesh_comp = actor.get_component_by_class(unreal.StaticMeshComponent)
mesh_comp.set_static_mesh(easset.load_asset("/Game/Meshes/SM_Tree"))
```

## Idempotent re-spawn

If the script is re-run, don't double-spawn. Check by label:

```python
existing = [a for a in eas.get_all_level_actors() if a.get_actor_label() == "MyTree_01"]
if existing:
    actor = existing[0]
else:
    actor = eas.spawn_actor_from_class(unreal.StaticMeshActor, loc)
```

## Cleanup

```python
eas.destroy_actor(actor)
```

## Gotcha

`spawn_actor_from_class` requires a Python `unreal.<ClassName>`, not a string. If you only have a class path string, resolve via `unreal.find_class("StaticMeshActor")` first. Engine and module loading order matters — call `python/list_subsystems` before `EditorActorSubsystem` is available (it isn't on `-headless` runs).
