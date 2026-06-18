# SpecialAgent — MCP Tool Reference

**337 tools across 45 services.**

Auto-generated from the service sources (`docs/TOOLS.md` step). The live, authoritative list is `tools/list` / `mcp://unreal/services` when connected.

## `ai` (5 tools)

- **`ai/spawn_ai_pawn`** — Spawn a Pawn into the editor world and optionally let it spawn its default AIController.
- **`ai/assign_controller`** — Spawn and possess an AController (or subclass) on an existing Pawn, replacing whatever controller it had.
- **`ai/run_behavior_tree`** — Load a UBehaviorTree asset and run it on a Pawn's AAIController (AAIController::RunBehaviorTree).
- **`ai/set_blackboard_value`** — Write a typed value to a key on a pawn's AIController Blackboard via the matching SetValueAs* call.
- **`ai/stop_ai`** — Stop the logic (BehaviorTree/StateTree) running on a Pawn's AIController via BrainComponent::StopLogic, optionally unpossess it.

## `animation` (5 tools)

- **`animation/play`** — Play an animation asset on a skeletal mesh component via PlayAnimation (puts the component in single-node mode).
- **`animation/stop`** — Stop the single-node animation currently playing on a skeletal mesh component.
- **`animation/set_anim_blueprint`** — Assign an AnimBlueprint (via its generated UAnimInstance class) to a skeletal mesh component using SetAnimInstanceClass.
- **`animation/list_animations`** _[read-only]_ — Enumerate UAnimSequence assets (including subclasses) from the asset registry without loading them.
- **`animation/set_pose`** — Freeze a skeletal mesh on a single pose of an animation: plays the asset paused (play rate 0) at the requested time.

## `asset_deps` (4 tools)

- **`asset_deps/get_references`** _[read-only]_ — List the packages this asset references (forward, package-category dependencies).
- **`asset_deps/get_referencers`** _[read-only]_ — List the packages that reference this asset (reverse, package-category dependencies).
- **`asset_deps/find_unused`** _[read-only]_ — Enumerate assets under a content root that have zero referencers (deletion candidates).
- **`asset_deps/get_dependency_graph`** _[read-only]_ — Build a recursive package-dependency tree rooted at an asset.

## `asset_import` (6 tools)

- **`asset_import/import_fbx`** — Import an FBX file as a StaticMesh / SkeletalMesh / Animation.
- **`asset_import/import_texture`** — Import an image file (.png/.jpg/.tga/.exr/...) as a UTexture2D.
- **`asset_import/import_sound`** — Import a sound file (.wav/.ogg/.flac) as a USoundWave.
- **`asset_import/import_folder`** — Import every file in an OS folder, one at a time, auto-routing each by extension.
- **`asset_import/create_data_table_from_csv`** — Create a new UDataTable asset and populate it from a CSV file on disk.
- **`asset_import/get_import_settings_template`** _[read-only]_ — Return JSON examples of the parameter shapes for every asset_import tool.

## `assets` (16 tools)

- **`assets/list`** _[read-only]_ — List assets from the Asset Registry using cached metadata (no asset load).
- **`assets/find`** _[read-only]_ — Find assets whose short name contains the given substring (case-sensitive Contains).
- **`assets/get_properties`** _[read-only]_ — Read an asset's cached Asset Registry metadata (no asset load).
- **`assets/search`** _[read-only]_ — Free-text search (case-insensitive) across asset short name, object path, and class.
- **`assets/get_bounds`** _[read-only]_ — Get a StaticMesh or SkeletalMesh local-space bounds (cm) and pivot offset for ground-aligned placement.
- **`assets/get_info`** _[read-only]_ — Load an asset and return type-specific details (sizes/bounds in cm).
- **`assets/sync_to_browser`** — Navigate and select assets in the Content Browser UI.
- **`assets/create_folder`** — Create a Content Browser folder (virtual /Game path, not an OS directory).
- **`assets/rename`** — Rename an asset in place, keeping its folder and fixing up references.
- **`assets/delete`** _[destructive]_ — Delete a single asset (resolves object or package path).
- **`assets/move`** — Move/rename an asset to a new folder via a full destination object path.
- **`assets/duplicate`** — Duplicate an asset under a new name (and optional new folder).
- **`assets/save`** — Persist an asset's package to disk.
- **`assets/set_metadata`** — Store a string key/value on the asset's editor metadata (package FMetaData map) and mark the package dirty.
- **`assets/get_metadata`** _[read-only]_ — Read editor metadata from an asset's package FMetaData map.
- **`assets/validate`** _[read-only]_ — Run a lightweight load + IsValidLowLevel check on one or more assets.

## `blueprint` (10 tools)

- **`blueprint/create`** — Create a new Blueprint asset and save it to disk.
- **`blueprint/compile`** — Compile a Blueprint, rebuilding its generated class and refreshing the CDO.
- **`blueprint/add_variable`** — Add a member variable to a Blueprint and save the asset.
- **`blueprint/add_function`** — Add an empty user function graph to a Blueprint and save the asset.
- **`blueprint/set_default_value`** — Set a class-default (CDO) property value on a Blueprint and save the asset.
- **`blueprint/list_functions`** _[read-only]_ — List a Blueprint's user function graphs plus the UFunctions on its generated class.
- **`blueprint/list_variables`** _[read-only]_ — List the member variables declared directly on a Blueprint.
- **`blueprint/open_in_editor`** — Open a Blueprint in its asset editor window (UAssetEditorSubsystem).
- **`blueprint/duplicate`** — Duplicate a Blueprint asset to a new location and save the copy.
- **`blueprint/reparent`** — Reparent a Blueprint to a new base UClass; this tool refreshes nodes, recompiles, and saves for you.

