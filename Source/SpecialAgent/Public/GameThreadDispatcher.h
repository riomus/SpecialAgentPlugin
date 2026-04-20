#pragma once
#include "CoreMinimal.h"
#include "MCPCommon/MCPGameThreadProcessor.h"

class SPECIALAGENT_API FGameThreadDispatcher
{
public:
    template<typename ReturnType>
    static ReturnType DispatchToGameThreadSyncWithReturn(TFunction<ReturnType()> Task)
    {
        checkf(!IsInGameThread(),
            TEXT("FGameThreadDispatcher called from game thread — would self-deadlock."));
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
