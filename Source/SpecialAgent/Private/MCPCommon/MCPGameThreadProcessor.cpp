#include "MCPCommon/MCPGameThreadProcessor.h"

FMCPGameThreadProcessor& FMCPGameThreadProcessor::Get()
{
    static FMCPGameThreadProcessor Instance;
    return Instance;
}

void FMCPGameThreadProcessor::Shutdown()
{
    bShuttingDown.store(true, std::memory_order_release);

    // Drain any pending work so in-flight futures complete. Do this on
    // whatever thread called us — if it's the game thread, the drain is
    // outside ProcessTasksUntilIdle because module shutdown happens from
    // editor subsystem lifecycle, not from task-graph pump.
    FWorkItem Item;
    while (Pending.Dequeue(Item))
    {
        Item.Run();
    }
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
