# SpecialAgent UE5 MCP — Tool Surface Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the current task-graph reentrancy crash and expand the SpecialAgent MCP plugin from ~31 callable tools to ~280 callable tools across 30 services, all implemented as direct UE5 C++ handlers with typed schemas and polished descriptions.

**Architecture:** Tickable-drain game-thread processor replaces the crashing `AsyncTask` dispatcher. A shared `FMCPToolBuilder` + `FMCPJson` + `FMCPActorResolver` foundation eliminates boilerplate. Each of 30 services exposes its full tool surface via `GetAvailableTools()` with matching direct-C++ handlers (no Python codegen in handlers). Work is parallelized: 1 sequential foundation agent → 12 parallel service-group agents → 1 integration agent → 2 parallel polish agents → 1 verification agent.

**Tech Stack:** Unreal Engine 5.6+, C++17, Editor module, JSON-RPC 2.0 over HTTP/SSE. Dependencies already present: `PythonScriptPlugin`, `EditorScriptingUtilities`, `UnrealEd`, `AssetRegistry`, `Foliage`, `Landscape`, `NavigationSystem`, `AIModule`. New dependencies to add per-phase: `Niagara`, `MovieSceneTools`, `LevelSequenceEditor`, `WorldPartitionEditor`, `PCG`, `SourceControl`, `AssetTools`, `InterchangeEngine`, `Blutility`, `GeometryFramework`, `EnhancedInput`, `MovieRenderPipelineCore`.

**Spec:** `Plugins/SpecialAgentPlugin/docs/superpowers/specs/2026-04-19-ue5-mcp-tools-expansion-design.md`

---

## Notes to Executor

