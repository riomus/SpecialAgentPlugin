# SpecialAgent MCP — UE5 Python API Guidance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the SpecialAgent MCP server teach the LLM how to use UE5 Python correctly — via an always-on cheat sheet, on-demand markdown resources, eight live introspection tools, prompt fixes, and a full pass over all ~294 tool descriptions enforced by a unit test.

**Architecture:** Three layers — (1) `initialize.instructions` is loaded from `Plugins/SpecialAgentPlugin/Content/Docs/ue5_python_cheatsheet.md`; (2) `resources/list` + `resources/read` serve markdown from the same `Content/Docs/` tree; (3) eight new `python/*` tools query the running `unreal` module on the game thread (and one C++ regex-based deprecation linter). A new automation test enforces a uniform description shape across all services.

**Tech Stack:** Unreal Engine 5.6/5.7 plugin (C++), `IPythonScriptPlugin`, `FRegexMatcher`, UE Automation test framework (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), MCP / JSON-RPC 2.0 over HTTP.

**Spec:** [`Plugins/SpecialAgentPlugin/docs/superpowers/specs/2026-05-05-mcp-ue5-api-guidance-design.md`](../specs/2026-05-05-mcp-ue5-api-guidance-design.md)

**Project conventions to honor (from `CLAUDE.md`):**
- No git worktrees on this UE project — work in the main checkout.
- Surgical changes: every changed line traces to spec.
- TDD where it pays: doc-quality test is the lever for the description audit.
- All UE-API mutations dispatch via `FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<T>` (this is the project rule from `IMCPService.h`).
- Each handler: `{"success": bool, "error"?: string, ...payload}`. Log on success at Log verbosity, Warning on failure, prefix `SpecialAgent:`.

**Build/test invocation (used throughout):**
- Build: macOS — open `RBR720Simulator.uproject` in Unreal Editor, hit Compile, watch for Output Log "SpecialAgent: ..." lines. Or close-and-reopen if hot-reload glitches.
- Automation tests: Editor → Window → Test Automation → filter `SpecialAgent.*` → Run.
- End-to-end MCP probes: `curl http://localhost:8767/codex` per the README.

**Total tasks:** 16. Phases 1-4 are sequential foundation; Phase 5 is the description audit (one failing test + nine fix batches).

---

## File structure (will be created or modified)

**Created:**
```
Plugins/SpecialAgentPlugin/Content/Docs/
├── ue5_python_cheatsheet.md
├── deprecations.md
└── idioms/
    ├── load_asset.md
    ├── spawn_actor.md
    ├── transactions.md
    ├── material_params.md
    ├── shader_compile_progress.md
    └── sampler_types.md

Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Tests/
└── MCPDocQualityTest.cpp                    (NEW)
```

**Modified:**
```
Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp
Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/PythonService.cpp
Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/Services/PythonService.h
Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/*.cpp     (audit pass, ~45 files)
```

`Build.cs` does not change — `FRegexMatcher` is in `Core`, `Json`/`JsonUtilities` already linked, no new modules needed.

---

## Phase 1 — Markdown corpus (no C++ changes)

The plugin must have these files on disk before any C++ that loads them runs.

### Task 1: Create the always-on cheat sheet

**Files:**
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/ue5_python_cheatsheet.md`

**Why:** This file is what `BuildSpecialAgentInstructions()` will load into `initialize.instructions`. Target ≤2,000 tokens (≈8 KB). Seven sections per spec section A.

- [ ] **Step 1: Create directory + write file**

Create `Plugins/SpecialAgentPlugin/Content/Docs/` and write `ue5_python_cheatsheet.md` with these seven sections (use existing CLAUDE.md as the source of truth — copy verbatim where applicable, condense where verbose):

```markdown
# SpecialAgent — UE5 Python Cheat Sheet

You are driving Unreal Editor 5.6/5.7 via MCP. Read this once at session start.

## 1. Modern subsystems (use these — the Library classes are deprecated)

| Use this                                  | Instead of                          |
|-------------------------------------------|-------------------------------------|
| `unreal.EditorActorSubsystem`             | `unreal.EditorLevelLibrary`         |
| `unreal.EditorAssetSubsystem`             | `unreal.EditorAssetLibrary`         |
| `unreal.LevelEditorSubsystem`             | misc level-edit Library calls       |
| `unreal.UnrealEditorSubsystem`            | viewport/camera Library calls       |
| `unreal.EditorUtilitySubsystem`           | running utilities                   |
| `unreal.AssetEditorSubsystem`             | open/close asset editors            |

Acquire via: `subsys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)`.

## 2. Idempotency

- Check `unreal.EditorAssetLibrary.does_asset_exist(path)` (still the canonical existence check) before re-creating an asset. If it exists, **skip the expensive path** — never `delete_asset` + re-create just to re-run.
- Default `save_asset(only_if_is_dirty=True)`. Pass `False` only when you have a concrete reason.
- Before `set_material_instance_parent`, compare `mi.get_editor_property('parent')`. Reparenting to the same asset still triggers a full MI shader rebuild.
- Prefer modifying the existing parent (e.g. add a static switch) over duplicating and editing.

## 3. Game thread + shader compile

- `IPythonScriptPlugin` runs on the **game thread**. Anything blocking freezes the editor UI. Slowest operations: shader compiles, `set_material_instance_parent`, asset duplication, saves.
- `unreal.ShaderCompilingManager` is **not always exposed** (missing on UE 5.7 macOS). Always:
  ```python
  scm = getattr(unreal, "ShaderCompilingManager", None)
  if scm: ...
  ```
- Use `Content/Python/_shader_progress.py:wait_for_shaders("label")` — polls the compile queue inside a `ScopedSlowTask` with proper degrade.
- Wrap slow work in `unreal.ScopedSlowTask`:
  ```python
  with unreal.ScopedSlowTask(N, "Doing X...") as t:
      t.make_dialog(True)
      t.enter_progress_frame(1, "Step A")
      if t.should_cancel(): return
  ```
- Batch UI-visible edits in `with unreal.ScopedEditorTransaction("...")` so the editor coalesces redraws.
- Logs: prefer `unreal.log` / `unreal.log_warning` / `unreal.log_error` (route through LogPython) over `print`.

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

When you set a default texture, `load_asset` it first and read `compression_settings` + `srgb` so the sampler choice is **driven by the texture, not guessed**.

## 5. Viewport redraw

Camera writes do **not** repaint until next tick. After any of: `viewport/set_*`, `viewport/focus_actor`, `viewport/orbit_around_actor`, `viewport/set_fov`, `viewport/set_view_mode`, `viewport/toggle_game_view`, `viewport/bookmark_restore`, **or any `python/execute` that touches `UnrealEditorSubsystem.set_level_viewport_camera_info` or other viewport state** — call `viewport/force_redraw` before `screenshot/capture` or `screenshot/save`. Otherwise the captured frame shows the previous view.

