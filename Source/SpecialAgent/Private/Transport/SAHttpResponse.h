#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include <atomic>

class FSocket;

class FSAHttpResponse
{
public:
    explicit FSAHttpResponse(FSocket* InSocket);

    /** Single-body mode: write status line + headers + body + close. */
    bool WriteSingleBody(int32 StatusCode,
                         const FString& ContentType,
                         TConstArrayView<uint8> Body,
                         const TMap<FString, FString>& ExtraHeaders = {});

    bool WriteSingleBodyString(int32 StatusCode,
                               const FString& ContentType,
                               const FString& Body,
                               const TMap<FString, FString>& ExtraHeaders = {});

    /** Chunked-SSE mode. Call BeginSSE once, then WriteEvent/WriteKeepAlive
     *  as often as needed, then Finish. All three are thread-safe: they
     *  acquire SSEWriteLock internally. */
    bool BeginSSE(const TMap<FString, FString>& ExtraHeaders = {});
    bool WriteEvent(const FString& EventName, const FString& Data);
    bool WriteKeepAlive();
    bool Finish();

    bool IsDead() const { return bDead.load(std::memory_order_acquire); }

private:
    FSocket* Socket;
    FCriticalSection SSEWriteLock;
    std::atomic<bool> bDead { false };

    bool WriteAll(const FString& Utf8Payload);
    bool WriteAllRaw(TConstArrayView<uint8> Payload);
    static FString StatusText(int32 Code);
};
