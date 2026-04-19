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