## 6. Common idioms

```python
# Load an asset
asset = unreal.EditorAssetLibrary.load_asset("/Game/Foo/Bar")

# Spawn an actor (modern)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actor = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,0))

# Set a property
actor.set_editor_property("hidden_in_game", False)

# Transactional batch edit
with unreal.ScopedEditorTransaction("Layout Trees"):
    for loc in locations:
        eas.spawn_actor_from_class(unreal.StaticMeshActor, loc)
```

## 7. Discovery-first protocol

Before writing Python that calls a class/function you're not 100% certain exists on **this** build:

1. `python/search_symbol substring=...` — list matching names in `dir(unreal)`.
2. `python/get_function_signature class_name=... method=...` — confirm parameter list + types.
3. `python/list_subsystems` — see which `EditorSubsystem` / `EngineSubsystem` classes are loaded right now.
4. `python/diff_against_deprecated snippet=...` — paste your draft to flag deprecated calls before executing.

These are cheap and stop wrong-arg / wrong-API failures cold.
```

- [ ] **Step 2: Sanity check size**

Run: `wc -c Plugins/SpecialAgentPlugin/Content/Docs/ue5_python_cheatsheet.md`
Expected: ≤ 8000 bytes (≈ 2000 tokens). If larger, condense.

- [ ] **Step 3: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Content/Docs/ue5_python_cheatsheet.md
git commit -m "docs(SpecialAgent): add always-on UE5 Python cheat sheet"
```

---

### Task 2: Add `deprecations.md` + 6 idiom files

**Files:**
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/deprecations.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/load_asset.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/spawn_actor.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/transactions.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/material_params.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/shader_compile_progress.md`
- Create: `Plugins/SpecialAgentPlugin/Content/Docs/idioms/sampler_types.md`

**Why:** These are served by `resources/read`. `deprecations.md` is also the **machine-readable input** to the C++ `python/diff_against_deprecated` regex scanner (Task 12). Format choice matters there — see step 1.

- [ ] **Step 1: Write `deprecations.md`** with a parser-friendly table

The C++ scanner (Task 12) parses lines starting with `| ` (markdown table rows) and pulls columns 1 (`deprecated`) and 2 (`modern`). Stick to this exact shape — no nested formatting, no backticks **inside** the columns:

```markdown
# UE5 Python — Deprecated → Modern API mapping

The C++ scanner (`python/diff_against_deprecated`) parses the table below.
Format rule: each table row is `| deprecated | modern | reason |`. No backticks inside columns.

| Deprecated                                        | Modern replacement                                                | Notes |
|---------------------------------------------------|-------------------------------------------------------------------|-------|
| unreal.EditorLevelLibrary.spawn_actor_from_class  | unreal.EditorActorSubsystem.spawn_actor_from_class                | EditorLevelLibrary deprecated in 5.0; subsystem in 5.1+. |
| unreal.EditorLevelLibrary.get_all_level_actors    | unreal.EditorActorSubsystem.get_all_level_actors                  | Same. |
| unreal.EditorLevelLibrary.set_actor_selection_state | unreal.EditorActorSubsystem.set_actor_selection_state           | Same. |
| unreal.EditorLevelLibrary.destroy_actor           | unreal.EditorActorSubsystem.destroy_actor                         | Same. |
| unreal.EditorAssetLibrary.load_asset              | unreal.EditorAssetSubsystem.load_asset                            | EditorAssetLibrary deprecated in 5.1; subsystem in 5.1+. |
| unreal.EditorAssetLibrary.save_asset              | unreal.EditorAssetSubsystem.save_asset                            | Same. |
| unreal.EditorAssetLibrary.duplicate_asset         | unreal.EditorAssetSubsystem.duplicate_asset                       | Same. |
| unreal.EditorAssetLibrary.delete_asset            | unreal.EditorAssetSubsystem.delete_asset                          | Same. |
| unreal.EditorAssetLibrary.does_asset_exist        | unreal.EditorAssetSubsystem.does_asset_exist                      | Same. |
| unreal.EditorFilterLibrary                        | unreal.EditorAssetSubsystem.find_asset_data + manual filter       | EditorFilterLibrary subsumed by AssetRegistry queries. |
| unreal.EditorLevelUtils                           | unreal.LevelEditorSubsystem (level streaming + visibility)        | Library shimmed; prefer subsystem. |
| EditorLevelLibrary                                | EditorActorSubsystem                                              | Catch-all bare-name reference. |
| EditorAssetLibrary                                | EditorAssetSubsystem                                              | Catch-all bare-name reference. |
```

Acquire all subsystems via `unreal.get_editor_subsystem(unreal.<Subsystem>)`.

- [ ] **Step 2: Write the 6 idiom files**

Each ≤ 400 tokens. Patterns + working snippet + one gotcha. Suggested skeletons:

- `idioms/load_asset.md` — `EditorAssetLibrary.load_asset` vs `EditorAssetSubsystem.load_asset`, when to use `find_asset_data` for metadata-only queries, asset-path string form.
- `idioms/spawn_actor.md` — `EditorActorSubsystem.spawn_actor_from_class` + `spawn_actor_from_object`, transform args, attach-to-parent pattern, cleanup with `destroy_actor`.
- `idioms/transactions.md` — `unreal.ScopedEditorTransaction` for undo grouping; `unreal.ScopedSlowTask` for progress UI; combining the two for batch edits.
- `idioms/material_params.md` — MI parameter editing without double-compile (set scalar/vector/texture, **single** save); guard `set_material_instance_parent` with parent-equality check.
- `idioms/shader_compile_progress.md` — `getattr(unreal, "ShaderCompilingManager", None)` pattern; `Content/Python/_shader_progress.py:wait_for_shaders` helper; macOS UE 5.7 absence note.
- `idioms/sampler_types.md` — full table from cheat sheet with explanation; "drive sampler from texture, never guess" rule; example: setting a default texture parameter on a `TextureSample` node.

Keep tone terse; the LLM reads these only when stuck.

- [ ] **Step 3: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Content/Docs/deprecations.md \
        Plugins/SpecialAgentPlugin/Content/Docs/idioms/
git commit -m "docs(SpecialAgent): add deprecations table and idiom cookbooks"
```

---

## Phase 2 — Wire `MCPRequestRouter` to the corpus

