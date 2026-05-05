# SpecialAgent MCP — UE5 Python API guidance overhaul

**Date:** 2026-05-05
**Scope:** `Plugins/SpecialAgentPlugin`
**Status:** Approved (brainstorm), pending implementation plan

## Problem

The SpecialAgent MCP server exposes ~294 tools across 45 services plus a `python/execute` escape hatch with full UE5 Python API access. Today the LLM-facing surface is thin:

- `initialize.instructions` — single ~1.5k-token C++ string listing service prefixes; no UE5 Python API guidance.
- `tools/list` — ~294 entries; description quality varies from 5-line (`sky/*`) to single-line (most others).
- `resources/list` — returns empty array. Unused.
- `prompts/list` — 16 hand-written prompts; several reference **deprecated** UE5 APIs (`EditorLevelLibrary`, `EditorAssetLibrary` were deprecated in 5.0/5.1; modern equivalents are `EditorActorSubsystem` / `EditorAssetSubsystem`).
- `python/list_modules` — dumps `sys.modules.keys()` truncated to 100, mostly Python noise.

Result: the LLM uses training-era guesses for UE5 Python — calls deprecated `Library` classes, gets sampler types wrong, skips `viewport/force_redraw` after camera writes, doesn't wrap slow work in `ScopedSlowTask`, re-runs non-idempotent scripts.

## Goal

Improve LLM accuracy on UE5 Python by:

1. Putting the highest-value rules **always-on** (no round trip).
2. Making the long tail of API docs **on-demand** (resources + introspection tools).
3. Bringing **all** ~294 tool descriptions to the same quality bar (`sky/*`).
4. Removing deprecated-API references from prompts.
5. Enforcing the doc-quality bar with a unit test so it doesn't drift.

## Non-goals

- Bundling the auto-generated `unreal.py` stub file (~10 MB, version-skew risk; live introspection is build-accurate).
- Auto-generating tool descriptions from C++ reflection.
- Web/HTML docs viewer.
- Changing tool semantics or behavior (this is a docs/discovery pass).

## Architecture

Three layers, ordered by LLM access cost:

```
┌────────────────────────────────────────────────────┐
│ Layer 1 — ALWAYS-ON                                │
│   initialize.instructions = cheat-sheet.md (~2 KB) │
│   No round trip; in every session's context.       │
└────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────┐
│ Layer 2 — ON-DEMAND DOCS                           │
│   resources/list + resources/read serve markdown   │
│   from Plugins/SpecialAgentPlugin/Content/Docs/.   │
│   1 round trip; static, version-controlled.        │
└────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────┐
│ Layer 3 — LIVE INTROSPECTION                       │
│   8 new python/* tools query running unreal module │
│   1 round trip; build-accurate, fresh per call.    │
└────────────────────────────────────────────────────┘
```

The LLM gets the high-frequency rules for free in Layer 1, can pull cookbook material in Layer 2, and queries the actual running editor for ground truth in Layer 3.

## Components

### A. Markdown corpus — `Plugins/SpecialAgentPlugin/Content/Docs/`

New directory shipped with the plugin. Files:

| File | Purpose | Approx. tokens |
|------|---------|---------------|
| `ue5_python_cheatsheet.md` | Always-on; loaded into `initialize.instructions` | ≤2,000 |
| `deprecations.md` | Full deprecated→modern API mapping table | ~1,000 |
| `idioms/load_asset.md` | How to load assets the modern way | ~400 |
| `idioms/spawn_actor.md` | Spawn + transform + idempotent re-spawn | ~400 |
| `idioms/transactions.md` | `ScopedEditorTransaction` + `ScopedSlowTask` | ~400 |
| `idioms/material_params.md` | MI parameter editing without double-compile | ~400 |
| `idioms/shader_compile_progress.md` | `wait_for_shaders` helper, macOS caveat | ~300 |
| `idioms/sampler_types.md` | `compression_settings` → `SAMPLERTYPE_*` table | ~400 |