1. **No unit-test harness exists** in this plugin. Tests here mean:
   - **Compile test** — the module compiles without errors on Mac (user's platform).
   - **Schema test** — a diagnostic dumped at startup validates every registered service has non-empty `GetAvailableTools()` and tool schemas are well-formed.
   - **Smoke test** — the editor is launched, the MCP server comes up, `curl http://localhost:8767/health` returns 200, and `tools/list` returns the expected tool counts. One handler per service is invoked with a known-good request.
2. **Game thread discipline** — after Phase 0, every handler that touches `UObject`, `GEditor`, `UWorld`, or any editor subsystem MUST run via `FMCPGameThreadProcessor::Get().Enqueue(Task).Get()`. Direct dispatch from the HTTP worker thread WILL crash the editor.
3. **Commit granularity** — commit at the end of each task (one task = one coherent change). Never batch multiple services into one commit.
4. **Reference docs** — consult the spec's Tool Catalog when implementing a service. This plan lists tool names; the spec lists their intent.
5. **Files mentioned below are relative to** `Plugins/SpecialAgentPlugin/Source/SpecialAgent/`.

---

## File Structure (locked in)

### New files (Phase 0)

```
Public/MCPCommon/
  MCPGameThreadProcessor.h      -- tickable drain that replaces the dispatcher
  MCPToolBuilder.h              -- fluent schema builder
  MCPJson.h                     -- JSON read/write helpers
  MCPActorResolver.h            -- actor lookup helpers
  MCPServiceBase.h              -- optional: shared base class for services

Private/MCPCommon/
  MCPGameThreadProcessor.cpp
  MCPToolBuilder.cpp
  MCPJson.cpp
  MCPActorResolver.cpp
```

### Modified files (Phase 0)

```
Private/GameThreadDispatcher.h   -- thinned to a forwarder (or deleted)
Private/MCPRequestRouter.cpp     -- registers new services, adds ValidateServices()
Public/Services/IMCPService.h    -- strengthened contract comments
Source/SpecialAgent/SpecialAgent.Build.cs  -- added module dependencies
```

### New service files (Phase 0 stubs, Phase 1 implementation)

Per service: `Public/Services/<Name>Service.h` + `Private/Services/<Name>Service.cpp`.

Phase 1 expands existing services: `AssetService`, `WorldService`, `ViewportService`, `UtilityService`, `LightingService`, `FoliageService`, `LandscapeService`, `StreamingService`, `NavigationService`, `GameplayService`, `PerformanceService` (11 existing services).

Phase 2 creates new services: `BlueprintService`, `MaterialService`, `AssetImportService`, `PIEService`, `ConsoleService`, `ComponentService`, `EditorModeService`, `LevelService`, `LogService`, `DataTableService`, `AssetDependencyService`, `SequencerService`, `NiagaraService`, `SoundService`, `WorldPartitionService`, `PCGService`, `ContentBrowserService`, `ProjectService`, `ReflectionService`, `PhysicsService`, `AnimationService`, `AIService`, `PostProcessService`, `SkyService`, `DecalService`, `HLODService`, `RenderingService`, `ValidationService`, `SourceControlService`, `RenderQueueService`, `ModelingService`, `InputService` (32 new services… wait, catalog shows 16 new. Correction: 16 new services — the rest are Phase 1 expansions of existing services. Full list in spec §Tool Catalog.)

### Polish (Phase 3)

```
Private/MCPRequestRouter.cpp     -- BuildSpecialAgentInstructions expanded, prompts/list entries added
Private/Services/*.cpp           -- description review pass
README.md                        -- tool count claim updated
```

---

## Phase 0 — Foundation (Sequential, 1 agent)

### Task 0.1: Fix the task-graph reentrancy crash — MCPGameThreadProcessor

**Files:**
- Create: `Public/MCPCommon/MCPGameThreadProcessor.h`
- Create: `Private/MCPCommon/MCPGameThreadProcessor.cpp`
- Modify: `Source/SpecialAgent/SpecialAgent.Build.cs` (add `Projects` if missing — already present; no change expected)

**Context:** The current `FGameThreadDispatcher` uses `AsyncTask(ENamedThreads::GameThread, ...)`. When the executed lambda runs an engine API that itself calls `WaitUntilTasksComplete` (e.g., `ImportAssetsAutomated`, `Compile` for Blueprint, `RebuildNavmesh`), it re-enters `ProcessTasksUntilIdle` on the same named thread queue and trips the `++Queue.RecursionGuard == 1` assert. The fix is to run the lambda during `FTickableEditorObject::Tick()`, which executes outside the task-graph pump.

- [ ] **Step 1: Create the header with the full class shape**

Create `Public/MCPCommon/MCPGameThreadProcessor.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "TickableEditorObject.h"
#include "Containers/Queue.h"
#include "Async/Future.h"
#include "HAL/CriticalSection.h"

/**
 * FMCPGameThreadProcessor
 *
 * Drains enqueued work on the editor tick, OUTSIDE the task-graph
 * ProcessTasksUntilIdle pump. This avoids the recursion-guard crash that
 * occurs when editor subsystems (asset import, PIE, blueprint compile, etc.)
 * call WaitUntilTasksComplete from inside an AsyncTask(GameThread) lambda.
 *
 * Callers on worker threads:
 *     TFuture<int> F = FMCPGameThreadProcessor::Get().Enqueue<int>([]{ return 42; });
 *     int Result = F.Get();   // blocks worker thread until Tick() runs the lambda
 *
 * Never call Enqueue from the game thread — it would self-deadlock.
 */
class SPECIALAGENT_API FMCPGameThreadProcessor : public FTickableEditorObject
{
public:
    static FMCPGameThreadProcessor& Get();

    /** Enqueue a game-thread task. Returns a future that completes after Tick(). */
    template<typename ReturnType>
    TFuture<ReturnType> Enqueue(TFunction<ReturnType()> Task)
    {
        TSharedPtr<TPromise<ReturnType>> Promise = MakeShared<TPromise<ReturnType>>();
        TFuture<ReturnType> Future = Promise->GetFuture();

        FWorkItem Item;
        Item.Run = [Task = MoveTemp(Task), Promise]()
        {
            Promise->SetValue(Task());
        };

        {
            FScopeLock Lock(&QueueLock);
            Pending.Enqueue(MoveTemp(Item));
        }
        return Future;
    }

    // FTickableEditorObject
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return true; }
    virtual bool IsTickableInEditor() const override { return true; }
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
    struct FWorkItem
    {
        TFunction<void()> Run;
    };

    FCriticalSection QueueLock;
    TQueue<FWorkItem, EQueueMode::Mpsc> Pending;

    /** Safety cap: max items to drain per Tick (prevents starvation of editor). */
    static constexpr int32 MaxItemsPerTick = 64;
};
```

- [ ] **Step 2: Create the .cpp**

Create `Private/MCPCommon/MCPGameThreadProcessor.cpp`:

```cpp
#include "MCPCommon/MCPGameThreadProcessor.h"

FMCPGameThreadProcessor& FMCPGameThreadProcessor::Get()
{
    static FMCPGameThreadProcessor Instance;
    return Instance;
}

void FMCPGameThreadProcessor::Tick(float DeltaTime)
{
    int32 Processed = 0;
    FWorkItem Item;
    while (Processed < MaxItemsPerTick && Pending.Dequeue(Item))
    {
        Item.Run();
        ++Processed;
    }
}

TStatId FMCPGameThreadProcessor::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(FMCPGameThreadProcessor, STATGROUP_Tickables);
}
```

- [ ] **Step 3: Make `FGameThreadDispatcher` a forwarder**

Edit `Public/GameThreadDispatcher.h` (currently in `Private/`, move to `Public/` for SPECIALAGENT_API visibility — or keep it Private and note the scope). Replace the body so every entry point goes through the processor:

```cpp
#pragma once
#include "CoreMinimal.h"
#include "MCPCommon/MCPGameThreadProcessor.h"

class SPECIALAGENT_API FGameThreadDispatcher
{
public:
    // Kept for source compatibility with existing call sites during migration.
    template<typename ReturnType>
    static ReturnType DispatchToGameThreadSyncWithReturn(TFunction<ReturnType()> Task)
    {
        checkf(!IsInGameThread(),
            TEXT("FGameThreadDispatcher called from game thread — would self-deadlock. "
                 "Either fix the caller (should only be called from HTTP worker) or "
                 "refactor to not require a sync wait."));
        return FMCPGameThreadProcessor::Get().Enqueue<ReturnType>(MoveTemp(Task)).Get();
    }

    static void DispatchToGameThreadSync(TFunction<void()> Task)
    {
        checkf(!IsInGameThread(), TEXT("FGameThreadDispatcher called from game thread"));
        auto Wrapped = [Task = MoveTemp(Task)]() -> int { Task(); return 0; };
        FMCPGameThreadProcessor::Get().Enqueue<int>(MoveTemp(Wrapped)).Get();
    }

    template<typename ReturnType>
    static TFuture<ReturnType> DispatchToGameThread(TFunction<ReturnType()> Task)
    {
        return FMCPGameThreadProcessor::Get().Enqueue<ReturnType>(MoveTemp(Task));
    }
};
```

(Note: if `GameThreadDispatcher.h` currently lives in `Private/`, keep the move-or-not decision consistent with how other headers are organized. If moving, update any `#include` paths throughout.)

- [ ] **Step 4: Build**

Run a full clean build. Expected: no errors. Existing `DispatchToGameThreadSyncWithReturn` call sites compile unchanged.

```bash
# From the project root (the .uproject's directory):
# Mac:
sh -c 'pushd Engine/Build/BatchFiles/Mac/ && ./RocketEditor.sh ... '
# Or via UBT — the user's preferred Mac build command.
# Simpler: build from within the editor's IDE or UBT CLI.
```

- [ ] **Step 5: Reproduce-then-verify the fix**

Start the editor. With the MCP server running, POST a `tools/call` that Python-imports an asset:

```bash
curl -X POST http://localhost:8767/message -H 'Content-Type: application/json' -d '{
  "jsonrpc":"2.0","id":"1","method":"tools/call",
  "params":{"name":"python/execute","arguments":{"code":
    "import unreal\n"
    "task = unreal.AssetImportTask()\n"
    "# Use a real FBX on disk for your project, or point at any existing file.\n"
    "task.filename = \"/tmp/any_fbx.fbx\"\n"
    "task.destination_path = \"/Game/Imported\"\n"
    "task.automated = True\n"
    "task.replace_existing = True\n"
    "unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])\n"
    "print(\"import path exercised\")\n"
  }}
}'
```

Before the fix this reproduced the `RecursionGuard` assert. After the fix the call completes without a crash (whether the import succeeds or fails on a missing file is irrelevant — the assert must not trigger).

- [ ] **Step 6: Commit**

```bash
git add Source/SpecialAgent/Public/MCPCommon/MCPGameThreadProcessor.h \
        Source/SpecialAgent/Private/MCPCommon/MCPGameThreadProcessor.cpp \
        Source/SpecialAgent/Public/GameThreadDispatcher.h \
        Source/SpecialAgent/Private/GameThreadDispatcher.h
git commit -m "fix(SpecialAgent): replace AsyncTask dispatcher with tickable drain to end task-graph recursion crash"
```

---

### Task 0.2: Shared JSON helpers — MCPJson

**Files:**
- Create: `Public/MCPCommon/MCPJson.h`
- Create: `Private/MCPCommon/MCPJson.cpp`

- [ ] **Step 1: Header**

```cpp
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;

class SPECIALAGENT_API FMCPJson
{
public:
    // Readers
    static bool ReadVec3(const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector& Out);
    static bool ReadRotator(const TSharedPtr<FJsonObject>& Params, const FString& Field, FRotator& Out);
    static bool ReadColor(const TSharedPtr<FJsonObject>& Params, const FString& Field, FLinearColor& Out);
    static bool ReadString(const TSharedPtr<FJsonObject>& Params, const FString& Field, FString& Out);
    static bool ReadNumber(const TSharedPtr<FJsonObject>& Params, const FString& Field, double& Out);
    static bool ReadBool(const TSharedPtr<FJsonObject>& Params, const FString& Field, bool& Out);

    // Writers
    static void WriteVec3(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FVector& V);
    static void WriteRotator(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FRotator& R);
    static void WriteColor(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FLinearColor& C);
    static void WriteActor(const TSharedPtr<FJsonObject>& Out, AActor* Actor);  // standard actor serialization

    // Standard result shapes
    static TSharedPtr<FJsonObject> MakeSuccess();
    static TSharedPtr<FJsonObject> MakeError(const FString& Message);
};
```

- [ ] **Step 2: Implementation**

In `Private/MCPCommon/MCPJson.cpp`:
- `ReadVec3` reads `[X,Y,Z]` array (length 3) into `FVector`. Returns false if field missing or wrong shape.
- `ReadRotator` reads `[Pitch,Yaw,Roll]` into `FRotator`.
- `ReadColor` reads `[R,G,B]` or `[R,G,B,A]` into `FLinearColor` (default A=1).
- Writers output the same array shape.
- `WriteActor` emits `{name, class, location[3], rotation[3], scale[3], tags[]}` — mirror the current `SerializeActor` in `WorldService.cpp`.
- `MakeSuccess` returns `{"success": true}`; `MakeError` returns `{"success": false, "error": <msg>}`.

- [ ] **Step 3: Build & commit**

```bash
git add Source/SpecialAgent/Public/MCPCommon/MCPJson.h \
        Source/SpecialAgent/Private/MCPCommon/MCPJson.cpp
git commit -m "feat(SpecialAgent): shared JSON helpers (FMCPJson)"
```

---

### Task 0.3: Fluent schema builder — MCPToolBuilder

**Files:**
- Create: `Public/MCPCommon/MCPToolBuilder.h`
- Create: `Private/MCPCommon/MCPToolBuilder.cpp`

- [ ] **Step 1: Header**

```cpp
#pragma once
#include "CoreMinimal.h"
#include "Services/IMCPService.h"

class SPECIALAGENT_API FMCPToolBuilder
{
public:
    FMCPToolBuilder(const FString& Name, const FString& Description);

    FMCPToolBuilder& RequiredString(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredNumber(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredInteger(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredBool(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredVec3(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredColor(const FString& Field, const FString& Description);
    FMCPToolBuilder& RequiredEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description);
    FMCPToolBuilder& RequiredArrayOfString(const FString& Field, const FString& Description);

    FMCPToolBuilder& OptionalString(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalNumber(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalInteger(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalBool(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalVec3(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalColor(const FString& Field, const FString& Description);
    FMCPToolBuilder& OptionalEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description);
    FMCPToolBuilder& OptionalArrayOfString(const FString& Field, const FString& Description);

    FMCPToolInfo Build() const;

private:
    FMCPToolInfo Tool;
    void AddParam(const FString& Field, const FString& JsonType, const FString& Description, bool bRequired, const TArray<FString>* Enum = nullptr);
};
```

- [ ] **Step 2: Implementation**

- Each `Required*` / `Optional*` delegates to `AddParam` with the correct JSON Schema `"type"` string: `"string"`, `"number"`, `"integer"`, `"boolean"`, `"array"`, and `"array"` for Vec3/Color (with note in description).
- `AddParam` builds a JSON object with `type` and `description`, and if an enum is provided, also sets `"enum"` to the allowed values. Adds to `Tool.Parameters` under `Field`. If required, also appends to `Tool.RequiredParams`.
- `Build()` returns the completed `FMCPToolInfo` by value.

- [ ] **Step 3: Migrate one existing service to prove the builder works**

Rewrite `WorldService::GetAvailableTools()`'s `spawn_actor` block and `list_actors` block using the builder. Confirm compile and that `tools/list` still returns the same schema shapes.

- [ ] **Step 4: Commit**

```bash
git add Source/SpecialAgent/Public/MCPCommon/MCPToolBuilder.h \
        Source/SpecialAgent/Private/MCPCommon/MCPToolBuilder.cpp \
        Source/SpecialAgent/Private/Services/WorldService.cpp
git commit -m "feat(SpecialAgent): fluent tool schema builder (FMCPToolBuilder)"
```

---

### Task 0.4: Actor lookup helpers — MCPActorResolver

**Files:**
- Create: `Public/MCPCommon/MCPActorResolver.h`
- Create: `Private/MCPCommon/MCPActorResolver.cpp`

- [ ] **Step 1: Header**

```cpp
#pragma once
#include "CoreMinimal.h"

class AActor;
class UClass;
class UWorld;

class SPECIALAGENT_API FMCPActorResolver
{
public:
    static AActor* ByLabel(UWorld* World, const FString& Name);
    static AActor* ByLabelOrPath(UWorld* World, const FString& NameOrPath);
    static TArray<AActor*> ByClass(UWorld* World, UClass* Class);
    static TArray<AActor*> ByTag(UWorld* World, const FName& Tag);
};
```

- [ ] **Step 2: Implementation**

Port the existing `FindActor` lambda from `WorldService.cpp` to `ByLabel`. Implement `ByTag` using `Actor->Tags.Contains(Tag)`. Implement `ByClass` using `TActorIterator`.

- [ ] **Step 3: Migrate existing `FindActor` static in WorldService to use the helper**

Replace all `FindActor(World, ActorName)` call sites in `WorldService.cpp` with `FMCPActorResolver::ByLabel(World, ActorName)`, and delete the local `FindActor` static.

- [ ] **Step 4: Commit**

```bash
git add Source/SpecialAgent/Public/MCPCommon/MCPActorResolver.h \
        Source/SpecialAgent/Private/MCPCommon/MCPActorResolver.cpp \
        Source/SpecialAgent/Private/Services/WorldService.cpp
git commit -m "feat(SpecialAgent): shared actor resolver"
```

---

### Task 0.5: Strengthen the service contract & add startup validator

**Files:**
- Modify: `Public/Services/IMCPService.h` (update comments)
- Modify: `Private/MCPRequestRouter.cpp` (add `ValidateServices()`)
- Modify: `Public/MCPRequestRouter.h` (declare `ValidateServices()`)

- [ ] **Step 1: Comment the contract**

At the top of `IMCPService.h`, replace the current class doc with:

```
/**
 * MCP Service Interface.
 *
 * Contract (enforced by code review + startup validation):
 *   1. GetAvailableTools() MUST return an entry for every branch in HandleRequest().
 *      No unlisted methods. No listed-but-unhandled tools.
 *   2. Every handler returns {"success": bool, "error"?: string, ...payload}.
 *   3. No handler returns {"status": "not_implemented"}. Ship it or remove.
 *   4. Any handler that touches UObject/GEditor/UWorld state MUST run via
 *      FMCPGameThreadProcessor::Get().Enqueue(...).Get().
 *   5. Every handler logs at Log verbosity with "SpecialAgent: " prefix.
 */
```

- [ ] **Step 2: Add `ValidateServices()` to the router**

In `MCPRequestRouter.cpp`:

```cpp
void FMCPRequestRouter::ValidateServices() const
{
    int32 TotalTools = 0;
    for (const auto& Pair : Services)
    {
        TArray<FMCPToolInfo> Tools = Pair.Value->GetAvailableTools();
        if (Tools.Num() == 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("SpecialAgent: service '%s' registered but exposes ZERO tools"),
                *Pair.Key);
        }
        TotalTools += Tools.Num();
    }
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: %d services, %d tools total"),
        Services.Num(), TotalTools);
}
```

Declare it in the header. Call it at the end of the router constructor.

- [ ] **Step 3: Build & run editor; observe the warning list**

Expected on current codebase: warnings for `lighting`, `foliage`, `landscape`, `streaming`, `navigation`, `gameplay`, `performance` (7 services with empty tools). This is the current baseline — these are exactly what Phase 1 will fix.

- [ ] **Step 4: Commit**

```bash
git add Source/SpecialAgent/Public/Services/IMCPService.h \
        Source/SpecialAgent/Public/MCPRequestRouter.h \
        Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "feat(SpecialAgent): strengthen service contract + startup validator"
```

---

### Task 0.6: Stub all 16 new service files + register them in router

**Files:**
- Create (pairs of .h/.cpp under `Public/Services/` and `Private/Services/`) for each new service: `BlueprintService`, `MaterialService`, `AssetImportService`, `PIEService`, `ConsoleService`, `ComponentService`, `EditorModeService`, `LevelService`, `LogService`, `DataTableService`, `AssetDependencyService`, `SequencerService`, `NiagaraService`, `SoundService`, `WorldPartitionService`, `PCGService`, `ContentBrowserService`, `ProjectService`, `ReflectionService`, `PhysicsService`, `AnimationService`, `AIService`, `PostProcessService`, `SkyService`, `DecalService`, `HLODService`, `RenderingService`, `ValidationService`, `SourceControlService`, `RenderQueueService`, `ModelingService`, `InputService` — 32 headers + 32 cpps.

(Correction: catalog lists 16 "Phase 2 new" services plus 16 "extra Phase 2" services = 32 new services total. Verify count against spec §Tool Catalog before starting.)

- Modify: `Private/MCPRequestRouter.cpp` to register each new service.
- Modify: `SpecialAgent.Build.cs` to add minimum module dependencies so headers compile.

- [ ] **Step 1: Generate a stub template and apply per-service**

Template `.h`:

```cpp
#pragma once
#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * <ServiceName> Service
 * See spec §Tool Catalog for the full tool list.
 */
class SPECIALAGENT_API F<ServiceName>Service : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;
};
```

Template `.cpp`:

```cpp
#include "Services/<ServiceName>Service.h"

FString F<ServiceName>Service::GetServiceDescription() const
{
    return TEXT("<one-line description>");
}

FMCPResponse F<ServiceName>Service::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("<prefix>"), MethodName);
}

TArray<FMCPToolInfo> F<ServiceName>Service::GetAvailableTools() const
{
    // TODO: populated in Phase 1
    return {};
}
```

- [ ] **Step 2: Register each service in `FMCPRequestRouter::FMCPRequestRouter()`**

Example pattern matching the existing code:

```cpp
RegisterService(TEXT("blueprint"),      MakeShared<FBlueprintService>());
RegisterService(TEXT("material"),       MakeShared<FMaterialService>());
RegisterService(TEXT("asset_import"),   MakeShared<FAssetImportService>());
... etc for every new service.
```

- [ ] **Step 3: Add required modules to `SpecialAgent.Build.cs`**

Append to `PrivateDependencyModuleNames`:

```
"AssetTools",
"InterchangeCore",
"InterchangeEngine",
"Blutility",
"KismetCompiler",
"BlueprintGraph",
"UMG",
"UMGEditor",
"Niagara",
"NiagaraEditor",
"MovieScene",
"MovieSceneTracks",
"MovieSceneTools",
"LevelSequence",
"LevelSequenceEditor",
"WorldPartitionEditor",
"PCG",
"PCGEditor",
"SourceControl",
"MovieRenderPipelineCore",
"MovieRenderPipelineEditor",
"EnhancedInput",
"GeometryFramework",
"GeometryScriptingCore",
"MeshModelingTools",
"SlateCore",
```

(Some of these may not be needed and will be pruned during Phase 1 as each agent verifies. Bias toward including; Phase 1 agent for that service removes unused deps.)

- [ ] **Step 4: Full build; expect clean compile, 32 empty-tools warnings at startup**

The startup log will now show 7 old + 32 new = 39 services with empty tools. This is expected and will be eliminated progressively by Phase 1.

- [ ] **Step 5: Commit**

```bash
git add Source/SpecialAgent/Public/Services/*.h \
        Source/SpecialAgent/Private/Services/*.cpp \
        Source/SpecialAgent/Private/MCPRequestRouter.cpp \
        Source/SpecialAgent/SpecialAgent.Build.cs
git commit -m "scaffold(SpecialAgent): stub all new services + register + add build deps"
```

---

## Phase 1 — Service Implementation (Parallel, 12 agents)

**Execution model:** Each Phase 1 task brief is written so one agent can own it end-to-end. Every agent follows the same per-service procedure:

1. Read the spec's Tool Catalog entry for each assigned service.
2. For each tool in order:
   - Define the tool in `GetAvailableTools()` via `FMCPToolBuilder`.
   - Add the method dispatch branch in `HandleRequest()`.
   - Implement the handler, using `FMCPGameThreadProcessor::Get().Enqueue(...).Get()` for any work touching editor state.
   - Use `FMCPJson::MakeSuccess()` / `MakeError()` / `WriteActor()` / `ReadVec3()` helpers.
   - Log on success and on error with `SpecialAgent:` prefix.
3. After the service is complete:
   - Build the editor.
   - Verify startup no longer warns for this service.
   - Smoke-test one handler via curl.
4. Commit as one commit per service.

**Description standard (applied per tool):**

```
<verb> <object>. <effect, one sentence>.
Params: <param> (type, unit/range, role).
Workflow: <cross-reference>
Warning: <caveat, if any>
```

**Reference implementations already present:** `WorldService::HandleListActors` (list pattern), `WorldService::HandleSpawnActor` (spawn pattern), `UtilityService::HandleSelectAtScreen` (screen-to-world pattern), `ViewportService::HandleTraceFromScreen` (trace pattern), `AssetService::HandleGetAssetBounds` (asset query pattern). Copy their structure.

---

### Task 1.A: Core Actor — World stub-fixes + Utility expansion + Viewport expansion

**Scope:** ~37 tools.
- `world`: implement the 20 currently-stub `not_implemented` handlers + add 5 new (`set_actor_tick_enabled`, `set_actor_hidden`, `set_actor_collision`, `attach_to`, `detach`).
- `utility`: add 11 new tools (`focus_asset_in_browser`, `deselect_all`, `invert_selection`, `select_by_class`, `group_selected`, `ungroup`, `begin_transaction`, `end_transaction`, `show_notification`, `show_dialog`, `focus_tab`).
- `viewport`: add 8 new tools (`orbit_around_actor`, `set_fov`, `set_view_mode`, `toggle_game_view`, `bookmark_save`, `bookmark_restore`, `set_grid_snap`, `toggle_realtime`).

**Files:**
- Modify: `Private/Services/WorldService.cpp` (implement stubs, add new tools to `GetAvailableTools()` + `HandleRequest()`)
- Modify: `Public/Services/WorldService.h` (add handler declarations for new tools)
- Modify: `Private/Services/UtilityService.cpp`, `Public/Services/UtilityService.h`
- Modify: `Private/Services/ViewportService.cpp`, `Public/Services/ViewportService.h`

- [ ] **Step 1: World — stubs**

Implement each of the 20 `not_implemented` stubs. Key UE5 APIs to use:
- `find_actors_by_tag` — `TActorIterator` + `Actor->Tags.Contains`.
- `spawn_actors_batch` / `delete_actors_batch` — loop + collect results; each sub-spawn uses the same path as `HandleSpawnActor`.
- `set_actor_transform` — `Actor->SetActorTransform(FTransform(Rot, Loc, Scale))`.
- `set_actor_property` — `FProperty` reflection via `FindFProperty<FProperty>(Actor->GetClass(), PropName)` + `ImportText_Direct`.
- `set_actor_label` — `Actor->SetActorLabel`.
- `set_actor_material` — loop static mesh components, `SetMaterial`.
- `set_material_parameter` — get material from component, `SetScalarParameterValueEditorOnly` / `SetVectorParameterValueEditorOnly` on `UMaterialInstanceConstant`.
- `create_folder` / `move_actor_to_folder` — `Actor->SetFolderPath(FName(*Path))`.
- `add_actor_tag` / `remove_actor_tag` — `Actor->Tags.Add/Remove`.
- `measure_distance` — `FVector::Dist(A,B)`.
- `find_actors_in_bounds` — `TActorIterator` + `FBox::IsInside`.
- `raycast` — `World->LineTraceSingleByChannel(..., ECC_Visibility, ...)`.
- `get_ground_height` — downward trace from high Z, return hit Z.
- `place_in_grid` / `place_in_circle` / `place_along_spline` / `scatter_in_area` — compute positions, call `HandleSpawnActor` logic internally, return array of spawned actor names.
- New 5: `set_actor_tick_enabled` (`Actor->SetActorTickEnabled`), `set_actor_hidden` (`Actor->SetActorHiddenInGame` + `SetIsTemporarilyHiddenInEditor`), `set_actor_collision` (`Actor->SetActorEnableCollision`), `attach_to` (`Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform)`), `detach` (`Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform)`).

For each: add `FMCPToolBuilder` entry in `GetAvailableTools()`, add `MethodName ==` branch, implement on game thread via processor.

- [ ] **Step 2: Utility — new tools**

- `focus_asset_in_browser` — `IContentBrowserSingleton::Get().SyncBrowserToAssets`.
- `deselect_all` — `GEditor->SelectNone(true,true,false)`.
- `invert_selection` — enumerate level actors, toggle selection.
- `select_by_class` — `TActorIterator` + select matching.
- `group_selected` / `ungroup` — `AGroupActor::AddSelectedActorsToSelectedGroup()` / `DetachSelectedGroup()`.
- `begin_transaction` / `end_transaction` — `GEditor->BeginTransaction(FText::FromString(Name))` / `GEditor->EndTransaction()`.
- `show_notification` — `FNotificationInfo Info(FText::FromString(Msg)); FSlateNotificationManager::Get().AddNotification(Info)`.
- `show_dialog` — `FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Msg))`.
- `focus_tab` — `FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT(<name>)))`.

- [ ] **Step 3: Viewport — new tools**

- `orbit_around_actor` — compute camera position at distance around actor bounds center.
- `set_fov` — `FLevelEditorViewportClient::ViewFOV = FOV`.
- `set_view_mode` — `Viewport->EngineShowFlags.SetX(true/false)` or `Viewport->SetViewMode(EViewModeIndex)`.
- `toggle_game_view` — `Viewport->SetGameView(!Viewport->IsInGameView())`.
- `bookmark_save` / `bookmark_restore` — `GEngine->GetLevelViewportClient` then `LevelEditorViewportClient::Bookmark` APIs.
- `set_grid_snap` — `GEditor->GetDerivedPositionSnapping` / `SetGridSize`.
- `toggle_realtime` — `Viewport->SetRealtime(!IsRealtime())`.

- [ ] **Step 4: Build, smoke-test each service, commit per service (3 commits)**

```bash
git commit -m "feat(SpecialAgent/world): implement 25 actor/spatial/pattern tools"
git commit -m "feat(SpecialAgent/utility): add 11 editor-UI + transaction tools"
git commit -m "feat(SpecialAgent/viewport): add 8 camera + view-mode tools"
```

---

### Task 1.B: Level Design — Lighting + Foliage + Landscape

**Scope:** 17 tools.

**Files:**
- Fully rewrite `Private/Services/LightingService.cpp`, `FoliageService.cpp`, `LandscapeService.cpp` (they currently only forward to Python; replace with direct C++).
- Update corresponding `.h` with new handler declarations.

**Lighting (6):**
- `spawn_light` — spawn `APointLight`, `ASpotLight`, `ADirectionalLight`, `ARectLight`, or `ASkyLight` based on `light_type` enum. Set intensity, color, rotation, location.
- `set_light_intensity` — find actor by label; `ULightComponent::SetIntensity`.
- `set_light_color` — `ULightComponent::SetLightColor`.
- `set_light_attenuation` — `UPointLightComponent::SetAttenuationRadius` / `USpotLightComponent` cone angles.
- `set_light_cast_shadows` — `SetCastShadows(bool)`.
- `build_lighting` — `GEditor->Exec(World, TEXT("BuildLighting"))` or `FEditorBuildUtils::EditorBuild(World, NAME_LightingBuild)`.

**Foliage (5):**
- `paint_in_area` — via `AInstancedFoliageActor::AddInstance` within a bounding region; use `UFoliageType` asset.
- `remove_from_area` — iterate foliage components in bounds, remove instances.
- `get_density` — count instances per unit area.
- `list_foliage_types` — enumerate `UFoliageType` assets in the level.
- `add_foliage_type` — load foliage type asset, add to `AInstancedFoliageActor`.

**Landscape (6):**
- `get_info` — find `ALandscape`, return dimensions, components, material.
- `sculpt_height` — use `FLandscapeEditDataInterface::GetHeightData` / `SetHeightData`.
- `flatten_area` — set all heights in a rect to target.
- `smooth_area` — convolve heights with a 3x3 kernel.
- `paint_layer` — `FLandscapeEditDataInterface::SetAlphaData` on a named layer.
- `list_layers` — enumerate `ALandscape::LandscapeLayerInfoObjects`.

- [ ] **Step 1: Lighting service — rewrite**
- [ ] **Step 2: Foliage service — rewrite**
- [ ] **Step 3: Landscape service — rewrite**
- [ ] **Step 4: Build + smoke test each; commit 3 commits.**

---

### Task 1.C: Structure / Streaming — Streaming + Navigation + WorldPartition

**Scope:** 14 tools.

- `streaming` (5): `list_levels` (`UWorld::StreamingLevels`), `load_level` (`ULevelStreamingDynamic::LoadLevelInstance`), `unload_level` (`UGameplayStatics::UnloadStreamLevel`), `set_level_visibility`, `set_level_streaming_volume`.
- `navigation` (4): `rebuild_navmesh` (`UNavigationSystemV1::Build`), `test_path` (`FindPathToLocationSynchronously`), `get_navmesh_bounds`, `find_nearest_reachable_point` (`ProjectPointToNavigation`).
- `world_partition` (5): `list_cells`, `load_cell`, `unload_cell`, `get_loaded_cells`, `force_load_region` — via `UWorldPartition::LoadCells` / `UWorldPartition::GetCells`.

- [ ] **Step 1:** streaming
- [ ] **Step 2:** navigation
- [ ] **Step 3:** world_partition (add `WorldPartitionEditor` dep if not yet)
- [ ] **Step 4:** 3 commits.

---

### Task 1.D: Gameplay / Performance / Assets

**Scope:** 21 tools.

- `gameplay` (6): spawn `ATriggerVolume`, `APlayerStart`, `ANote`, `ATargetPoint`, `AKillZVolume` (inherit from `AVolume`), `ABlockingVolume`. Use `World->SpawnActor`.
- `performance` (5): `get_statistics` (`UEngine::GameStatMap`, or `GEngine->GetMemoryFootprint` + UE stat system), `get_actor_bounds` (`Actor->GetComponentsBoundingBox(true)`), `check_overlaps` (`World->OverlapMulti`), `get_triangle_count` (iterate static meshes, sum LOD0 triangle count), `get_draw_call_estimate`.
- `assets` (10): `sync_to_browser` (`IContentBrowserSingleton::SyncBrowserToAssets`), `create_folder` (`IAssetTools::Get().CreateUniqueAssetName`), `rename` (`IAssetTools::RenameAssets`), `delete` (`ObjectTools::DeleteAssets`), `move` (`IAssetTools::AdvancedCopyPackages` or move), `duplicate` (`IAssetTools::DuplicateAsset`), `save` (`UEditorAssetLibrary::SaveAsset`), `set_metadata` / `get_metadata` (`UMetaData::SetValue` / `GetValue`), `validate` (`UEditorValidatorSubsystem::ValidateObjects`).

- [ ] **Step 1-4:** 3 services, 3 commits.

---

### Task 1.E: Blueprint + Reflection

**Scope:** 15 tools.

- `blueprint` (10): `create` (`FKismetEditorUtilities::CreateBlueprint`), `compile` (`FKismetEditorUtilities::CompileBlueprint`), `add_variable` (`FBlueprintEditorUtils::AddMemberVariable`), `add_function` (`FBlueprintEditorUtils::AddFunctionGraph`), `set_default_value` (CDO field set via reflection), `list_functions` (iterate `UBlueprint->FunctionGraphs`), `list_variables` (iterate `UBlueprint->NewVariables`), `open_in_editor` (`GEditor->EditObject(BP)` or `FAssetEditorManager::OpenEditorForAsset`), `duplicate` (`IAssetTools::DuplicateAsset`), `reparent` (`FBlueprintEditorUtils::ReparentBlueprint`).
- `reflection` (5): `list_classes` (iterate `UClass` subclasses, filter by prefix), `get_class_info` (describe class name, super, package, UProperties, UFunctions), `list_properties` (iterate FProperty), `list_functions` (iterate UFunctions with signature strings), `call_function` — **scope restricted to primitive-only args per spec reviewer's recommendation** — accept function name + array of string args, marshal via `FProperty::ImportText_Direct`.

- [ ] 2 commits.

---

### Task 1.F: Materials / Visuals — Material + PostProcess + Sky + Decal

**Scope:** 22 tools.

- `material` (8): `create` (new `UMaterial`), `create_instance` (`UMaterialInstanceConstant` from parent), `set_scalar_parameter` (`UMaterialInstanceConstant::SetScalarParameterValueEditorOnly`), `set_vector_parameter`, `set_texture_parameter`, `set_static_switch`, `list_parameters` (`GetAllScalarParameterInfo`), `get_parameters`.
- `post_process` (6): `spawn_volume` (`APostProcessVolume`), `set_exposure` / `set_bloom` / `set_dof` / `set_color_grading` / `set_gi` via `FPostProcessSettings` fields.
- `sky` (5): `spawn_sky_atmosphere` (`ASkyAtmosphere`), `spawn_height_fog` (`AExponentialHeightFog`), `spawn_cloud` (`AVolumetricCloud`), `spawn_sky_light` (`ASkyLight`), `set_sun_angle` (rotate existing directional light).
- `decal` (3): `spawn` (`ADecalActor`), `set_material` (`DecalComponent->SetDecalMaterial`), `set_size` (`DecalSize`).

- [ ] 4 commits.

---

### Task 1.G: Asset Pipeline — AssetImport + ContentBrowser + AssetDeps + DataTable + Validation

**Scope:** 29 tools.

- `asset_import` (6): `import_fbx`, `import_texture`, `import_sound`, `import_folder` — each constructs `UAssetImportTask` + `IAssetTools::Get().ImportAssetTasks`. **This is exactly the code path that triggered the Phase 0 crash; verify it runs cleanly post-fix.** `create_data_table_from_csv` (`UDataTableFactory` + `UDataTable::CreateTableFromCSVString`), `get_import_settings_template`.
- `content_browser` (9): `sync_to_folder`, `create_folder` (`IAssetTools::CreateUniqueAssetName` + `FFileHelper::SaveStringToFile`), `rename`, `delete`, `move`, `duplicate`, `save`, `set_metadata`, `get_metadata`.
- `asset_deps` (4): `get_references` / `get_referencers` (`IAssetRegistry::GetDependencies` / `GetReferencers`), `find_unused` (filter assets whose referencers are empty), `get_dependency_graph`.
- `data_table` (7): `list_tables`, `get_row`, `set_row`, `add_row`, `delete_row`, `list_rows`, `get_row_struct` — via `UDataTable::GetRowMap`.
- `validation` (3): `validate_selected`, `validate_level`, `list_errors` — `UEditorValidatorSubsystem`.

- [ ] 5 commits.

---

### Task 1.H: Editor Control — PIE + Console + Log + Level + EditorMode + Project

**Scope:** 32 tools.

- `pie` (8): `start` (`FLevelEditorModule::PlayInEditor`), `stop` (`GEditor->RequestEndPlayMap`), `pause`, `resume`, `step_frame`, `toggle_simulate`, `is_playing` (`GEditor->PlayWorld != nullptr`), `get_world_context`.
- `console` (4): `execute` (`GEngine->Exec(GetWorld(), *Cmd)`), `list_commands` (return curated list), `set_cvar` / `get_cvar` (`IConsoleManager::Get().FindConsoleVariable`).
- `log` (4): `tail` (capture via `FOutputDeviceRedirector`), `clear`, `list_categories`, `set_category_verbosity`. Tail implementation: register a custom `FOutputDevice` that buffers the last N messages in a ring buffer (thread-safe).
- `level` (5): `open` (`UEditorLoadingAndSavingUtils::LoadMap`), `new` (`UEditorLevelUtils::CreateNewStreamingLevel`), `save_as` (`FEditorFileUtils::SaveLevelAs`), `get_current_path`, `list_templates`.
- `editor_mode` (3): `activate` (`GLevelEditorModeTools().ActivateMode(FEditorModeID)`), `get_current`, `configure_brush` (brush size/strength on landscape/foliage modes).
- `project` (8): `get_setting` / `set_setting` (`GConfig->GetString/SetString` on `GGameIni`), `get_version`, `list_plugins` (`IPluginManager::Get().GetEnabledPlugins`), `enable_plugin` / `disable_plugin` (`IPluginManager::Get().SetPluginEnabled`), `get_content_path`, `get_project_path`.

- [ ] 6 commits.

---

### Task 1.I: Components / Physics / Animation

**Scope:** 19 tools.

- `component` (7): `add` (`NewObject<UActorComponent>(Actor, Class)` + `RegisterComponent`), `remove`, `list`, `get_properties`, `set_property`, `attach` (`USceneComponent::AttachToComponent`), `detach`.
- `physics` (7): `set_simulate_physics` (`UPrimitiveComponent::SetSimulatePhysics`), `apply_impulse` / `apply_force`, `set_linear_velocity` / `set_angular_velocity`, `set_mass` (`SetMassOverrideInKg`), `set_collision_enabled` (`SetCollisionEnabled`).
- `animation` (5): `play` (`USkeletalMeshComponent::PlayAnimation`), `stop`, `set_anim_blueprint` (`SetAnimInstanceClass`), `list_animations` (asset query), `set_pose`.

- [ ] 3 commits.

---

### Task 1.J: AI / Input / Sound

**Scope:** 14 tools.

- `ai` (5): `spawn_ai_pawn`, `assign_controller` (`Pawn->SpawnDefaultController`), `run_behavior_tree` (`AAIController::RunBehaviorTree`), `set_blackboard_value`, `stop_ai`.
- `input` (4): `list_mappings` (`UInputSettings::GetActionMappings`), `add_action_mapping`, `add_axis_mapping`, `remove_mapping`.
- `sound` (4): `play_2d` (`UGameplayStatics::PlaySound2D`), `play_at_location` (`PlaySoundAtLocation`), `spawn_ambient_actor` (`AAmbientSound`), `set_volume_multiplier`.

- [ ] 3 commits.

---

### Task 1.K: Cinematics / Rendering — Sequencer + Niagara + RenderQueue + Rendering

**Scope:** 20 tools.

- `sequencer` (6): `create` (`ULevelSequenceFactoryNew`), `add_actor_binding`, `add_transform_track`, `add_keyframe`, `set_playback_range`, `play` (`ULevelSequencePlayer::CreateLevelSequencePlayer`).
- `niagara` (6): `spawn_emitter` (`UNiagaraFunctionLibrary::SpawnSystemAtLocation`), `set_parameter`, `activate`, `deactivate`, `set_user_float`, `set_user_vec3`.
- `render_queue` (3): `queue_sequence`, `set_output`, `get_status` — `UMoviePipelineQueueSubsystem`.
- `rendering` (5): `set_scalability` (`Scalability::ApplyCachedQualityLevels`), `set_view_mode`, `high_res_screenshot` (`FScreenshotRequest::RequestScreenshot`), `toggle_nanite`, `toggle_lumen` (cvars).

- [ ] 4 commits.

---

### Task 1.L: Finishing — PCG + Modeling + HLOD + SourceControl

**Scope:** 15 tools.

- `pcg` (3): `list_graphs`, `execute_graph`, `spawn_pcg_actor` (`APCGVolume`).
- `modeling` (4): `boolean_union`, `boolean_subtract`, `extrude`, `simplify` — use `UGeometryScriptLibrary_MeshBooleanFunctions` if available, else Modeling Mode tool subsystem.
- `hlod` (3): `build` (`UHierarchicalLODUtilities::BuildStaticMeshesForLODActors`), `clear`, `set_setting`.
- `source_control` (5): `get_status`, `check_out`, `revert`, `submit`, `list_modified` — via `ISourceControlModule::Get()`.

- [ ] 4 commits.

---

## Phase 2 — Integration (Sequential, 1 agent)

### Task 2.1: Full build and tools/list verification

**Files:**
- Modify: `Private/MCPRequestRouter.cpp` — verify all Phase 2 services are registered.

- [ ] **Step 1: Full clean build.** Expected: zero errors.
- [ ] **Step 2: Start editor with MCP server.** Run:

```bash
curl -X POST http://localhost:8767/message -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":"1","method":"tools/list"}'
```

Expected: JSON with at least 250 tools; every tool has a non-empty `description` and an `inputSchema.properties` object.

- [ ] **Step 3: Startup validator check.** The log should show `SpecialAgent: 30 services, >=250 tools total` and zero "exposes ZERO tools" warnings.

- [ ] **Step 4: Spot-check schema/handler consistency.** For 3 random services, POST each advertised tool name with empty args; the handler should return a clean `{success:false, error:"Missing params"}`, not crash.

- [ ] **Step 5: Commit** any small fixes found.

```bash
git commit -m "fix(SpecialAgent): integration fixes found during Phase 2 verification"
```

---

## Phase 3 — Polish (Parallel, 2 agents)

### Task 3.M: Description polish pass

**Files:** every `Private/Services/*.cpp`'s `GetAvailableTools()`.

- [ ] **Step 1: Enforce description standard**

For every tool, rewrite the description to follow:
```
<verb> <object>. <effect>.
Params: <param> (type, unit/range, role).
Workflow: <cross-reference>
Warning: <caveat>
```

Example — `world/spawn_actor` before:
> Spawn an actor at a location. IMPORTANT: Place ONE at a time, ... (okay)

Example — `world/set_actor_location` before:
> Move an actor to a new location.

After:
> Move an actor. Replaces location in world space. Params: actor_name (string), location ([X,Y,Z] cm world). Workflow: use viewport/trace_from_screen to find valid positions. Warning: does not preserve attachment offset.

- [ ] **Step 2: Commit**

```bash
git commit -m "polish(SpecialAgent): tool description review pass"
```

---

### Task 3.N: Prompts, instructions, README

**Files:**
- Modify: `Private/MCPRequestRouter.cpp`
- Modify: `Plugins/SpecialAgentPlugin/README.md`

- [ ] **Step 1: Expand `BuildSpecialAgentInstructions()`**

Rewrite to match the spec's prescribed format: preserve the current "screenshot→act→verify" workflow guidance, add a compact service map listing all 30 service prefixes and one-liner each. Keep the total under ~1500 tokens (this string ships with every initialize response).

- [ ] **Step 2: Add 12 prompts to `HandlePromptsGet`**

Add branches for: `build_scene`, `create_blueprint`, `import_assets`, `build_sequence`, `setup_lighting`, `populate_foliage`, `build_landscape`, `configure_postprocess`, `setup_navigation`, `wire_gameplay`, `debug_performance`, `run_pie_test`. Model each after the existing `explore_level` prompt. Update `HandlePromptsList` to return entries for all 16 prompts with description + arguments schema.

- [ ] **Step 3: Update README**

Replace the "71+ tools" claim and the service table with the current catalog:
- 30 services (list them)
- ~280 callable tools
- Add: "Every registered tool has a working handler — no `not_implemented` stubs."
- Add the Phase 0 crash fix to the What's New notes.

- [ ] **Step 4: Commit**

```bash
git commit -m "polish(SpecialAgent): expand instructions, add 12 prompts, update README"
```

---

## Phase 4 — Verification (Sequential, 1 agent)

### Task 4.1: Full smoke test

- [ ] **Step 1: Clean build on Mac.** Zero errors and zero new warnings beyond pre-existing baseline.

- [ ] **Step 2: Start editor, open a test project.** Verify `LogSpecialAgent: MCP Server started on port 8767` appears.

- [ ] **Step 3: Tools/list audit.** POST `tools/list`, assert:
  - Response tool count ≥ 250.
  - Every tool has non-empty description ≥ 40 characters.
  - Every tool has `inputSchema.type == "object"`.
  - No tool name contains spaces or uppercase.

- [ ] **Step 4: Per-service smoke test.** For each of 30 services, pick one safe read-only tool and POST it. Expected: 30/30 return `{success:true}`. Specific picks:
  - `world/get_level_info`, `assets/list`, `python/list_modules`, `viewport/get_transform`, `screenshot/capture`, `lighting/build_lighting` (dry-run safe?), `foliage/list_foliage_types`, `landscape/list_layers`, `streaming/list_levels`, `navigation/get_navmesh_bounds`, `gameplay/…` (pick one safe query), `performance/get_statistics`, `utility/get_selection`, `blueprint/list_functions`, `material/list_parameters`, `asset_import/get_import_settings_template`, `pie/is_playing`, `console/list_commands`, `component/list`, `editor_mode/get_current`, `level/get_current_path`, `log/list_categories`, `data_table/list_tables`, `asset_deps/get_references`, `sequencer/…`, `niagara/…`, `sound/…`, `world_partition/get_loaded_cells`, `pcg/list_graphs`, `content_browser/…`, `project/get_project_path`, `reflection/list_classes`, `physics/…`, `animation/list_animations`, `ai/…`, `post_process/…`, `sky/…`, `decal/…`, `hlod/…`, `rendering/…`, `validation/list_errors`, `source_control/get_status`, `render_queue/get_status`, `modeling/…`, `input/list_mappings`.

- [ ] **Step 5: Crash regression test.** Rerun the Phase 0 `asset_import/import_fbx` repro with a real FBX file. Expected: handler returns `{success:true}` or a sensible error; editor does not crash.

- [ ] **Step 6: Tag and commit the verification report.**

Create `docs/superpowers/plans/2026-04-19-ue5-mcp-tools-expansion-report.md` summarizing:
- tools/list count observed
- services with any failing smoke test
- known follow-ups (if any)

```bash
git add docs/superpowers/plans/2026-04-19-ue5-mcp-tools-expansion-report.md
git commit -m "chore(SpecialAgent): verification report for tool surface expansion"
git tag v0.2.0 -m "SpecialAgent 0.2.0 — 30 services, ~280 tools, crash-free"
```

---

## Risk Register (live during execution)

| Risk | If encountered |
|---|---|
| Phase 0 Task 0.1 fix is insufficient (some subsystem still recurses) | Add `FMCPGameThreadProcessor::RunNow()` that short-circuits when already inside a safe context; investigate the specific stack. Escalate to user. |
| A module in `Build.cs` doesn't exist in UE 5.6 or varies by platform | Phase 0 Task 0.6 removes it from `Build.cs` and marks the affected service as deferred. Reduce the service's tools for the release. |
| `reflection/call_function` opens arbitrary UFUNCTION invocation risk | Per spec reviewer note: v1 restricts args to primitive types (bool, int, float, string, FVector). Document in tool description. |
| `assets` vs `content_browser` scope overlap | Agent 1.D (assets) uses Asset Registry / UObject API only. Agent 1.G (content_browser) uses `IContentBrowserSingleton` UI sync only. Reviewer noted this explicitly. |
| `editor_mode/modeling` overlap | `editor_mode/activate("modeling")` is a mode switch. `modeling/boolean_union` is a mesh operation. They're distinct tools; coexist cleanly. |
| Scope of a Phase 1 agent exceeds single-session capacity | Agent commits per-service and checkpoints; remaining services in their group can be picked up by the same or a different agent in a follow-up. |

---

## Commit Hygiene

- One commit per service (or one commit per logical sub-step in Phase 0).
- Commit messages: `<type>(SpecialAgent/<service>): <one-sentence summary>` where `type` is one of `feat`, `fix`, `scaffold`, `polish`, `chore`.
- Never skip hooks. If the pre-commit fails, fix the root cause.
- Never amend a published commit.

---

## Sub-skill Directives

When an agent is dispatched to execute a Phase 1 task, its prompt MUST include:
1. Path to this plan + the specific section (e.g., "Task 1.D").
2. Path to the spec's Tool Catalog.
3. The reminder: "Every editor-touching handler must use `FMCPGameThreadProcessor::Get().Enqueue(...).Get()` — never raw `AsyncTask(GameThread)`."
4. The description standard.
5. The commit message format.
6. A forbidden list: no `not_implemented` stubs, no `ExecutePythonFromParams` in handlers, no direct game-thread execution from HTTP worker.