### Task 3: Load cheat sheet from disk in `BuildSpecialAgentInstructions`

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:51-125` (the anonymous-namespace `BuildSpecialAgentInstructions` function)

**Why:** Today this is a hardcoded ~1500-token C++ string. We replace with a disk-loader that caches in a static `FString` and falls back to the existing hardcoded text when the file is missing (so an editor running without the file packaged still works).

- [ ] **Step 1: Find plugin Content path helper**

Skim `Source/SpecialAgent/Private/SpecialAgentModule.cpp` for any existing `FPaths::ProjectPluginsDir()` helper. If none, use `IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"))->GetContentDir()`. Confirm the include — `IPluginManager.h` lives in the `Projects` module (already in `Build.cs:54`).

- [ ] **Step 2: Rewrite `BuildSpecialAgentInstructions`**

Replace the function body with:

```cpp
static FString BuildSpecialAgentInstructions()
{
    static FString Cached;
    static bool bLoaded = false;
    if (bLoaded) return Cached;

    auto FallbackHardcoded = []() -> FString {
        return TEXT(
            "SpecialAgent controls Unreal Editor via MCP tools. "
            "See Content/Docs/ue5_python_cheatsheet.md (missing on disk — using fallback). "
            "WORKFLOW: screenshot/capture -> inspect/select/trace -> act -> screenshot/capture. "
            "After camera changes, call viewport/force_redraw before screenshot. "
            "Prefer specific service tools; fall back to python/execute for the long tail."
        );
    };

    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: plugin not found, using fallback instructions"));
        Cached = FallbackHardcoded();
        bLoaded = true;
        return Cached;
    }

    const FString CheatSheetPath = FPaths::Combine(
        Plugin->GetContentDir(), TEXT("Docs"), TEXT("ue5_python_cheatsheet.md"));

    if (!FFileHelper::LoadFileToString(Cached, *CheatSheetPath))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SpecialAgent: cheat sheet not found at %s — using fallback"), *CheatSheetPath);
        Cached = FallbackHardcoded();
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("SpecialAgent: loaded instructions cheat sheet (%d bytes) from %s"),
            Cached.Len(), *CheatSheetPath);
    }

    bLoaded = true;
    return Cached;
}
```

Add the includes near the top of the file (after the existing `#include` block):

```cpp
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
```

- [ ] **Step 3: Build the editor**

Verify Output Log shows `SpecialAgent: loaded instructions cheat sheet (N bytes) from .../Plugins/SpecialAgentPlugin/Content/Docs/ue5_python_cheatsheet.md`. If you see "using fallback" instead, double-check the path in the log.

- [ ] **Step 4: Verify with curl**

```bash
curl -s -X POST http://localhost:8767/codex \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' \
  | python3 -c 'import sys,json; r=json.load(sys.stdin); print(r["result"]["instructions"][:200])'
```

Expected: prints the first 200 chars of your cheat-sheet markdown (`# SpecialAgent — UE5 Python Cheat Sheet ...`).

- [ ] **Step 5: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "feat(SpecialAgent): load initialize.instructions from cheat-sheet markdown"
```

---

### Task 4: Implement `resources/list`

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:540-549` (`HandleResourcesList`)

**Why:** Today returns empty array. Populate with the 3 fixed URIs + one entry per `idioms/*.md` discovered on disk + `mcp://unreal/services` (synthesized from registered services).

- [ ] **Step 1: Add helper `BuildDocResources()` in the anonymous namespace at the top of the file**

```cpp
static TArray<TSharedPtr<FJsonValue>> BuildDocResources()
{
    TArray<TSharedPtr<FJsonValue>> Resources;

    auto AddResource = [&Resources](const FString& Uri, const FString& Name, const FString& Description)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("uri"), Uri);
        R->SetStringField(TEXT("name"), Name);
        R->SetStringField(TEXT("description"), Description);
        R->SetStringField(TEXT("mimeType"), TEXT("text/markdown"));
        Resources.Add(MakeShared<FJsonValueObject>(R));
    };

    AddResource(TEXT("mcp://unreal/cheatsheet"),
        TEXT("UE5 Python cheat sheet"),
        TEXT("Always-on rules: subsystems, idempotency, shader compile, sampler types, redraw, idioms."));

    AddResource(TEXT("mcp://unreal/deprecations"),
        TEXT("Deprecated → modern API table"),
        TEXT("Mapping consumed by python/diff_against_deprecated."));

    AddResource(TEXT("mcp://unreal/services"),
        TEXT("MCP services + tools index"),
        TEXT("Auto-generated browsable index of all registered services and their tools."));

    // Discover idiom files on disk
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
    if (Plugin.IsValid())
    {
        const FString IdiomsDir = FPaths::Combine(Plugin->GetContentDir(), TEXT("Docs"), TEXT("idioms"));
        TArray<FString> IdiomFiles;
        IFileManager::Get().FindFiles(IdiomFiles, *(IdiomsDir / TEXT("*.md")), true, false);
        for (const FString& File : IdiomFiles)
        {
            const FString Stem = FPaths::GetBaseFilename(File);
            AddResource(
                FString::Printf(TEXT("mcp://unreal/idioms/%s"), *Stem),
                FString::Printf(TEXT("Idiom: %s"), *Stem.Replace(TEXT("_"), TEXT(" "))),
                TEXT("Cookbook entry — short example + gotcha."));
        }
    }

    return Resources;
}
```

- [ ] **Step 2: Replace `HandleResourcesList` body**

```cpp
FMCPResponse FMCPRequestRouter::HandleResourcesList(const FMCPRequest& Request)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Resources = BuildDocResources();
    Result->SetArrayField(TEXT("resources"), Resources);
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: resources/list returning %d entries"), Resources.Num());
    return FMCPResponse::Success(Request.Id, Result);
}
```

- [ ] **Step 3: Build, then probe**

```bash
curl -s -X POST http://localhost:8767/codex \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"resources/list"}' \
  | python3 -m json.tool
```

Expected: at least 4 entries (cheatsheet, deprecations, services, ≥1 idiom). Output Log shows `SpecialAgent: resources/list returning N entries`.

- [ ] **Step 4: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "feat(SpecialAgent): populate resources/list from Content/Docs"
```

---

### Task 5: Implement `resources/read` with path-traversal guard + services index

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:552-579` (`HandleResourcesRead`)

**Why:** Map `mcp://unreal/<key>` to a file under `Content/Docs/`. Reject anything that resolves outside that root (no `..`, no absolute-path injection). Also synthesize `mcp://unreal/services` on the fly from `Services` map.

- [ ] **Step 1: Replace `HandleResourcesRead` body**

