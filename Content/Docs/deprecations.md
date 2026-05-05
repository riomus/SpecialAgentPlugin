# UE5 Python — Deprecated → Modern API mapping

The C++ scanner (`python/diff_against_deprecated`) parses the table below.

**Format rule:** each table row is `| deprecated | modern | notes |`. No backticks inside columns. The header row (literal columns "Deprecated" / "Modern replacement") is skipped automatically; every other row whose first column appears in a Python snippet generates a finding.

Acquire all subsystems via `unreal.get_editor_subsystem(unreal.<Subsystem>)`.

| Deprecated                                          | Modern replacement                                                | Notes |
|-----------------------------------------------------|-------------------------------------------------------------------|-------|
| unreal.EditorLevelLibrary.spawn_actor_from_class    | unreal.EditorActorSubsystem.spawn_actor_from_class                | EditorLevelLibrary deprecated in 5.0; subsystem in 5.1+. |
| unreal.EditorLevelLibrary.spawn_actor_from_object   | unreal.EditorActorSubsystem.spawn_actor_from_object               | Same. |
| unreal.EditorLevelLibrary.get_all_level_actors      | unreal.EditorActorSubsystem.get_all_level_actors                  | Same. |
| unreal.EditorLevelLibrary.get_selected_level_actors | unreal.EditorActorSubsystem.get_selected_level_actors             | Same. |
| unreal.EditorLevelLibrary.set_actor_selection_state | unreal.EditorActorSubsystem.set_actor_selection_state             | Same. |
| unreal.EditorLevelLibrary.destroy_actor             | unreal.EditorActorSubsystem.destroy_actor                         | Same. |
| unreal.EditorLevelLibrary.duplicate_actor           | unreal.EditorActorSubsystem.duplicate_actor                       | Same. |
| unreal.EditorLevelLibrary.get_editor_world          | unreal.UnrealEditorSubsystem.get_editor_world                     | Editor world access moved off the legacy library. |
| unreal.EditorLevelLibrary.set_level_viewport_camera_info | unreal.UnrealEditorSubsystem.set_level_viewport_camera_info  | Viewport camera moved to UnrealEditorSubsystem. |
| unreal.EditorAssetLibrary.load_asset                | unreal.EditorAssetSubsystem.load_asset                            | EditorAssetLibrary deprecated in 5.1; subsystem in 5.1+. |
| unreal.EditorAssetLibrary.save_asset                | unreal.EditorAssetSubsystem.save_asset                            | Same. |
| unreal.EditorAssetLibrary.duplicate_asset           | unreal.EditorAssetSubsystem.duplicate_asset                       | Same. |
| unreal.EditorAssetLibrary.delete_asset              | unreal.EditorAssetSubsystem.delete_asset                          | Same. |
| unreal.EditorAssetLibrary.does_asset_exist          | unreal.EditorAssetSubsystem.does_asset_exist                      | Same. |
| unreal.EditorAssetLibrary.find_asset_data           | unreal.EditorAssetSubsystem.find_asset_data                       | Same. |
| unreal.EditorAssetLibrary.list_assets               | unreal.EditorAssetSubsystem.list_assets                           | Same. |
| unreal.EditorAssetLibrary.rename_asset              | unreal.EditorAssetSubsystem.rename_asset                          | Same. |
| unreal.EditorFilterLibrary                          | unreal.EditorAssetSubsystem.find_asset_data + manual filter       | EditorFilterLibrary subsumed by AssetRegistry queries. |
| unreal.EditorLevelUtils                             | unreal.LevelEditorSubsystem                                       | Level streaming + visibility moved to LevelEditorSubsystem. |
| EditorLevelLibrary                                  | EditorActorSubsystem                                              | Catch-all bare-name reference. |
| EditorAssetLibrary                                  | EditorAssetSubsystem                                              | Catch-all bare-name reference. |
| EditorFilterLibrary                                 | EditorAssetSubsystem.find_asset_data                              | Catch-all bare-name reference. |
| EditorLevelUtils                                    | LevelEditorSubsystem                                              | Catch-all bare-name reference. |