`ue5_python_cheatsheet.md` covers seven sections (one per CLAUDE.md rule cluster):

1. **Modern subsystems table** — `EditorActorSubsystem` / `EditorAssetSubsystem` / `LevelEditorSubsystem` / `EditorUtilitySubsystem` / `UnrealEditorSubsystem` etc., paired with the deprecated `Library` class each replaces.
2. **Idempotency rules** — check `EAL.does_asset_exist`, don't `delete_asset` + re-create just to re-run, default `save_asset(only_if_is_dirty=True)`.
3. **Game-thread / shader-compile rules** — `unreal.ShaderCompilingManager` may be unbound on macOS / 5.7; always `getattr(unreal, "ShaderCompilingManager", None)`; use `Content/Python/_shader_progress.py:wait_for_shaders`; wrap slow work in `ScopedSlowTask`; batch with `ScopedEditorTransaction`.
4. **Sampler-type table** — `TC_DEFAULT` / `TC_NORMALMAP` / `TC_MASKS` / `TC_GRAYSCALE` / `TC_ALPHA` / `TC_DEFAULT(sRGB off)` → `SAMPLERTYPE_*`. Driven by texture's compression + sRGB, not guessed.
5. **Viewport redraw rule** — already in today's instructions; tightened: list every camera-mutating tool, plus the rule that `python/execute` writes also need `viewport/force_redraw`.
6. **Common idioms** — short snippets: load asset, spawn actor, set property via `set_editor_property`, transaction wrap.
7. **Discovery-first protocol** — *before guessing API, call `python/search_symbol` then `python/get_function_signature`.* Cheap; saves wrong-arg failures.

### B. `MCPRequestRouter.cpp` changes

| Function | Change |
|---|---|
| `BuildSpecialAgentInstructions()` | Load `Content/Docs/ue5_python_cheatsheet.md` from disk on first call; cache in static `FString`. Fall back to today's hardcoded string if file missing (logged as warning). |
| `HandleResourcesList()` | Return entries for `mcp://unreal/cheatsheet`, `mcp://unreal/deprecations`, `mcp://unreal/idioms/<name>` (one per file under `idioms/`), `mcp://unreal/services` (auto-generated index of all MCP tools). |
| `HandleResourcesRead()` | Resolve URI to file under `Content/Docs/`. Path-traversal guarded (reject `..`, absolute paths, anything outside the docs dir). Return markdown body with `mimeType: text/markdown`. |
| `HandlePromptsGet()` | Replace `unreal.EditorLevelLibrary` / `unreal.EditorAssetLibrary` references in `place_objects`, `build_landscape`, `setup_navigation` prompts with modern subsystem names. |

### C. `PythonService` — 8 new tools (Layer 3)

All run on the game thread via the existing `FGameThreadDispatcher` pattern. All wrap `IPythonScriptPlugin::ExecPythonCommandEx` writing JSON output to the existing temp-file mechanism.

| Tool | Implementation sketch | Returns |
|---|---|---|
| `python/help` | `help(symbol)` captured via `io.StringIO` | `{ symbol, doc, signature }` |
| `python/inspect_class` | `dir(cls)` filtered + `inspect.getmembers`, plus `cls.__mro__` | `{ class, methods[], properties[], mro[] }` |
| `python/list_subsystems` | iterate `unreal.EditorSubsystem.__subclasses__()` + `unreal.EngineSubsystem.__subclasses__()` | `{ subsystems: [{name, base, doc_first_line}] }` |
| `python/search_symbol` | `[n for n in dir(unreal) if substring.lower() in n.lower()]`, capped at 200 | `{ matches[], truncated }` |
| `python/get_function_signature` | `inspect.signature(getattr(unreal.Cls, method))` | `{ signature, params[], return_type, doc }` |
| `python/list_enum_values` | `list(unreal.SomeEnum)` with `.name` and `.value` | `{ enum, values[] }` |
| `python/get_asset_class_for_path` | `EditorAssetSubsystem.find_asset_data(path).asset_class_path` | `{ class_path, exists }` |
| `python/diff_against_deprecated` | C++ side: `FRegexMatcher` over input snippet using a deprecated→modern mapping table loaded once from `Content/Docs/deprecations.md` (parsed at first use, cached). Does **not** run Python — pure string scan, no game-thread dispatch needed. | `{ findings: [{deprecated, modern, line}] }` |

