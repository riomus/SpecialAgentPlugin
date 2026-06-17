#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCPCommon/MCPGameThreadProcessor.h"

#include <type_traits>

class SPECIALAGENT_API FGameThreadDispatcher
{
public:
    /**
     * Default upper bound (seconds) on how long a worker thread will block
     * waiting for a game-thread task. The task is only run inside the editor
     * Tick(); if the game thread stalls (editor backgrounded + CPU throttle,
     * a modal dialog, a long blocking shader compile / asset import, or PIE
     * transitions) an unbounded wait would wedge the worker — and its HTTP
     * request — forever. After this bound we stop waiting and return a failure
     * value; the enqueued task still runs when the game thread resumes, it is
     * simply no longer awaited. Generous so legitimate long single operations
     * still complete in-band; only genuine wedges hit the bound.
     */
    static constexpr double DefaultSyncTimeoutSeconds = 120.0;

    template<typename ReturnType>
    static ReturnType DispatchToGameThreadSyncWithReturn(TFunction<ReturnType()> Task,
                                                         double TimeoutSeconds = DefaultSyncTimeoutSeconds)
    {
        checkf(!IsInGameThread(),
            TEXT("FGameThreadDispatcher called from game thread — would self-deadlock."));

        TFuture<ReturnType> Future =
            FMCPGameThreadProcessor::Get().Enqueue<ReturnType>(MoveTemp(Task));

        if (WaitBounded(Future, TimeoutSeconds))
        {
            return Future.Get();
        }

        UE_LOG(LogTemp, Error,
            TEXT("SpecialAgent: game-thread task did not complete within %.0fs — returning failure. "
                 "The editor may be backgrounded/CPU-throttled, a modal dialog may be open, or a blocking "
                 "compile/import/PIE transition is in progress."),
            TimeoutSeconds);

        return MakeTimeoutResult<ReturnType>();
    }

    static void DispatchToGameThreadSync(TFunction<void()> Task,
                                         double TimeoutSeconds = DefaultSyncTimeoutSeconds)
    {
        checkf(!IsInGameThread(), TEXT("FGameThreadDispatcher called from game thread"));
        auto Wrapped = [Task = MoveTemp(Task)]() -> int { Task(); return 0; };
        TFuture<int> Future = FMCPGameThreadProcessor::Get().Enqueue<int>(MoveTemp(Wrapped));
        if (!WaitBounded(Future, TimeoutSeconds))
        {
            UE_LOG(LogTemp, Error,
                TEXT("SpecialAgent: game-thread (void) task did not complete within %.0fs — abandoning wait."),
                TimeoutSeconds);
        }
    }

    template<typename ReturnType>
    static TFuture<ReturnType> DispatchToGameThread(TFunction<ReturnType()> Task)
    {
        return FMCPGameThreadProcessor::Get().Enqueue<ReturnType>(MoveTemp(Task));
    }

private:
    /**
     * Wait on Future for up to TimeoutSeconds, polling in short slices so we
     * notice editor shutdown promptly. Returns true if the future completed.
     */
    template<typename ReturnType>
    static bool WaitBounded(TFuture<ReturnType>& Future, double TimeoutSeconds)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        for (;;)
        {
            if (Future.WaitFor(FTimespan::FromMilliseconds(250)))
            {
                return true;
            }
            if (FMCPGameThreadProcessor::Get().IsShuttingDown())
            {
                // Drain in progress; the task either ran or will fast-fail.
                return Future.WaitFor(FTimespan::FromMilliseconds(250));
            }
            if (FPlatformTime::Seconds() >= Deadline)
            {
                return false;
            }
        }
    }

    /**
     * Failure value returned on timeout. For the overwhelmingly common JSON
     * handler return type we hand back a structured {success:false,error:...}
     * object so callers wrap it as a normal MCP error instead of dereferencing
     * a null pointer; everything else falls back to a default-constructed value.
     */
    template<typename ReturnType>
    static ReturnType MakeTimeoutResult()
    {
        if constexpr (std::is_same_v<ReturnType, TSharedPtr<FJsonObject>>)
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetBoolField(TEXT("success"), false);
            Err->SetStringField(TEXT("error"),
                TEXT("Game thread did not respond within the timeout "
                     "(editor backgrounded/throttled, a modal dialog is open, "
                     "or a long blocking operation is in progress)."));
            return Err;
        }
        else
        {
            return ReturnType{};
        }
    }
};
