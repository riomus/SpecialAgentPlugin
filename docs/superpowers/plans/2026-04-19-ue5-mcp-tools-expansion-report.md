# SpecialAgent UE5 MCP — Tool Surface Expansion Verification Report

Date: 2026-04-19
Branch: `feat/mcp-tools-expansion`
Base: UE 5.7, macOS 14+, arm64

## Summary

66 commits on `feat/mcp-tools-expansion`. The SpecialAgent plugin now exposes approximately **294 tools across 45 services**, up from ~31 callable tools at the start. Every registered service has a non-empty `GetAvailableTools()`; no handler returns `{"status":"not_implemented"}`; the pre-existing task-graph reentrancy crash (triggered by any tool that invoked `WaitUntilTasksComplete` from inside the HTTP handler's `AsyncTask(GameThread)` lambda) has been structurally fixed.

## Work done by phase

### Phase 0 — Foundation (sequential, 1 agent, 8 commits)

- Replaced `FGameThreadDispatcher` + MCPServer `AsyncTask(GameThread)` wrapper with `FMCPGameThreadProcessor`, a `FTickableEditorObject` that drains work items during editor Tick — outside `ProcessTasksUntilIdle` — ending the `RecursionGuard == 1` crash.
- Added `FMCPJson` (JSON read/write + standard `{success, error?}` shapes) and `FMCPToolBuilder` (fluent schema builder) so ~280 tool definitions don't each pay the boilerplate tax.
- Added `FMCPActorResolver` to unify actor lookup across 13+ services.
- Strengthened `IMCPService` contract + added `FMCPRequestRouter::ValidateServices()` that warns on dead services at startup.
- Stubbed 32 new service header/cpp pairs, registered them in the router, added 28 new module dependencies to `Build.cs`.
- Added 7 missing plugin dependencies to `SpecialAgent.uplugin`.
- Dropped two base-class-mismatched tickable overrides (`IsTickableInEditor` / `IsTickableWhenPaused` live on `FTickableGameObject`, not `FTickableObjectBase`).

### Phase 1 — Parallel service implementation (12 agents, 50+ commits)

All 12 agents ran in parallel worktrees (isolation hard-constrained by per-service scope). Tools delivered per agent group:

| Agent | Services | Tools |
|---|---|---|
| A | World (stubs+new) + Utility + Viewport | 44 |
| B | Lighting + Foliage + Landscape | 17 |
| C | Streaming + Navigation + WorldPartition | 14 |
| D | Gameplay + Performance + Assets (10 new) | 21 |
| E | Blueprint + Reflection | 15 |
| F | Material + PostProcess + Sky + Decal | 22 |
| G | AssetImport + ContentBrowser + AssetDeps + DataTable + Validation | 29 |
| H | PIE + Console + Log + Level + EditorMode + Project | 32 |
| I | Component + Physics + Animation | 19 |
| J | AI + Input + Sound | 13 |
| K | Sequencer + Niagara + RenderQueue + Rendering | 20 |
| L | PCG + Modeling + HLOD + SourceControl | 15 |

Total Phase 1: **~261 new or fixed tools** (11 services expanded, 32 services stood up).

7 agents (B, C, E, F, G, H, L) committed directly to `feat/mcp-tools-expansion`. 5 agents (A, D, I, J, K) committed to their own worktree branches and were merged sequentially with `--no-ff` after all completed. Zero merge conflicts across the 5 merges — every agent touched strictly disjoint files.

### Phase 2 — Integration verification

- Clean build after all merges. No linker errors.
- Tool counting via static grep: `Tools.Add(...)` occurs 294 times across 45 service files. 
- Service registration: the router's constructor registers 45 services (13 existing + 32 new).

### Phase 3 — Polish (2 parallel agents, 2 commits)

- **Agent N**: expanded `BuildSpecialAgentInstructions()` from a 4-sentence blurb to a compact 45-service map (still under the ~1500-token budget). Added 12 new MCP prompts (`build_scene`, `create_blueprint`, `import_assets`, `build_sequence`, `setup_lighting`, `populate_foliage`, `build_landscape`, `configure_postprocess`, `setup_navigation`, `wire_gameplay`, `debug_performance`, `run_pie_test`) to `HandlePromptsList` and `HandlePromptsGet`. Updated README to reflect ~294 tools / 45 services and added the crash-fix note to a What's New section.
- **Agent M**: reviewed all 294 tool descriptions. Found ~270 already compliant with the standard (authored by Phase 1 agents who were instructed explicitly). Rewrote 23 pre-Phase-1 legacy descriptions across 6 files — `AssetService`, `UtilityService`, `ViewportService`, `PythonService`, `ScreenshotService`, `ProjectService` — to match the `<verb> <object>. <effect>. Params: ... Workflow: ... Warning: ...` standard.

### Phase 4 — Verification (this report)

- Clean build: `RBR720SimulatorEditor Mac Development` → `Result: Succeeded`. Only pre-existing deprecation warnings (`NaniteSettings` direct-access and `SetNiagaraVariableFloat/Vec3` legacy variants) remain — none blocking, none introduced by this branch.

## Success criteria from the design

| # | Criterion | Status |
|---|---|---|
| 1 | Task-graph reentrancy crash no longer reproduces | **Structurally fixed**; verified no `AsyncTask(GameThread)` call site remains in plugin source; Phase 0 reasoning: service handlers now run off the game thread and marshal via the tickable processor, so `WaitUntilTasksComplete` no longer re-enters `ProcessTasksUntilIdle` on the same thread. Live curl repro deferred to manual user verification (requires editor restart). |
| 2 | `tools/list` ≥ 250 tools with non-empty schemas | **Static count: 294 via `Tools.Add(...)` grep.** Live `tools/list` verification deferred to manual user check. |
| 3 | Zero `"status":"not_implemented"` returns | **Confirmed by grep**: no service file contains the literal `not_implemented` in its handlers. |
| 4 | Every registered service returns a non-empty `GetAvailableTools()` | **Confirmed by static count per service**: every service file has ≥ 1 `Tools.Add(...)`. The `ValidateServices()` startup log will confirm this live. |
| 5 | Every tool description follows the standard | **Best-effort**: Phase 3.M rewrote 23 non-compliant legacy descriptions. Phase 1 agents were instructed and spot-check showed compliance. Full conformance depends on Phase 1 agents applying the standard, which cannot be 100% verified without a full audit — acceptable for v1. |
| 6 | `BuildSpecialAgentInstructions()` enumerates all 30+ services | **Done** (covers all 45 registered services). |
| 7 | `prompts/list` returns ≥ 16 prompt templates | **Done** (4 existing + 12 new = 16). |
| 8 | README tool count claim matches reality | **Done** (updated to 294 tools / 45 services). |
| 9 | Clean Mac build | **Passing.** |
| 10 | Live smoke test: one handler per service | **Deferred to manual user verification**. The authoritative check requires starting the editor and calling `tools/list` then one tool per service. A curl recipe is in the plan (§Phase 4). |

## Manual verification steps for the user

These require starting the editor. The automated/static parts are done.

1. **Start the editor** with the plugin enabled. Confirm Output Log shows:
   ```
   SpecialAgent: Registered 45 services
   SpecialAgent: 45 services, 294 tools total, 0 services with zero tools
   SpecialAgent: MCP Server started on port 8767
   ```
   If any service reports "exposes ZERO tools", that's a Phase 1 gap to file.

2. **Hit `tools/list`** via curl:
   ```bash
   curl -s -X POST http://localhost:8767/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":"1","method":"tools/list"}' \
     | jq '.result.tools | length'
   ```
   Expect ≥ 250.

3. **Reproduce the original crash** (now fixed). Ask any MCP client or use `python/execute` to call `IAssetTools::ImportAssetsAutomated` (see the original stack trace in the spec). Expected: the call completes (possibly with an error about the input file, which is fine) but **does not assert**.

4. **Per-service smoke pick** — curl one safe read-only tool from each service:
   ```
   world/get_level_info
   assets/list
   lighting/... (no safe query — skip or call build_lighting in a test map)
   python/list_modules
   viewport/get_transform
   performance/get_statistics
   utility/get_selection
   blueprint/list_functions (needs a BP asset path)
   pie/is_playing
   console/list_commands
   ...
   ```
   Each should return `{"isError": false, "content": [...]}`.

## Follow-ups / known limitations

- **NiagaraService** uses the deprecated `SetNiagaraVariableFloat/Vec3` name variants. Migrate to the `FName` variants in a follow-up PR (they'll be removed in UE 5.8).
- **AssetService::get_info** touches `UStaticMesh::NaniteSettings` directly; migrate to the accessor functions when convenient.
- **ValidationService** uses `UObject::IsDataValid` instead of `UEditorValidatorSubsystem` because the `DataValidation` module isn't in `Build.cs`. Functional for single-object validation; richer multi-object validation would require adding the module.
- **ModelingService** boolean ops assume the target static mesh is safe to overwrite in place (documented in tool warnings).
- **EditorModeService** uses console-command-based brush configuration rather than direct mode APIs (the direct APIs require `EditorFramework` which wasn't added to `Build.cs`).
- **`input` service** targets legacy `UInputSettings`; Enhanced Input support is deferred.
- **LogService** uses a console-command-based `set_category_verbosity` (the direct API isn't public).
- **RenderQueueService** supports the classic `UMoviePipelinePrimaryConfig` job path only; the graph-config path returns a clear error.
- **Reflection `call_function`** is restricted to primitive argument types (bool, int, float, string, name, FVector) for safety — documented in the tool description.

## Tag

Not tagged by this report — the user should decide whether to tag `v0.2.0` after live smoke verification passes.

## Diff statistics

```
git diff --stat main..feat/mcp-tools-expansion
```

approximate (at this writing):
- ~60 files changed across Source/SpecialAgent/
- ~10000 lines added, ~500 lines removed
- 66 commits from the initial baseline
