#pragma once

#include "CoreMinimal.h"
#include "TickableEditorObject.h"
#include "Containers/Queue.h"
#include "Async/Future.h"
#include "HAL/CriticalSection.h"

/**
 * FMCPGameThreadProcessor
 *
 * Drains enqueued work during editor Tick(), OUTSIDE the task-graph
 * ProcessTasksUntilIdle pump. Fixes the recursion-guard crash that
 * occurs when editor subsystems (asset import, PIE, blueprint compile,
 * navmesh rebuild, etc.) call WaitUntilTasksComplete from inside an
 * AsyncTask(GameThread) lambda.
 *
 * Usage (from a worker thread, e.g., the HTTP server thread):
 *     TFuture<int> F = FMCPGameThreadProcessor::Get().Enqueue<int>([]{ return 42; });
 *     int Result = F.Get();
 *
 * Do NOT call Enqueue from the game thread — it would self-deadlock,
 * since Enqueue waits on Tick() to run the task.
 */
class SPECIALAGENT_API FMCPGameThreadProcessor : public FTickableEditorObject
{
public:
    static FMCPGameThreadProcessor& Get();

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

        Pending.Enqueue(MoveTemp(Item));
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

    TQueue<FWorkItem, EQueueMode::Mpsc> Pending;

    static constexpr int32 MaxItemsPerTick = 64;
};