```cpp
FMCPResponse FMCPRequestRouter::HandleResourcesRead(const FMCPRequest& Request)
{
    FString Uri;
    if (Request.Params.IsValid()) Request.Params->TryGetStringField(TEXT("uri"), Uri);
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: resources/read uri=%s"), *Uri);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Contents;

    auto MakeError = [&](const TCHAR* Msg)
    {
        return FMCPResponse::Error(Request.Id, -32602,
            FString::Printf(TEXT("resources/read: %s"), Msg));
    };

    // Synthesize services index live (not on disk).
    if (Uri == TEXT("mcp://unreal/services"))
    {
        FString Body = TEXT("# SpecialAgent MCP — services + tools index\n\n");
        for (const auto& Pair : Services)
        {
            const TArray<FMCPToolInfo> Tools = Pair.Value->GetAvailableTools();
            Body += FString::Printf(TEXT("## %s\n\n%s\n\n"),
                *Pair.Key, *Pair.Value->GetServiceDescription());
            for (const FMCPToolInfo& T : Tools)
            {
                Body += FString::Printf(TEXT("### %s/%s\n\n%s\n\n"),
                    *Pair.Key, *T.Name, *T.Description);
            }
        }
        TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
        Content->SetStringField(TEXT("uri"), Uri);
        Content->SetStringField(TEXT("mimeType"), TEXT("text/markdown"));
        Content->SetStringField(TEXT("text"), Body);
        Contents.Add(MakeShared<FJsonValueObject>(Content));
        Result->SetArrayField(TEXT("contents"), Contents);
        return FMCPResponse::Success(Request.Id, Result);
    }

    // File-backed URIs all live under Content/Docs/
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
    if (!Plugin.IsValid()) return MakeError(TEXT("plugin not loaded"));
    const FString DocsRoot = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(Plugin->GetContentDir(), TEXT("Docs")));

    FString RelKey;
    if (Uri == TEXT("mcp://unreal/cheatsheet"))     RelKey = TEXT("ue5_python_cheatsheet.md");
    else if (Uri == TEXT("mcp://unreal/deprecations")) RelKey = TEXT("deprecations.md");
    else if (Uri.StartsWith(TEXT("mcp://unreal/idioms/")))
    {
        const FString Stem = Uri.RightChop(FString(TEXT("mcp://unreal/idioms/")).Len());
        // Reject path traversal in the stem itself
        if (Stem.IsEmpty() || Stem.Contains(TEXT("/")) || Stem.Contains(TEXT("\\"))
            || Stem.Contains(TEXT("..")))
        {
            return MakeError(TEXT("invalid idiom name"));
        }
        RelKey = FString::Printf(TEXT("idioms/%s.md"), *Stem);
    }
    else
    {
        return MakeError(TEXT("unknown URI"));
    }

    FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(DocsRoot, RelKey));
    // Normalize separators on both sides so StartsWith is reliable across Windows + Unix
    FString NormalizedRoot = DocsRoot;
    FPaths::NormalizeFilename(NormalizedRoot);
    FPaths::NormalizeFilename(FullPath);
    // Belt-and-suspenders: resolved path must still live under DocsRoot.
    if (!FullPath.StartsWith(NormalizedRoot))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SpecialAgent: resources/read rejected path traversal: %s"), *FullPath);
        return MakeError(TEXT("path escapes docs root"));
    }

    FString Body;
    if (!FFileHelper::LoadFileToString(Body, *FullPath))
    {
        return MakeError(TEXT("file not found"));
    }

    TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
    Content->SetStringField(TEXT("uri"), Uri);
    Content->SetStringField(TEXT("mimeType"), TEXT("text/markdown"));
    Content->SetStringField(TEXT("text"), Body);
    Contents.Add(MakeShared<FJsonValueObject>(Content));
    Result->SetArrayField(TEXT("contents"), Contents);
    return FMCPResponse::Success(Request.Id, Result);
}
```

- [ ] **Step 2: Build, then probe four URIs**

```bash
# happy path
for uri in mcp://unreal/cheatsheet mcp://unreal/deprecations mcp://unreal/services mcp://unreal/idioms/spawn_actor; do
  echo "== $uri =="
  curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/read\",\"params\":{\"uri\":\"$uri\"}}" \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["contents"][0]["text"][:120])'
done

# attack path
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"mcp://unreal/idioms/../../../etc/passwd"}}' \
  | python3 -m json.tool
```

Expected: first four print body excerpts. Attack URI returns an error object (`code: -32602`, message contains "invalid idiom name").

- [ ] **Step 3: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "feat(SpecialAgent): serve docs via resources/read with traversal guard"
```

---

### Task 6: Fix prompts that reference deprecated APIs

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:777-796` (`place_objects`)
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:917-937` (`build_landscape`)
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp:952-964` (`setup_navigation`)

**Why:** Per spec section F.

- [ ] **Step 1: `place_objects` — rewrite step 2**

In the `place_objects` branch, change:

```cpp
"2. Use unreal.EditorLevelLibrary or unreal.EditorAssetLibrary as needed\n"
```

to:

```cpp
"2. Acquire modern subsystems: eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem); "
"easset = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem). "
"Do NOT use unreal.EditorLevelLibrary / unreal.EditorAssetLibrary — both deprecated.\n"
```

- [ ] **Step 2: `build_landscape` — expand step 1**

Change:

```cpp
"1. python/execute to create the landscape actor (UE editor API fallback)\n"
```

to:

```cpp
"1. python/execute to create the landscape actor: "
"eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem); "
"eas.spawn_actor_from_class(unreal.Landscape, unreal.Vector(0,0,0)). "
"Do NOT use deprecated EditorLevelLibrary.\n"
```

- [ ] **Step 3: `setup_navigation` — expand step 1**

Change:

```cpp
"1. python/execute to spawn a NavMeshBoundsVolume covering the playable area\n"
```

to:

```cpp
"1. python/execute to spawn the bounds volume: "
"eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem); "
"eas.spawn_actor_from_class(unreal.NavMeshBoundsVolume, unreal.Vector(0,0,0)) — "
"then set its scale/extent to cover the playable area.\n"
```

- [ ] **Step 4: Build, then probe each prompt**

```bash
for p in place_objects build_landscape setup_navigation; do
  echo "== $p =="
  curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/get\",\"params\":{\"name\":\"$p\",\"arguments\":{\"description\":\"x\",\"size\":\"1\",\"layers\":\"a\"}}}" \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["result"]["messages"][0]["content"])'
done
```

Expected: none of the three printed prompts contain `EditorLevelLibrary` or `EditorAssetLibrary`. All three contain `EditorActorSubsystem` (the latter two also `unreal.get_editor_subsystem`).

