# Idiom — Load an asset

## Modern path (preferred)

```python
easset = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
asset = easset.load_asset("/Game/Foo/Bar")    # full asset by path
data  = easset.find_asset_data("/Game/Foo/Bar")  # AssetRegistry FAssetData; cheap, no load
exists = easset.does_asset_exist("/Game/Foo/Bar")
```

`load_asset` returns the actual `UObject` (StaticMesh, Material, etc.) — heavy. `find_asset_data` returns lightweight metadata (`asset_class_path`, `package_name`, tags) without loading; use it for existence / type checks.

## Path forms

- Object path: `"/Game/Foo/Bar"` — preferred, package + asset name happen to match.
- Disambiguated: `"/Game/Foo/Bar.Bar"` — when package contains multiple top-level assets (rare).
- **Wrong:** filesystem paths (`Content/Foo/Bar.uasset`) — these never resolve.

## Type-driven dispatch

If you don't know the class beforehand:

```python
data = easset.find_asset_data(path)
if not data.is_valid():
    return  # nothing here

cls_path = str(data.asset_class_path)  # e.g. "/Script/Engine.StaticMesh"
asset = easset.load_asset(path)
```

Or call `python/get_asset_class_for_path asset_path=...` to ask the server.

## Gotcha

`unreal.load_asset(...)` (bare module function) still works in current builds, but routes through deprecated paths internally. Prefer the subsystem.