The first seven tools run on the game thread via `IPythonScriptPlugin::ExecPythonCommandEx`. `diff_against_deprecated` is a pure C++ scan with no Python execution — kept in `PythonService` for namespace cohesion. All eight live in `PythonService.cpp` / `PythonService.h`; no new service registration.

### D. Tool-description audit

Bring all ~294 tool descriptions to the `sky/*` shape:

```
"<one-sentence what it does>.\n"
"Params: <list each param + brief>\n"
"Workflow: <how it composes with other tools>\n"
"Warning: <gotchas>\n"
```

Touched files: every `Source/SpecialAgent/Private/Services/*.cpp` (the `GetAvailableTools()` body, **not** the handlers). No behavior change.

### E. Doc-quality test — `Source/SpecialAgent/Private/Tests/MCPDocQualityTest.cpp`

New unit test (UE automation framework) that walks `MCPRequestRouter`'s registered services and asserts:

1. Each tool description ≥ 80 chars.
2. Each description contains literal `Params:` and at least one of `Workflow:` or `Warning:`. Tools with no parameters use the convention `Params: (none)` to satisfy the rule without a special-case.
3. No description contains deprecated symbols: `EditorLevelLibrary`, `EditorAssetLibrary`, `EditorFilterLibrary`, `EditorLevelUtils` (those usages exist in user-written code today, but not in user-facing docs).

Test fails the build on regression. Sits alongside the existing `Source/SpecialAgent/Private/Tests/` files.

### F. Prompt fixes

`HandlePromptsGet` strings — rewrites in three prompts whose Python guidance is either deprecated or vague. No schema change.

- `place_objects` (currently mentions `unreal.EditorLevelLibrary or unreal.EditorAssetLibrary` literally): replace those names with `unreal.EditorActorSubsystem / unreal.EditorAssetSubsystem (acquire via unreal.get_editor_subsystem(...))`.
- `build_landscape` (currently says only `"python/execute to create the landscape actor (UE editor API fallback)"`): expand step 1 to direct the LLM to `unreal.EditorActorSubsystem.spawn_actor_from_class(unreal.Landscape, ...)` instead of leaving the API choice open.
- `setup_navigation` (currently says only `"python/execute to spawn a NavMeshBoundsVolume covering the playable area"`): expand step 1 to direct the LLM to `EditorActorSubsystem.spawn_actor_from_class(unreal.NavMeshBoundsVolume, ...)`.

The intent is the same in all three: ensure the prompt explicitly names the modern subsystem entry point so the LLM doesn't fall back to a deprecated `Library` class.

## Data flow

```
LLM session start
  ──▶ initialize
       └─ server reads ue5_python_cheatsheet.md once, caches it
       └─ returns instructions = cheat sheet (~2 KB)
  ──▶ resources/list
       └─ returns 4+ mcp://unreal/* URIs
  ──▶ tools/list
       └─ returns ~302 tools (8 new python/*) all rich-described

LLM working
  1. Cheat sheet in context: knows subsystems, redraw rule, idempotency
  2. Wants more depth → resources/read mcp://unreal/idioms/spawn_actor
  3. Needs signature → python/get_function_signature
  4. Wrote a snippet → python/diff_against_deprecated before executing
  5. Stuck on enum → python/list_enum_values
```

## File layout (new + touched)