- [ ] **Step 5: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "fix(SpecialAgent): point prompts at modern editor subsystems"
```

---

## Phase 3 — Live introspection tools (`python/*`)

All eight extend `PythonService`. The first seven use the existing `IPythonScriptPlugin::ExecPythonCommandEx` + temp-file JSON pattern (see `PythonService.cpp:122-202`). The eighth runs in C++.

Each tool task follows the same shape: declare in header → register in `GetAvailableTools()` → add `Handle*` method → add dispatch line in `HandleRequest`. Build between tasks; smoke-test with one curl probe per tool.

### Task 7: Add `python/help` tool

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/Services/PythonService.h`
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/PythonService.cpp`

- [ ] **Step 1: Header — add private declaration**

Inside `class FPythonService`'s private section (next to `HandleListModules`):

```cpp
FMCPResponse HandleHelp(const FMCPRequest& Request);
```

- [ ] **Step 2: Register the tool in `GetAvailableTools`**

Add this entry inside `FPythonService::GetAvailableTools` (use `FMCPToolBuilder` for consistency with `sky/*`):

```cpp
Tools.Add(FMCPToolBuilder(TEXT("help"),
    TEXT("Return docstring + signature for any unreal.* symbol via help() / inspect.\n"
         "Params: symbol (string, required, e.g. 'unreal.EditorActorSubsystem.spawn_actor_from_class').\n"
         "Workflow: call before guessing API; pair with python/get_function_signature for exact arg types.\n"
         "Warning: large classes can produce long output — truncated to ~8 KB."))
    .RequiredString(TEXT("symbol"), TEXT("Fully-qualified unreal.* symbol path"))
    .Build());
```

- [ ] **Step 3: Add dispatch in `HandleRequest`**

```cpp
if (MethodName == TEXT("help")) return HandleHelp(Request);
```

- [ ] **Step 4: Implement `HandleHelp`**

Pattern: build a short Python wrapper that resolves the symbol, calls `help()`, captures stdout, writes JSON to temp file. Reuse the temp-file mechanism from `HandleExecute`.

**Safety note for the wrapper text in Tasks 7-13:** the LLM-supplied `symbol` / `class_name` / `enum_name` / `asset_path` strings are interpolated **into a Python source literal**. A stray apostrophe, backslash, or newline would break the wrapper. Always pass these via `repr()` inside the Python or escape with `Symbol.ReplaceCharWithEscapedChar()` before `FString::Printf`. The skeleton below uses `repr()`-style triple-single-quoted literals to sidestep the problem.

Skeleton:

```cpp
FMCPResponse FPythonService::HandleHelp(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString Symbol;
    if (!Request.Params->TryGetStringField(TEXT("symbol"), Symbol))
        return InvalidParams(Request.Id, TEXT("Missing 'symbol'"));

    auto Task = [Symbol]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        IPythonScriptPlugin* P = IPythonScriptPlugin::Get();
        if (!P)
        {
            Result->SetBoolField(TEXT("success"), false);
            Result->SetStringField(TEXT("error"), TEXT("Python plugin unavailable"));
            return Result;
        }

        const FString TempFile = FPaths::Combine(FPaths::ProjectIntermediateDir(),
            TEXT("mcp_python_help.json"));

        // Build a triple-single-quoted Python literal so quotes/newlines in `Symbol`
        // can't break out. We still defensively strip ''' since Python rejects nested
        // triple-single inside a triple-single literal.
        const FString SafeSymbol = Symbol.Replace(TEXT("'''"), TEXT(""));

        const FString Wrap = FString::Printf(TEXT(
            "import io, sys, json, inspect, importlib\n"
            "_buf = io.StringIO()\n"
            "_old = sys.stdout\n"
            "sys.stdout = _buf\n"
            "_doc = ''\n"
            "_sig = ''\n"
            "_ok = True\n"
            "_sym = '''%s'''\n"
            "try:\n"
            "    _parts = _sym.split('.')\n"
            "    obj = importlib.import_module(_parts[0])\n"
            "    for p in _parts[1:]:\n"
            "        obj = getattr(obj, p)\n"
            "    help(obj)\n"
            "    _doc = _buf.getvalue()[:8000]\n"
            "    try:\n"
            "        _sig = str(inspect.signature(obj))\n"
            "    except Exception:\n"
            "        _sig = ''\n"
            "except Exception as _e:\n"
            "    _ok = False\n"
            "    _doc = repr(_e)\n"
            "finally:\n"
            "    sys.stdout = _old\n"
            "    with open(r'%s', 'w', encoding='utf-8') as _f:\n"
            "        json.dump({'symbol':_sym,'doc':_doc,'signature':_sig,'success':_ok}, _f)\n"
        ), *SafeSymbol, *TempFile);

        FPythonCommandEx Cmd;
        Cmd.Command = Wrap;
        Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
        Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;
        P->ExecPythonCommandEx(Cmd);

        FString Json;
        if (FFileHelper::LoadFileToString(Json, *TempFile))
        {
            TSharedPtr<FJsonObject> Parsed;
            TSharedRef<TJsonReader<>> Rd = TJsonReaderFactory<>::Create(Json);
            if (FJsonSerializer::Deserialize(Rd, Parsed) && Parsed.IsValid())
            {
                Result = Parsed;
            }
            IFileManager::Get().Delete(*TempFile);
        }
        if (!Result->HasField(TEXT("success")))
            Result->SetBoolField(TEXT("success"), false);
        return Result;
    };

    return FMCPResponse::Success(Request.Id,
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}
```

- [ ] **Step 5: Build + probe**

```bash
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"python/help","arguments":{"symbol":"unreal.EditorActorSubsystem"}}}' \
  | python3 -m json.tool
```

Expected: `result.content[0].text` JSON contains `success: true`, `doc` starts with `"Help on class EditorActorSubsystem in module unreal:"` (or similar).

- [ ] **Step 6: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/Services/PythonService.h \
        Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/PythonService.cpp
git commit -m "feat(SpecialAgent/python): add python/help introspection tool"
```

---

### Task 8: Add `python/inspect_class`

**Files:** same two files as Task 7.

Same shape — decl + register + dispatch + handler. Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("inspect_class"),
    TEXT("List methods, properties, and inheritance chain for an unreal.* class.\n"
         "Params: class_name (string, required, e.g. 'unreal.EditorActorSubsystem' or bare 'EditorActorSubsystem').\n"
         "Workflow: discovery before guessing API; gives you the menu of callable methods.\n"
         "Warning: includes inherited members; chain is in __mro__ order."))
    .RequiredString(TEXT("class_name"), TEXT("Class name (full path or bare name)"))
    .Build());
```

Handler runs Python that resolves the class (try `unreal.<bare>` first, then full dotted path), then emits JSON `{class, mro:[], methods:[{name,signature,doc_first_line}], properties:[...]}` via `inspect.getmembers` filtered by `inspect.isfunction` / `inspect.ismethod` and `inspect.isdatadescriptor`. Cap at 200 entries each.

Smoke probe: `class_name=unreal.EditorActorSubsystem` should return ≥10 methods including `spawn_actor_from_class`.

Commit message: `feat(SpecialAgent/python): add python/inspect_class introspection tool`.

---

### Task 9: Add `python/list_subsystems`

Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("list_subsystems"),
    TEXT("List all unreal.EditorSubsystem and unreal.EngineSubsystem subclasses available in this build.\n"
         "Params: (none).\n"
         "Workflow: this is the modern UE5 entry-point catalog; pick a subsystem here before reaching for any *Library class.\n"
         "Warning: only subsystems whose modules are loaded in the current editor are listed."))
    .Build());
```

Handler runs:
```python
subs = []
for base_name in ("EditorSubsystem", "EngineSubsystem"):
    base = getattr(unreal, base_name, None)
    if base is None: continue
    for cls in base.__subclasses__():
        doc = (cls.__doc__ or "").strip().splitlines()[:1]
        subs.append({"name": cls.__name__, "base": base_name, "doc_first_line": (doc[0] if doc else "")})
```

Smoke probe: returns ≥10 entries on a typical 5.6 editor.

---

### Task 10: Add `python/search_symbol`

Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("search_symbol"),
    TEXT("Substring-search dir(unreal) for matching class / function / enum names.\n"
         "Params: substring (string, required, case-insensitive).\n"
         "Workflow: cheapest discovery primitive; e.g. substring='Foliage' surfaces every Foliage* type/subsystem.\n"
         "Warning: capped at 200 matches; refine substring if 'truncated':true."))
    .RequiredString(TEXT("substring"), TEXT("Case-insensitive substring to match against names in dir(unreal)"))
    .Build());
```

Handler: `[n for n in dir(unreal) if substring.lower() in n.lower()][:200]`. Return `{matches, truncated}`.

Smoke probe: `substring=Foliage` → ≥5 matches including `FoliageActor`, `FoliageType`.

---

### Task 11: Add `python/get_function_signature`

Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("get_function_signature"),
    TEXT("Return parameter list, types, and return type for an unreal.<Class>.<method>.\n"
         "Params: class_name (string, required), method (string, required).\n"
         "Workflow: confirm exact arg order/types before calling; saves wrong-arg crashes.\n"
         "Warning: signatures come from inspect; default values may print as <unreal.Foo object>."))
    .RequiredString(TEXT("class_name"), TEXT("Class name (full path or bare)"))
    .RequiredString(TEXT("method"), TEXT("Method name on that class"))
    .Build());
```

Handler resolves `getattr(cls, method)` and emits `inspect.signature(...)` plus a structured `params: [{name, kind, default, annotation}]` derived from `sig.parameters`. Wrap in try/except — `inspect.signature` does not always work on UE-bound methods; on failure, fall back to the docstring's first line.

Smoke probe: `class_name=unreal.EditorActorSubsystem method=spawn_actor_from_class` → returns a signature string.

---

### Task 12: Add `python/list_enum_values`

Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("list_enum_values"),
    TEXT("Dump all values of an unreal enum (e.g. unreal.ETextureSourceFormat).\n"
         "Params: enum_name (string, required).\n"
         "Workflow: pair with material / asset_import / foliage tools whose params expect enum values.\n"
         "Warning: caps at 500 values; raises error if the symbol isn't an enum."))
    .RequiredString(TEXT("enum_name"), TEXT("Full path or bare name of the unreal enum"))
    .Build());
```

Handler: `list(enum_cls)` mapping each member to `{name, value: int(member)}`. Return `{enum, values, count}`. Raise on non-enum types.

Smoke probe: `enum_name=ETextureSourceFormat` returns ≥3 entries.

---

### Task 13: Add `python/get_asset_class_for_path`

Tool description:

```cpp
Tools.Add(FMCPToolBuilder(TEXT("get_asset_class_for_path"),
    TEXT("Look up the Python class that an asset path resolves to (so you load it via the right API).\n"
         "Params: asset_path (string, required, e.g. '/Game/Foo/Bar' or '/Game/Foo/Bar.Bar').\n"
         "Workflow: call before unreal.load_asset / EditorAssetSubsystem.load_asset to avoid type-cast surprises.\n"
         "Warning: returns {exists:false} cleanly when the path is missing — does not raise."))
    .RequiredString(TEXT("asset_path"), TEXT("Content browser asset path"))
    .Build());
```

Handler: use `EditorAssetSubsystem.find_asset_data(path).asset_class_path` (returns `unreal.TopLevelAssetPath`). Return `{class_path, package_name, exists}`. If the asset is missing, `exists=false`, no error.

Smoke probe: existing `/Game/StarterContent/Materials/M_Basic_Floor` → `class_path` ends in `Material`.

---

### Task 14: Add `python/diff_against_deprecated` (C++ scan)

**Files:**
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/Services/PythonService.h` — add `HandleDiffAgainstDeprecated`.
- Modify: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/PythonService.cpp` — add a private `LoadDeprecationsTable()` helper + the handler.

**Why:** Pure C++ regex scan of an input snippet against `deprecations.md`. No Python execution, no game-thread dispatch.

**Includes:** add to the top of `PythonService.cpp` if not already present:

```cpp
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
```

(`FFileHelper` and `IPluginManager.h` are already used elsewhere in this file path-wise but the includes need to be present in `PythonService.cpp` specifically.)

- [ ] **Step 1: Tool description**

```cpp
Tools.Add(FMCPToolBuilder(TEXT("diff_against_deprecated"),
    TEXT("Scan a Python snippet for calls to deprecated UE5 APIs and suggest modern replacements.\n"
         "Params: snippet (string, required).\n"
         "Workflow: paste your draft before python/execute — finds EditorLevelLibrary, EditorAssetLibrary, etc.\n"
         "Warning: substring match; false positives possible inside string literals."))
    .RequiredString(TEXT("snippet"), TEXT("Python source to scan"))
    .Build());
```

- [ ] **Step 2: Helper — load + parse `deprecations.md` once**

Anonymous-namespace static in `PythonService.cpp`:

```cpp
struct FDeprecationEntry
{
    FString Deprecated;
    FString Modern;
    FString Notes;
};

static const TArray<FDeprecationEntry>& GetDeprecationsTable()
{
    static TArray<FDeprecationEntry> Table;
    static bool bLoaded = false;
    if (bLoaded) return Table;
    bLoaded = true;

    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
    if (!Plugin.IsValid()) return Table;
    const FString Path = FPaths::Combine(Plugin->GetContentDir(), TEXT("Docs"), TEXT("deprecations.md"));

    FString Body;
    if (!FFileHelper::LoadFileToString(Body, *Path))
    {
        UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: deprecations.md not found at %s"), *Path);
        return Table;
    }

    TArray<FString> Lines;
    Body.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);
    bool bSeenHeader = false;
    for (const FString& Line : Lines)
    {
        // Need a leading '|', three pipes, no header / divider lines.
        if (!Line.StartsWith(TEXT("| "))) continue;
        if (Line.Contains(TEXT("|---"))) continue;
        // Skip the first header row only (where columns 1/2 are the literal headings),
        // not every row that happens to mention "Deprecated" in its Notes column.
        if (!bSeenHeader)
        {
            bSeenHeader = true;
            // The header row contains the literal column titles; skip it once and continue.
            if (Line.Contains(TEXT("Deprecated")) && Line.Contains(TEXT("Modern"))) continue;
        }

        TArray<FString> Cols;
        Line.ParseIntoArray(Cols, TEXT("|"), /*InCullEmpty=*/false);
        // Cols[0] is the empty span before the first '|'; valid rows produce ≥4 entries.
        if (Cols.Num() < 4) continue;

        FDeprecationEntry E;
        E.Deprecated = Cols[1].TrimStartAndEnd();
        E.Modern     = Cols[2].TrimStartAndEnd();
        E.Notes      = Cols[3].TrimStartAndEnd();
        if (!E.Deprecated.IsEmpty() && !E.Modern.IsEmpty())
        {
            Table.Add(E);
        }
    }
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: loaded %d deprecation entries"), Table.Num());
    return Table;
}
```

- [ ] **Step 3: Handler**

```cpp
FMCPResponse FPythonService::HandleDiffAgainstDeprecated(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString Snippet;
    if (!Request.Params->TryGetStringField(TEXT("snippet"), Snippet))
        return InvalidParams(Request.Id, TEXT("Missing 'snippet'"));

    const TArray<FDeprecationEntry>& Table = GetDeprecationsTable();

    // Pre-split into lines so we can report 1-based line numbers
    TArray<FString> Lines;
    Snippet.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);

    TArray<TSharedPtr<FJsonValue>> Findings;
    for (const FDeprecationEntry& E : Table)
    {
        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            if (Lines[i].Contains(E.Deprecated))
            {
                TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
                F->SetStringField(TEXT("deprecated"), E.Deprecated);
                F->SetStringField(TEXT("modern"), E.Modern);
                F->SetStringField(TEXT("notes"), E.Notes);
                F->SetNumberField(TEXT("line"), i + 1);
                Findings.Add(MakeShared<FJsonValueObject>(F));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("findings"), Findings);
    Result->SetNumberField(TEXT("count"), Findings.Num());
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: diff_against_deprecated -> %d findings"), Findings.Num());
    return FMCPResponse::Success(Request.Id, Result);
}
```

- [ ] **Step 4: Wire dispatch + smoke probe**

```bash
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"python/diff_against_deprecated","arguments":{"snippet":"unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,0))"}}}' \
  | python3 -m json.tool