## `component` (7 tools)

- **`component/add`** — Add an instance component to a placed actor in the editor world.
- **`component/remove`** _[destructive]_ — Remove an instance component from a placed actor (RemoveInstanceComponent + DestroyComponent).
- **`component/list`** _[read-only]_ — List every component currently on a placed actor.
- **`component/get_properties`** _[read-only]_ — Read all reflected properties of a component as strings.
- **`component/set_property`** — Set one reflected property on a component (ImportText + PostEditChangeProperty).
- **`component/attach`** — Attach one scene component to another scene component on the SAME actor (AttachToComponent).
- **`component/detach`** — Detach a scene component from its current parent (DetachFromComponent).

## `console` (4 tools)

- **`console/execute`** — Run an arbitrary Unreal console command line against the editor world via GEngine->Exec.
- **`console/list_commands`** _[read-only]_ — Return a static, curated catalog of commonly-used console commands and CVars (stat/show/viewmode toggles, Nanite, Lumen, VSM, etc.).
- **`console/set_cvar`** — Set a console variable by name (priority ECVF_SetByConsole) and read its value back.
- **`console/get_cvar`** _[read-only]_ — Read a console variable's current value in every typed rendering.

## `content_browser` (9 tools)

- **`content_browser/sync_to_folder`** — Navigate and focus the Content Browser on a folder.
- **`content_browser/create_folder`** — Create a folder (virtual /Game path) and focus it in the Content Browser.
- **`content_browser/rename`** — Rename or move an asset to a new full object path, then focus it.
- **`content_browser/delete`** _[destructive]_ — Force-delete an asset by object path with NO reference check and no confirmation dialog.
- **`content_browser/move`** — Move an asset to a new full object path and focus it.
- **`content_browser/duplicate`** — Duplicate an asset to a new full object path and focus the copy.
- **`content_browser/save`** — Save an asset's package to disk and focus it in the Content Browser.
- **`content_browser/set_metadata`** — Set a string metadata tag on a loaded asset via EditorAssetSubsystem and mark the package dirty.
- **`content_browser/get_metadata`** _[read-only]_ — Read a metadata tag from a loaded asset via EditorAssetSubsystem.

## `data_table` (7 tools)

- **`data_table/list_tables`** _[read-only]_ — List every UDataTable asset in the project via the Asset Registry (no asset loads).
- **`data_table/list_rows`** _[read-only]_ — List the row keys (names) of one DataTable.
- **`data_table/get_row`** _[read-only]_ — Read one DataTable row as a JSON object, serialized from the row struct by reflection.
- **`data_table/set_row`** — Overwrite an EXISTING DataTable row from a JSON object (reflection-based).
- **`data_table/add_row`** — Upsert a DataTable row from a JSON object: creates it if absent, replaces it if present (reflection-based).
- **`data_table/delete_row`** _[destructive]_ — Remove a single row from a DataTable (UDataTable::RemoveRow).
- **`data_table/get_row_struct`** _[read-only]_ — Describe a DataTable's row struct schema.

## `decal` (3 tools)