```
Plugins/SpecialAgentPlugin/
├── Content/
│   └── Docs/                                       (NEW)
│       ├── ue5_python_cheatsheet.md
│       ├── deprecations.md
│       └── idioms/
│           ├── load_asset.md
│           ├── spawn_actor.md
│           ├── transactions.md
│           ├── material_params.md
│           ├── shader_compile_progress.md
│           └── sampler_types.md
└── Source/SpecialAgent/
    ├── Private/
    │   ├── MCPRequestRouter.cpp                   (modified — load cheat sheet, populate resources, fix prompts)
    │   ├── Services/PythonService.cpp             (modified — 8 new handlers + tool entries)
    │   └── Services/*.cpp                         (modified — full description audit, ~45 files)
    │   └── Tests/MCPDocQualityTest.cpp            (NEW)
    └── Public/
        └── Services/PythonService.h               (modified — 8 new handler decls)
```

## Verification / success criteria

Each is independently verifiable:

1. `curl http://localhost:8767/codex` `initialize` → `instructions` field equals contents of `Content/Docs/ue5_python_cheatsheet.md`. Edit the md, restart server, repeat — change reflected.
2. `resources/list` → returns ≥4 entries with `mcp://unreal/` URIs.
3. `resources/read` `uri=mcp://unreal/cheatsheet` → returns the markdown body, `mimeType: text/markdown`.
4. `resources/read` `uri=mcp://unreal/../etc/passwd` → returns error (path-traversal guard works).
5. `tools/list` → contains the 8 new `python/*` tools beyond today's `execute` / `execute_file` / `list_modules`.
6. `python/list_subsystems` from a live editor → returns ≥10 subsystem names (e.g. `EditorActorSubsystem`, `EditorAssetSubsystem`, `LevelEditorSubsystem`, `EditorUtilitySubsystem`, `UnrealEditorSubsystem`, plus engine ones).
7. `python/diff_against_deprecated` on a snippet using `unreal.EditorLevelLibrary.spawn_actor_from_class(...)` → returns one finding pointing at `EditorActorSubsystem.spawn_actor_from_object`.
8. `MCPDocQualityTest` passes: every tool description has `Params:` + (`Workflow:` or `Warning:`), no deprecated symbol names appear.
9. Existing tests under `Source/SpecialAgent/Private/Tests/` still pass.
10. `prompts/get` for `place_objects` / `build_landscape` / `setup_navigation` → no `EditorLevelLibrary` / `EditorAssetLibrary` substrings.

## Risk / tradeoffs

- **Token budget on `tools/list`**: the description audit adds ~60-150 chars per tool × 294 tools ≈ +20-50 KB. Most clients fetch `tools/list` once per session; acceptable. Mitigation: keep audit descriptions tight; the Layer 2 cookbook handles depth.
- **Cheat sheet drift**: the markdown lives in version control; the doc-quality test catches deprecated mentions in tool descriptions but not in the cheat sheet itself. Mitigation: deprecations.md and the cheat sheet share the same canonical mapping table; reviewers can spot drift in PRs.
- **Live introspection on game thread**: each new tool runs Python; cheap (`dir`, `inspect.signature`) but still serializes through the dispatcher. Acceptable — these are discovery calls, not hot-path.
- **Path traversal in `resources/read`**: must reject `..`, absolute paths, anything resolving outside `Content/Docs/`. Test #4 covers it.

## Out of scope (deferred)

- Bundling `unreal.py` stub file as a fallback for headless / non-editor contexts.
- Auto-generating tool descriptions from C++ reflection.
- A web/HTML docs viewer.
- Cleaning up user-written Python under `Content/Python/*.py` to match the cheat sheet (different problem; LLM-output ergonomics, not server docs).

## Implementation order (suggested for the plan phase)

1. Markdown corpus (cheat sheet, deprecations, 6 idioms files).
2. `MCPRequestRouter` changes (instructions loader, resources/list, resources/read, prompt fixes).
3. 8 new `python/*` tools.
4. Doc-quality test (initially expected to fail).
5. Tool-description audit, service-by-service, until test passes.

Each step is independently testable with `curl` against a running editor.