```

Expected: ≥1 finding pointing `EditorLevelLibrary.spawn_actor_from_class` → `EditorActorSubsystem.spawn_actor_from_class`, line 1.

- [ ] **Step 5: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/Services/PythonService.h \
        Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/PythonService.cpp
git commit -m "feat(SpecialAgent/python): add diff_against_deprecated C++ linter"
```

---

## Phase 4 — Doc-quality enforcement

### Task 15: Add the failing `MCPDocQualityTest`

**Files:**
- Create: `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Tests/MCPDocQualityTest.cpp`

**Why:** TDD lever for Phase 5. Test starts red — every service that hasn't been audited yet fails it. Phase 5 turns it green service-by-service.

- [ ] **Step 1: Write the test**

```cpp
// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCPRequestRouter.h"
#include "Services/IMCPService.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDocQualityTest,
    "SpecialAgent.Docs.ToolDescriptionsMeetQualityBar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPDocQualityTest::RunTest(const FString&)
{
    static const TCHAR* DeprecatedNeedles[] = {
        TEXT("EditorLevelLibrary"),
        TEXT("EditorAssetLibrary"),
        TEXT("EditorFilterLibrary"),
        TEXT("EditorLevelUtils"),
    };

    FMCPRequestRouter Router;  // ctor registers all services
    int32 Failures = 0;

    for (const auto& Pair : Router.GetServicesForTest())
    {
        const FString& Prefix = Pair.Key;
        const TArray<FMCPToolInfo> Tools = Pair.Value->GetAvailableTools();
        for (const FMCPToolInfo& T : Tools)
        {
            const FString Where = FString::Printf(TEXT("%s/%s"), *Prefix, *T.Name);

            if (T.Description.Len() < 80)
            {
                AddError(FString::Printf(TEXT("%s: description < 80 chars"), *Where));
                ++Failures;
            }
            if (!T.Description.Contains(TEXT("Params:")))
            {
                AddError(FString::Printf(TEXT("%s: missing 'Params:' (use 'Params: (none)' for zero-arg)"), *Where));
                ++Failures;
            }
            if (!T.Description.Contains(TEXT("Workflow:")) && !T.Description.Contains(TEXT("Warning:")))
            {
                AddError(FString::Printf(TEXT("%s: missing 'Workflow:' or 'Warning:'"), *Where));
                ++Failures;
            }
            for (const TCHAR* Needle : DeprecatedNeedles)
            {
                if (T.Description.Contains(Needle))
                {
                    AddError(FString::Printf(TEXT("%s: mentions deprecated symbol '%s'"), *Where, Needle));
                    ++Failures;
                }
            }
        }
    }
    return Failures == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Expose `Services` for tests**

`MCPRequestRouter` keeps `Services` private. Add a test accessor in `MCPRequestRouter.h`:

```cpp
public:
    const TMap<FString, TSharedPtr<IMCPService>>& GetServicesForTest() const { return Services; }