- **`decal/spawn`** — Spawn an ADecalActor into the editor world.
- **`decal/set_material`** — Assign a decal-domain UMaterialInterface to an existing ADecalActor's UDecalComponent.
- **`decal/set_size`** — Set DecalSize (the half-extents of the decal's projection box) on an existing ADecalActor's UDecalComponent.

## `editor_mode` (3 tools)

- **`editor_mode/activate`** — Switch the level editor into one tool mode, deactivating all other modes first.
- **`editor_mode/get_current`** _[read-only]_ — Report which of the well-known editor modes are currently active.
- **`editor_mode/configure_brush`** — Set the sculpt/paint brush radius and/or strength by issuing the Landscape.BrushRadius and Landscape.BrushStrength console commands.

## `foliage` (5 tools)

- **`foliage/paint_in_area`** — Scatter instanced foliage uniformly at random inside a world-space axis-aligned box.
- **`foliage/remove_from_area`** _[destructive]_ — Delete foliage instances whose origin falls inside a world-space axis-aligned box.
- **`foliage/get_density`** _[read-only]_ — Count foliage instances inside a world-space box and report density over the box's XY footprint.
- **`foliage/list_foliage_types`** _[read-only]_ — List every UFoliageType currently registered on the level's InstancedFoliageActor, with live instance counts.
- **`foliage/add_foliage_type`** — Load a UFoliageType asset by path and register it on the level's InstancedFoliageActor (creating that actor if needed) without placing any instances.

## `gameplay` (6 tools)

- **`gameplay/spawn_trigger_volume`** — Spawn an ATriggerVolume (box trigger) into the persistent editor world.
- **`gameplay/spawn_player_start`** — Spawn an APlayerStart (the default Pawn spawn point at PIE/game start) into the editor world.
- **`gameplay/spawn_note`** — Spawn an ANote (editor-only sticky-note actor) used to flag design tasks.
- **`gameplay/spawn_target_point`** — Spawn an ATargetPoint (lightweight transform marker) for AI, cinematics, or pathing hooks.
- **`gameplay/spawn_killz_volume`** — Spawn an AKillZVolume (PhysicsVolume-derived) that destroys actors entering it (FellOutOfWorld).
- **`gameplay/spawn_blocking_volume`** — Spawn an ABlockingVolume (invisible collider) that blocks pawns and physics with no rendering cost.

## `hlod` (3 tools)

- **`hlod/build`** — Build Hierarchical LOD (clusters source actors into ALODActor proxy meshes) for the current editor world.
- **`hlod/clear`** _[destructive]_ — Destroy every ALODActor (built HLOD proxy cluster) in the current editor world.
- **`hlod/set_setting`** — Configure one HierarchicalLODSetup level on the world's AWorldSettings; missing levels up to the requested index are appended.

## `input` (9 tools)

- **`input/list_mappings`** _[read-only]_ — List legacy UInputSettings action and axis mappings.
- **`input/add_action_mapping`** — Add a legacy UInputSettings action mapping (name + key + modifiers).
- **`input/add_axis_mapping`** — Add a legacy UInputSettings axis mapping (name + key + scale).
- **`input/remove_mapping`** _[destructive]_ — Remove legacy UInputSettings action or axis mapping(s) by name, optionally scoped to a single key.
- **`input/list_enhanced_actions`** _[read-only]_ — List Enhanced Input UInputAction assets via the asset registry (recursive, cached metadata — no asset load unless value_type is missing).
- **`input/list_mapping_contexts`** _[read-only]_ — List Enhanced Input UInputMappingContext (IMC) assets via the asset registry, with each one's mapping count.
- **`input/get_mapping_context`** _[read-only]_ — Read all key mappings from one UInputMappingContext asset.
- **`input/add_enhanced_mapping`** — Bind a key to a UInputAction inside a UInputMappingContext (Context->MapKey + MarkPackageDirty, then optional save).
- **`input/remove_enhanced_mapping`** _[destructive]_ — Unbind a key from a UInputAction inside a UInputMappingContext (Context->UnmapKey + MarkPackageDirty, then optional save).

## `landscape` (6 tools)

- **`landscape/get_info`** _[read-only]_ — Report the first ALandscape's quad extents, component layout, and material so you can pick valid edit rectangles.
- **`landscape/sculpt_height`** — Raise or lower every height sample in a rectangular quad region by the same delta, relative to its current height.
- **`landscape/flatten_area`** — Set every height sample in a rectangular quad region to one absolute world-space Z height (a level plateau).
- **`landscape/smooth_area`** — Soften height noise in a rectangular quad region by applying a 3x3 box-average (blur) filter one or more passes; edge samples are kept unchanged each pass.
- **`landscape/paint_layer`** — Paint a single weightmap (Paint mode) layer to a uniform alpha across a rectangular quad region.
- **`landscape/list_layers`** _[read-only]_ — List the landscape's paint (weightmap) layers and whether each has a bound Layer Info Object.

## `level` (5 tools)

- **`level/open`** — Load an existing level into the editor by virtual package path, replacing the current world.
- **`level/new`** — Create a new untitled blank map and make it the active editor world (in memory only, not yet on disk).
- **`level/save_as`** — Save the current editor level to disk at a caller-supplied path, non-modal (no Save-As dialog) so it is safe for unattended editor automation.
- **`level/get_current_path`** _[read-only]_ — Return the world object path and virtual package name of the currently open editor level.
- **`level/list_templates`** _[read-only]_ — Return the static catalog of new-level template names this service documents (Empty, Basic, OpenWorld, VR-Basic, TimeOfDay).

## `lighting` (8 tools)

- **`lighting/spawn_light`** — Spawn a point/spot/directional/rect/sky light actor into the editor world and apply optional intensity/color.
- **`lighting/set_light_intensity`** — Set the intensity of an existing light actor (resolved by its editor label).
- **`lighting/set_light_color`** — Set the light color (filter tint) of an existing light actor, resolved by its editor label.
- **`lighting/set_light_attenuation`** — Set the attenuation radius (point/spot/rect lights) and/or spot-light cone half-angles, on a light resolved by editor label.
- **`lighting/set_light_cast_shadows`** — Toggle shadow casting on a light actor, resolved by its editor label (works on point/spot/directional/rect and SkyLight).
- **`lighting/build_lighting`** — Trigger an editor Build Lighting (Lightmass bake) on the current editor world.
- **`lighting/spawn_reflection_capture`** — Spawn a reflection-capture actor (sphere/box/plane) into the editor world to provide localized cubemap reflections, and apply optional influence radius (sphere) and brightness.
- **`lighting/recapture`** — Rebuild (recapture) the reflection-capture cubemap contents for the current editor world so capture edits become visible (newly spawned captures, brightness/influence_radius changes).

## `log` (4 tools)

- **`log/tail`** _[read-only]_ — Return the most recent log lines captured by this plugin's in-memory ring buffer, oldest-first.
- **`log/clear`** _[destructive]_ — Empty this plugin's in-memory log ring buffer so the next log/tail returns only output from after this call.
- **`log/list_categories`** _[read-only]_ — List a curated set of common UE log category names (LogTemp, LogEngine, LogPython, LogBlueprint, etc.).
- **`log/set_category_verbosity`** — Set the runtime verbosity threshold of a log category (via the editor 'log <Category> <Verbosity>' console command).

## `material` (12 tools)

- **`material/create`** — Create a new empty UMaterial asset in the Content Browser via AssetTools.
- **`material/create_instance`** — Create a UMaterialInstanceConstant (editor/persistent MIC, not a runtime MID) parented to an existing UMaterialInterface.
- **`material/set_scalar_parameter`** — Override a scalar (float) parameter on a UMaterialInstanceConstant asset (editor-only set, cheap, no shader recompile).
- **`material/set_vector_parameter`** — Override a vector parameter on a UMaterialInstanceConstant asset with a linear color (editor-only, cheap, no recompile).
- **`material/set_texture_parameter`** — Override a texture parameter on a UMaterialInstanceConstant asset (editor-only set).
- **`material/set_static_switch`** — Override a static switch on a UMaterialInstanceConstant and rebuild its static permutation.
- **`material/list_parameters`** _[read-only]_ — List scalar/vector/texture parameter NAMES (names only, no values) on a UMaterial or UMaterialInstanceConstant.
- **`material/get_parameters`** _[read-only]_ — List scalar/vector/texture parameters AND their current values on a UMaterial or UMaterialInstanceConstant.
- **`material/add_expression`** — Add a material-graph expression node to a base UMaterial (NOT a UMaterialInstanceConstant) via UMaterialEditingLibrary::CreateMaterialExpression.
- **`material/connect_expression`** — Wire one material expression's output to another expression's input, OR to a base material property, on a base UMaterial.
- **`material/set_base_properties`** — Set top-level rendering properties on a base UMaterial: blend mode, shading model, two-sided, and opacity-mask clip value.
- **`material/recompile`** — Recompile a base UMaterial after graph/property edits via UMaterialEditingLibrary::RecompileMaterial, optionally laying out expression nodes first.

## `modeling` (8 tools)

- **`modeling/boolean_union`** — Boolean-union two static mesh actors via GeometryScript (UDynamicMesh), merging the tool mesh into the target.
- **`modeling/boolean_subtract`** — Boolean-subtract a tool mesh from a target mesh via GeometryScript (UDynamicMesh), carving the tool's volume out of the target.
- **`modeling/extrude`** — Linear-extrude every face of a static mesh actor a fixed distance along one direction via GeometryScript (UDynamicMesh).
- **`modeling/simplify`** — Reduce a static mesh actor's triangle count via GeometryScript (UDynamicMesh).
- **`modeling/add_simple_collision`** — Add a simple collision primitive to a StaticMesh asset via UStaticMeshEditorSubsystem::AddSimpleCollisionsWithNotification, operating on the /Game asset PATH (not an actor).
- **`modeling/set_nanite`** — Enable or disable Nanite on a StaticMesh asset via UStaticMeshEditorSubsystem::SetNaniteSettings, operating on the /Game asset PATH (not an actor).
- **`modeling/generate_lods`** — Build a chain of reduction LODs on a StaticMesh asset via UStaticMeshEditorSubsystem::SetLodsWithNotification, operating on the /Game asset PATH (not an actor).
- **`modeling/manage_sockets`** — Add, remove, or list named attachment sockets (UStaticMeshSocket) on a StaticMesh asset, operating on the /Game asset PATH (not an actor).

## `navigation` (4 tools)

- **`navigation/rebuild_navmesh`** — Rebuild the navigation mesh for the current editor world, then poll until build tasks drain (300s cap).
- **`navigation/test_path`** _[read-only]_ — Compute a synchronous navmesh path between two world points (UNavigationSystemV1::FindPathToLocationSynchronously).
- **`navigation/get_navmesh_bounds`** _[read-only]_ — List every ANavMeshBoundsVolume in the editor world with its bounds, plus a combined box spanning all of them.
- **`navigation/find_nearest_reachable_point`** _[read-only]_ — Snap a world-space point onto the nearest navmesh location (UNavigationSystemV1::ProjectPointToNavigation).

## `niagara` (11 tools)

- **`niagara/spawn_emitter`** — Spawn a Niagara system at a world location and auto-activate it.
- **`niagara/set_parameter`** — Set a float parameter on a spawned Niagara component via SetFloatParameter.
- **`niagara/activate`** — Activate a spawned Niagara component to (re)start emission.
- **`niagara/deactivate`** — Deactivate a spawned Niagara component: stops emission, existing particles finish their lifetime and die off.
- **`niagara/set_user_float`** — Set a User.
- **`niagara/set_user_vec3`** — Set a User.
- **`niagara/list_user_params`** _[read-only]_ — List the User.
- **`niagara/set_user_int`** — Set a User.
- **`niagara/set_user_bool`** — Set a User.
- **`niagara/set_user_color`** — Set a User.
- **`niagara/set_user_object`** — Set a User.

## `pcg` (4 tools)

- **`pcg/list_graphs`** _[read-only]_ — Discover PCG (Procedural Content Generation) graph assets via the asset registry.
- **`pcg/execute_graph`** — Run a PCG graph on an existing scene actor: finds (or adds) a PCGComponent on the actor, assigns the graph, and calls GenerateLocal.
- **`pcg/spawn_pcg_actor`** — Spawn a new APCGVolume at a world location, assign a PCG graph to its built-in PCGComponent, and generate immediately.
- **`pcg/set_graph_parameter`** — Set one typed user-exposed parameter (graph parameter / override) on the PCG graph instance of a scene actor's PCGComponent, then optionally regenerate.

## `performance` (5 tools)

- **`performance/get_statistics`** _[read-only]_ — Summarize editor-world performance in one call: counts actors, sums LOD0 triangles across every static-mesh component, estimates draw calls as distinct (StaticMesh, Material) pairs, and reports process memory.
- **`performance/get_actor_bounds`** _[read-only]_ — Get the world-space axis-aligned bounding box of one actor, resolved by its World Outliner label via GetComponentsBoundingBox.
- **`performance/check_overlaps`** _[read-only]_ — Find actors whose collision overlaps an oriented box, via World OverlapMultiByChannel on the WorldStatic channel.
- **`performance/get_triangle_count`** _[read-only]_ — Sum LOD0 render triangles across static-mesh components in the editor world, optionally limited to actors whose bounds intersect a box.
- **`performance/get_draw_call_estimate`** _[read-only]_ — Estimate draw calls as the number of distinct (StaticMesh, Material) pairs across every static-mesh component in the editor world.

## `physics` (7 tools)

- **`physics/set_simulate_physics`** — Enable or disable rigid-body (Chaos) simulation on a primitive component (SetSimulatePhysics).
- **`physics/apply_impulse`** — Apply an instantaneous impulse (one-shot velocity kick) to a primitive component at its center of mass (AddImpulse).
- **`physics/apply_force`** — Queue a continuous force on a primitive component for the next physics tick (AddForce).
- **`physics/set_linear_velocity`** — Set (or add to) the linear physics velocity of a primitive component (SetPhysicsLinearVelocity), mass-independent.
- **`physics/set_angular_velocity`** — Set (or add to) the angular physics velocity of a primitive component in degrees/sec (SetPhysicsAngularVelocityInDegrees), mass-independent.
- **`physics/set_mass`** — Override the mass of a primitive component in kilograms (SetMassOverrideInKg(NAME_None, kg, bOverrideMass=true)).
- **`physics/set_collision_enabled`** — Set the ECollisionEnabled mode on a primitive component (SetCollisionEnabled).

## `pie` (8 tools)

- **`pie/start`** — Request a Play-In-Editor session for the current editor world (GEditor->RequestPlaySession).
- **`pie/stop`** — Request end of the active Play-In-Editor session (GEditor->RequestEndPlayMap).
- **`pie/pause`** — Pause the active PIE world's gameplay clock (UGameplayStatics::SetGamePaused true).
- **`pie/resume`** — Resume the paused PIE world's gameplay clock (UGameplayStatics::SetGamePaused false).
- **`pie/step_frame`** — Single-step the paused PIE world by one engine frame (GEditor->PlaySessionSingleStepped), advancing on the next engine tick then re-pausing.
- **`pie/toggle_simulate`** — Toggle the active PIE session between Play and Simulate modes (GEditor->RequestToggleBetweenPIEandSIE).
- **`pie/is_playing`** _[read-only]_ — Query current PIE state.
- **`pie/get_world_context`** _[read-only]_ — Return the current world object paths.

## `post_process` (8 tools)

- **`post_process/spawn_volume`** — Spawn an APostProcessVolume into the editor world (Unbound by default = applies to the whole level, ignoring its bounds).
- **`post_process/set_exposure`** — Override AutoExposureBias (overall exposure compensation, in EV stops) on an existing PostProcessVolume.
- **`post_process/set_bloom`** — Override BloomIntensity (glow from bright pixels) on an existing PostProcessVolume.
- **`post_process/set_dof`** — Override cinematic depth-of-field: DepthOfFieldFocalDistance (cm) and/or DepthOfFieldFstop (aperture) on a PostProcessVolume.
- **`post_process/set_color_grading`** — Override the GLOBAL ColorSaturation and/or ColorContrast color-grading wheels on a PostProcessVolume (per-channel RGBY multipliers).
- **`post_process/set_gi`** — Override IndirectLightingIntensity (a multiplier on bounced / indirect global-illumination light) on a PostProcessVolume.
- **`post_process/set_lens_effects`** — Override camera/lens look effects on an existing PostProcessVolume: motion blur, chromatic aberration, vignette, film grain and lens flare.
- **`post_process/set_auto_exposure`** — Override the auto-exposure (eye adaptation) metering on an existing PostProcessVolume: metering method plus optional brightness range, metering percentiles and adaptation speeds.

## `project` (8 tools)

- **`project/get_setting`** _[read-only]_ — Read a single string config value from the game INI hierarchy (GGameIni, backed by DefaultGame.ini).
- **`project/set_setting`** — Write a string config value into the game INI hierarchy and flush it to DefaultGame.ini on disk.
- **`project/get_version`** _[read-only]_ — Get the running Unreal Engine version.
- **`project/list_plugins`** _[read-only]_ — List every discovered plugin with its enabled and mounted state.
- **`project/enable_plugin`** — Enable a plugin in the current project and persist the change to the .uproject file.
- **`project/disable_plugin`** — Disable a plugin in the current project and persist the change to the .uproject file.
- **`project/get_content_path`** _[read-only]_ — Get the absolute OS filesystem path of the project's Content/ directory.
- **`project/get_project_path`** _[read-only]_ — Get the absolute OS filesystem paths of the active .uproject file and its containing project directory.

## `python` (11 tools)

- **`python/execute`** — Run arbitrary Python via IPythonScriptPlugin synchronously on the game thread with full UE5 API access; the primary escape hatch when no purpose-built tool fits.
- **`python/execute_file`** — Read a Python script file off disk and execute it via IPythonScriptPlugin on the game thread (Private execution scope).
- **`python/list_modules`** _[read-only]_ — List the Python modules currently loaded into the embedded interpreter (sorted sys.modules keys, excluding names beginning with '_').
- **`python/help`** _[read-only]_ — Return docstring + signature for any unreal.* symbol via help() / inspect.nParams: symbol (string, required, e.g.
- **`python/inspect_class`** _[read-only]_ — List methods, properties, and inheritance chain for an unreal.* class.nParams: class_name (string, required, e.g.
- **`python/list_subsystems`** _[read-only]_ — List all unreal.EditorSubsystem and unreal.EngineSubsystem subclasses available in this build.nParams: (none).nWorkflow: this is the modern UE5 entry-point catalog; pick a subsystem here before reaching for any *Library class.nWarning: only….
- **`python/search_symbol`** _[read-only]_ — Substring-search dir(unreal) for matching class / function / enum names.nParams: substring (string, required, case-insensitive).nWorkflow: cheapest discovery primitive; e.g.
- **`python/get_function_signature`** _[read-only]_ — Return parameter list, types, and return type for an unreal.<Class>.<method>.nParams: class_name (string, required), method (string, required).nWorkflow: confirm exact arg order/types before calling; saves wrong-arg crashes.nWarning: signat….
- **`python/list_enum_values`** _[read-only]_ — Dump all values of an unreal enum (e.g.
- **`python/get_asset_class_for_path`** _[read-only]_ — Look up the Python class an asset path resolves to (so you load it via the right API).nParams: asset_path (string, required, e.g.
- **`python/diff_against_deprecated`** _[read-only]_ — Scan a Python snippet for calls to deprecated UE5 APIs and suggest modern replacements.nParams: snippet (string, required, Python source to scan).nWorkflow: paste your draft before python/execute — flags every entry from the deprecated-to-m….

## `reflection` (6 tools)

- **`reflection/list_classes`** _[read-only]_ — List native and Blueprint UClass objects currently loaded in the editor, optionally filtered by name prefix and/or base class.
- **`reflection/get_class_info`** _[read-only]_ — Describe one UClass.
- **`reflection/list_properties`** _[read-only]_ — List the FProperty fields on a UClass with their C++ type strings.
- **`reflection/list_functions`** _[read-only]_ — List the UFunction members on a UClass with reconstructed signatures.
- **`reflection/call_function`** — Invoke a UFunction on a live UObject via ProcessEvent with primitive args only (bool, int, float, string, FName, FVector).
- **`reflection/set_asset_property`** — Set any reflected UPROPERTY on any loaded UObject (DataAsset, asset config, settings object, in-memory actor) by parsing a text value through the property's own ImportText.

## `render_queue` (4 tools)

- **`render_queue/queue_sequence`** — Add a Level Sequence as a new job in the Movie Pipeline Queue.
- **`render_queue/set_output`** — Configure the Output setting of a queued classic-config job (directory, resolution, file-name format).
- **`render_queue/get_status`** _[read-only]_ — Report the Movie Pipeline Queue state.
- **`render_queue/start_render`** — Render the current Movie Render Queue with the in-process executor and block until it finishes.

## `rendering` (6 tools)

- **`rendering/set_scalability`** — Set engine scalability quality levels for the editor.
- **`rendering/set_view_mode`** — Set the active Level Editor viewport's render/visualization mode.
- **`rendering/high_res_screenshot`** — Request a high-resolution screenshot of the active viewport at a custom resolution, written to a PNG under <Project>/Saved/Screenshots/.
- **`rendering/toggle_nanite`** — Enable or disable Nanite rendering globally via the r.Nanite console variable.
- **`rendering/toggle_lumen`** — Enable or disable Lumen dynamic global illumination and reflections via r.DynamicGlobalIlluminationMethod and r.ReflectionMethod.
- **`rendering/build_virtual_textures`** — Run the editor 'Build Virtual Textures' pass (FEditorBuildUtils::EditorBuildVirtualTexture) — rebuilds the STREAMING low-mips of every Runtime Virtual Texture (RVT) in the current level into its UVirtualTextureBuilder.

## `screenshot` (2 tools)

- **`screenshot/capture`** _[read-only]_ — Capture the Level Editor viewport as an in-memory image for inline vision (no file written).
- **`screenshot/save`** — Capture the Level Editor viewport and write it to a lossless PNG file on disk.

## `sequencer` (9 tools)

- **`sequencer/create`** — Create a new ULevelSequence (cinematic) asset in the content browser.
- **`sequencer/add_actor_binding`** — Bind an existing level actor to a Level Sequence as a possessable.
- **`sequencer/add_transform_track`** — Add a 3D transform track (with a single full-range section) to an actor binding so it can hold transform keys.
- **`sequencer/add_camera_cut`** — Add a camera cut section to a Level Sequence so the cinematic looks through a bound camera actor over a frame range.
- **`sequencer/add_skeletal_animation_track`** — Add a skeletal animation track to an actor binding and place one UAnimSequence section at a frame so a SkeletalMeshActor plays an animation clip in the sequence.
- **`sequencer/add_audio_track`** — Add a master (unbound) audio track to a Level Sequence and place one sound section at a frame so the cinematic plays a USoundBase.
- **`sequencer/add_keyframe`** — Add a cubic-interpolated transform keyframe on a binding's transform track at a display-rate frame.
- **`sequencer/set_playback_range`** — Set the playback range of a Level Sequence, in Display-Rate frames.
- **`sequencer/play`** — Play a Level Sequence in the editor world.

## `sky` (5 tools)

- **`sky/spawn_sky_atmosphere`** — Spawn an ASkyAtmosphere actor: physically-based sky color, horizon, and aerial perspective.
- **`sky/spawn_height_fog`** — Spawn an AExponentialHeightFog actor: exponential atmospheric fog with height-based density falloff.
- **`sky/spawn_cloud`** — Spawn an AVolumetricCloud actor: a volumetric cloud layer rendered above an ASkyAtmosphere.
- **`sky/spawn_sky_light`** — Spawn an ASkyLight actor: captures the surrounding sky/scene into a cubemap for ambient and image-based (IBL) indirect lighting.
- **`sky/set_sun_angle`** — Rotate a directional light (the sun) to an explicit pitch/yaw or a time-of-day, driving the sky lighting.

## `sound` (4 tools)

- **`sound/play_2d`** — Play a sound non-spatialized (UI-style, no 3D attenuation) via PlaySound2D.
- **`sound/play_at_location`** — Play a spatialized (3D, attenuated) sound at a fixed world location via PlaySoundAtLocation.
- **`sound/spawn_ambient_actor`** — Spawn an AAmbientSound actor at a world location, assign its sound, and optionally set its initial volume.
- **`sound/set_volume_multiplier`** — Set the volume multiplier on an existing AAmbientSound actor's AudioComponent.

## `source_control` (5 tools)

- **`source_control/get_status`** _[read-only]_ — Query revision-control state for one or more files, forcing a fresh status update from the active provider.
- **`source_control/check_out`** _[read-only]_ — Check out files for editing through the active provider (runs an FCheckOut operation).
- **`source_control/revert`** _[destructive]_ — Discard local changes to files, restoring them to the depot/HEAD revision (runs an FRevert operation).
- **`source_control/submit`** — Submit (check in) files with a change description through the active provider (runs an FCheckIn operation).
- **`source_control/list_modified`** _[read-only]_ — Recursively scan a directory and list files that are modified, added, deleted, or checked out (runs FUpdateStatus, then reads cached state).

## `streaming` (5 tools)

- **`streaming/list_levels`** _[read-only]_ — List the streaming sublevels (ULevelStreaming) registered on the current editor world, plus the persistent base level.
- **`streaming/load_level`** — Load a streaming sublevel into the editor world.
- **`streaming/unload_level`** — Unload a registered streaming sublevel: clears should-be-loaded and should-be-visible, requests removal, then flushes streaming.
- **`streaming/set_level_visibility`** — Show or hide an already-loaded streaming sublevel without unloading it, by toggling ULevelStreaming should-be-visible and flushing visibility.
- **`streaming/set_level_streaming_volume`** — Spawn an ALevelStreamingVolume centered at location, sized to extent, and bind it to a streaming sublevel so the volume drives that level's load/visibility.

## `utility` (18 tools)

- **`utility/save_level`** — Save the current/persistent editor level to disk via FEditorFileUtils::SaveCurrentLevel, committing unsaved actor and level edits.
- **`utility/undo`** — Undo recent editor transactions.
- **`utility/redo`** — Redo previously-undone editor transactions.
- **`utility/select_actor`** — Select a single actor in the editor by its outliner label and highlight it in the viewport/outliner.
- **`utility/get_selection`** _[read-only]_ — List the actors currently selected in the editor.
- **`utility/get_selection_bounds`** _[read-only]_ — Get detailed transform and world-space bounds for each currently selected actor.
- **`utility/select_at_screen`** — Pick the actor under a screen-space point in the level viewport by deprojecting to a world ray and line-tracing on the Visibility channel (up to ~1000 m).
- **`utility/focus_asset_in_browser`** — Sync the Content Browser to a single asset, opening the browser and highlighting (selecting) that asset for the user.
- **`utility/deselect_all`** — Clear the entire editor actor selection so nothing is selected.
- **`utility/invert_selection`** — Invert the editor actor selection: every level actor that was selected becomes deselected and every other becomes selected.
- **`utility/select_by_class`** — Select every level actor that is an instance of (or subclass of) a given class.
- **`utility/group_selected`** — Group the currently selected actors into an AGroupActor so editor operations move/transform them together.
- **`utility/ungroup`** — Ungroup any AGroupActors in the current selection, releasing their child actors and destroying the group actors.
- **`utility/begin_transaction`** — Open a named editor undo/redo transaction so subsequent edits are grouped under one undo entry.
- **`utility/end_transaction`** — Close the current editor undo/redo transaction, finalizing everything opened since the matching begin as one undo step.
- **`utility/show_notification`** — Display a transient toast notification in the editor (Slate notification list) visible to the user.
- **`utility/show_dialog`** — Display a modal message dialog and block until the user picks a button.
- **`utility/focus_tab`** — Open or focus an editor tab by its registered tab id via the global tab manager.

## `validation` (3 tools)

- **`validation/validate_selected`** _[read-only]_ — Run UEditorValidatorSubsystem over assets selected in the Content Browser plus the class assets of any selected actors.
- **`validation/validate_level`** _[read-only]_ — Validate the current editor level plus its on-disk package dependencies through UEditorValidatorSubsystem.
- **`validation/list_errors`** _[read-only]_ — Survey the AssetCheck, MapCheck, AssetTools, and LoadErrors message logs for accumulated messages.

## `viewport` (14 tools)

- **`viewport/set_location`** — Move the active Level Editor viewport camera to a world-space location.
- **`viewport/set_rotation`** — Aim the active Level Editor viewport camera by setting its rotation.
- **`viewport/get_transform`** _[read-only]_ — Read the active Level Editor viewport camera transform.
- **`viewport/focus_actor`** — Frame an actor in the active Level Editor viewport (like pressing F on a selection).
- **`viewport/trace_from_screen`** _[read-only]_ — Deproject a screen-space point in the Level Editor viewport and line-trace it along the camera ray against the Visibility channel (trace length 100000 cm).
- **`viewport/orbit_around_actor`** — Place the Level Editor viewport camera on a horizontal ring around an actor's bounds center, looking inward.
- **`viewport/set_fov`** — Set the active Level Editor perspective viewport's horizontal field of view.
- **`viewport/set_view_mode`** — Switch the active Level Editor viewport's render/visualization mode.
- **`viewport/toggle_game_view`** — Toggle Game View on the active Level Editor viewport, flipping its current state.
- **`viewport/bookmark_save`** — Save the active Level Editor viewport camera (location, rotation, and view settings) into a numbered bookmark slot.
- **`viewport/bookmark_restore`** — Jump the active Level Editor viewport camera to a previously saved bookmark.
- **`viewport/set_grid_snap`** — Enable or disable editor position grid snapping, optionally selecting the grid size.
- **`viewport/toggle_realtime`** — Toggle realtime mode on the active Level Editor viewport, flipping its current state.
- **`viewport/force_redraw`** — Synchronously repaint all Level Editor viewports now, committing any queued camera/view changes to pixels before returning.

## `world` (40 tools)

- **`world/list_actors`** _[read-only]_ — List actors in the active editor world (editor-world only; excludes PIE actors).
- **`world/get_actor`** _[read-only]_ — Look up one actor by its outliner label and return its current transform.
- **`world/find_actors_by_tag`** _[read-only]_ — Find every actor that carries the given actor tag (the FName entries in Actor.Tags, not component tags).
- **`world/get_level_info`** _[read-only]_ — Summarize the active editor world: map name, package path, total actor count, and combined world-space bounds.
- **`world/spawn_actor`** — Spawn a single actor into the current editor level at a world-space location.
- **`world/spawn_actors_batch`** — Spawn many actors in one round-trip, reporting per-entry successes and failures.
- **`world/delete_actor`** _[destructive]_ — Destroy one actor in the editor world, resolved by outliner label.
- **`world/delete_actors_batch`** _[destructive]_ — Destroy many actors in one call, resolving each by outliner label.
- **`world/duplicate_actor`** — Duplicate an existing actor via the editor copy/paste path and return the new actor.
- **`world/set_actor_transform`** — Set any combination of world-space location, rotation, and scale on one actor in a single call; omitted components keep their current value.
- **`world/set_actor_location`** — Move an actor to a new world-space location, leaving its rotation and scale unchanged.
- **`world/set_actor_rotation`** — Set an actor's world-space rotation, leaving location and scale unchanged.
- **`world/set_actor_scale`** — Set an actor's 3D scale, leaving location and rotation unchanged.
- **`world/set_actor_property`** — Set any reflected UProperty on an actor by importing a string value via FProperty::ImportText_Direct, then PostEditChange.
- **`world/set_actor_label`** — Rename an actor's outliner display label.
- **`world/set_actor_material`** — Assign a material asset to the given slot on every MeshComponent of an actor (resolved by label).
- **`world/set_material_parameter`** — Set a scalar or vector parameter on an actor's current slot material, persisting to the asset when the slot holds a MaterialInstanceConstant.
- **`world/create_folder`** — Make an outliner folder visible by setting the folder path on the level's WorldSettings actor.
- **`world/move_actor_to_folder`** — Move an actor into an outliner folder (creating the folder implicitly if it does not yet exist).
- **`world/add_actor_tag`** — Add an actor tag (an FName in Actor.Tags) if it is not already present.
- **`world/remove_actor_tag`** _[destructive]_ — Remove an actor tag (an FName in Actor.Tags) if present.
- **`world/measure_distance`** _[read-only]_ — Measure the straight-line and ground-plane distance between two endpoints, each given as a raw point OR an actor (the two styles can be mixed).
- **`world/find_actors_in_radius`** _[read-only]_ — Find every actor whose pivot location lies within a sphere of the given radius around a center point.
- **`world/find_actors_in_bounds`** _[read-only]_ — Find every actor whose pivot location lies inside an axis-aligned bounding box.
- **`world/raycast`** _[read-only]_ — Single line-trace from start to end on the Visibility collision channel.
- **`world/get_ground_height`** _[read-only]_ — Find ground elevation at an (x,y) column by tracing straight down on the Visibility channel from +max_z to -max_z.
- **`world/place_in_grid`** — Spawn a rectangular grid of identical actors: count_x by count_y instances stepped by spacing along +X and +Y from an origin.
- **`world/place_along_spline`** — Spawn N actors evenly spaced by arc length along an existing spline actor's USplineComponent.
- **`world/place_in_circle`** — Spawn N actors evenly distributed around a horizontal circle (in the XY plane at the center's Z).
- **`world/scatter_in_area`** — Randomly scatter N actors inside an axis-aligned box using a deterministic seeded RNG, optionally snapping each to the ground.
- **`world/set_actor_tick_enabled`** — Enable or disable an actor's per-frame Tick.
- **`world/set_actor_hidden`** — Hide or show an actor in BOTH the editor viewport and at runtime (sets SetActorHiddenInGame and SetIsTemporarilyHiddenInEditor together).
- **`world/set_actor_collision`** — Enable or disable all collision on an actor (Actor::SetActorEnableCollision).
- **`world/attach_to`** — Attach one actor (child) to another (parent) so the child follows the parent's transform.
- **`world/detach`** — Detach an actor from its current parent, preserving its world transform.
- **`world/snap_to_floor`** — Drop a set of actors onto the surface beneath them by line-tracing straight down on the Visibility channel, ignoring each actor itself.
- **`world/randomize_transforms`** — Deterministically perturb the transforms of a set of actors using a seeded RNG, applying each change relative to the actor's current transform.
- **`world/set_actor_mesh`** — Swap the UStaticMesh asset on an actor's StaticMeshComponent without respawning the actor.
- **`world/set_world_settings`** — Edit the active level's AWorldSettings: kill-Z plane, world gravity, and/or the default GameMode override.
- **`world/spawn_cine_camera`** — Spawn an ACineCameraActor (cinematic camera) into the current editor level and optionally configure its lens, filmback, and focus on the UCineCameraComponent.

## `world_partition` (5 tools)

- **`world_partition/list_cells`** _[read-only]_ — List every UWorldPartition runtime streaming cell for the editor world.
- **`world_partition/load_cell`** — Load a single World Partition cell into the editor by spawning an FLoaderAdapterShape over the cell's content bounds (falls back to cell bounds), which pins the actors so they appear in the editor world.
- **`world_partition/unload_cell`** — Request that a single World Partition cell unload by calling its Unload() interface.
- **`world_partition/get_loaded_cells`** _[read-only]_ — List only the World Partition cells currently Loaded, Activated, or Always-Loaded (a filtered view of list_cells).
- **`world_partition/force_load_region`** — Force-load every World Partition cell intersecting an axis-aligned box by spawning an FLoaderAdapterShape over the region.

