#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Dom/JsonObject.h"

class FSAConnection;

class FSASessionRegistry
{
public:
    static FSASessionRegistry& Get();

    FString MintSession();

    /** Register the GET /sse connection for a session. If one is already
     *  registered, the previous gets a ": session_replaced\n\n" comment and
     *  is then dropped (last-writer-wins). */
    void RegisterStream(const FString& SessionId, FSAConnection* Conn);
    void UnregisterStream(const FString& SessionId, FSAConnection* Conn);

    bool IsSessionValid(const FString& SessionId) const;

    /** Push a notification to the session's SSE stream. Returns false if the
     *  session has no registered stream or the write failed. */
    bool SendNotification(const FString& SessionId, const TSharedRef<FJsonObject>& Notification);

private:
    FSASessionRegistry() = default;

    mutable FRWLock Lock;
    TSet<FString> ActiveSessions;
    TMap<FString, FSAConnection*> Streams;
};