```

(Matches the existing `ValidateServices()` style — read-only.)

- [ ] **Step 3: Build, run the test**

Editor → Test Automation → filter `SpecialAgent.Docs.*` → Run.

Expected: **fails** with many errors (the bar is the audit target). Note the count — say it's 220. That's your starting line for Phase 5.

- [ ] **Step 4: Commit**

```bash
git add Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Tests/MCPDocQualityTest.cpp \
        Plugins/SpecialAgentPlugin/Source/SpecialAgent/Public/MCPRequestRouter.h
git commit -m "test(SpecialAgent): add MCPDocQualityTest (initially failing)"
```

---

## Phase 5 — Tool description audit

### Task 16: Audit pass — bring every description to spec

**Files:**
- Modify: every `Plugins/SpecialAgentPlugin/Source/SpecialAgent/Private/Services/*.cpp` whose tools fail the doc-quality test.

**Why:** Drive `MCPDocQualityTest` from red to green. Each description should match the `sky/*` shape:

```
"<one-line: what it does>.\n"
"Params: <name> (<type>, <required|optional>): <brief>; <name>: <brief>.\n"
"Workflow: <how it composes with other tools or what to call before/after>.\n"
"Warning: <gotcha — game-thread cost, irreversibility, deprecation alternatives, idempotency, etc.>"
```

Use `Params: (none)` for zero-arg tools. **Never** mention deprecated symbols (`EditorLevelLibrary`, `EditorAssetLibrary`, `EditorFilterLibrary`, `EditorLevelUtils`); refer instead to subsystems.

This is iterative — work service by service, build, re-run the test, commit each batch.

- [ ] **Step 1: Identify already-passing services**

Quick `Grep` / read pass over `Source/SpecialAgent/Private/Services/*.cpp` for `FMCPToolBuilder` usage with multi-line descriptions. From the existing code: `sky/*` and `python/execute|execute_file|list_modules` already pass; the eight new `python/*` tools added in Phase 3 also pass (built with the audit shape in mind). Anything else is a candidate.

- [ ] **Step 2: Audit batch 1 — core editor services**

Files: `WorldService.cpp`, `AssetService.cpp`, `UtilityService.cpp`, `ViewportService.cpp`, `ScreenshotService.cpp`.

For each tool with a thin description, expand to the four-section shape. Don't change tool names, parameters, or behavior — descriptions only.

After the batch:
- Build.
- Editor → Test Automation → run `SpecialAgent.Docs.ToolDescriptionsMeetQualityBar`.
- Verify error count drops by the number of tools you touched.
- `git commit -m "docs(SpecialAgent): audit core service tool descriptions"`.

- [ ] **Step 3: Audit batch 2 — visuals**

Files: `LightingService.cpp`, `PostProcessService.cpp`, `DecalService.cpp`, `FoliageService.cpp`, `LandscapeService.cpp`. (`SkyService` is already done.)

Same rhythm — expand, build, run test, commit.

- [ ] **Step 4: Audit batch 3 — streaming + world**

Files: `StreamingService.cpp`, `WorldPartitionService.cpp`, `NavigationService.cpp`, `GameplayService.cpp`.

- [ ] **Step 5: Audit batch 4 — performance + diagnostics**

Files: `PerformanceService.cpp`, `ValidationService.cpp`, `LogService.cpp`, `ConsoleService.cpp`.

- [ ] **Step 6: Audit batch 5 — content pipeline**

Files: `AssetImportService.cpp`, `AssetDependencyService.cpp`, `ContentBrowserService.cpp`, `DataTableService.cpp`.

- [ ] **Step 7: Audit batch 6 — programming-facing**

Files: `BlueprintService.cpp`, `MaterialService.cpp`, `ReflectionService.cpp`, `ComponentService.cpp`.

- [ ] **Step 8: Audit batch 7 — runtime + gameplay**

Files: `PhysicsService.cpp`, `AnimationService.cpp`, `AIService.cpp`, `InputService.cpp`, `SoundService.cpp`, `PIEService.cpp`.

- [ ] **Step 9: Audit batch 8 — cinematics**

Files: `SequencerService.cpp`, `NiagaraService.cpp`, `RenderQueueService.cpp`, `RenderingService.cpp`.

- [ ] **Step 10: Audit batch 9 — editor + project**

Files: `EditorModeService.cpp`, `LevelService.cpp`, `ProjectService.cpp`, `SourceControlService.cpp`, `PCGService.cpp`, `ModelingService.cpp`, `HLODService.cpp`.

- [ ] **Step 11: Final test run + verification**

After batch 9:

- Run `SpecialAgent.Docs.ToolDescriptionsMeetQualityBar` — expected: **PASS**, zero errors.
- `curl tools/list` and `wc -c` the response — note the new size; flag if it grew >50% over the pre-audit baseline (reviewer recommendation B).

- [ ] **Step 12: End-to-end smoke**

One last pass against a running editor:

```bash
# (1) initialize.instructions = cheat sheet
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' \
  | python3 -c 'import sys,json; r=json.load(sys.stdin); print("OK" if "Cheat Sheet" in r["result"]["instructions"] else "FAIL")'

# (2) resources/list >= 4
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"resources/list"}' \
  | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["result"]["resources"]))'

# (3) tools/list contains 8 new python/* tools
curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' \
  | python3 -c 'import sys,json; tools=json.load(sys.stdin)["result"]["tools"]; print(sum(1 for t in tools if t["name"].startswith("python/")))'

# (4) prompts no longer mention EditorLevelLibrary
for p in place_objects build_landscape setup_navigation; do
  curl -s -X POST http://localhost:8767/codex -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/get\",\"params\":{\"name\":\"$p\",\"arguments\":{\"description\":\"x\",\"size\":\"1\",\"layers\":\"a\"}}}" \
    | python3 -c "import sys,json; t=json.load(sys.stdin)['result']['messages'][0]['content']; print('$p OK' if 'EditorLevelLibrary' not in t and 'EditorAssetLibrary' not in t else '$p FAIL')"
done
```

Expected: `OK`, `≥4`, `≥11` (3 existing + 8 new), three `OK`s.

- [ ] **Step 13: Final commit**

```bash
git commit --allow-empty -m "docs(SpecialAgent): doc-quality audit complete; MCPDocQualityTest green"
```

---

## Reference: Success criteria recap (mirrors spec § Verification)

1. `initialize.instructions` is the contents of `Content/Docs/ue5_python_cheatsheet.md`.
2. `resources/list` returns ≥4 entries.
3. `resources/read mcp://unreal/cheatsheet` returns the markdown.
4. `resources/read mcp://unreal/idioms/../../../etc/passwd` is rejected.
5. `tools/list` includes 8 new `python/*` tools.
6. `python/list_subsystems` returns ≥10 entries on a live editor.
7. `python/diff_against_deprecated` flags `EditorLevelLibrary` calls.
8. `MCPDocQualityTest` passes.
9. Existing tests under `Tests/` still pass.
10. `prompts/get` for `place_objects` / `build_landscape` / `setup_navigation` contains no `EditorLevelLibrary` / `EditorAssetLibrary` substrings.

## Reference: Skills

@superpowers:test-driven-development — for the doc-quality test loop.
@superpowers:verification-before-completion — run all 10 success-criteria checks before declaring done.
