#include "Transport/SASessionRegistry.h"
#include "Transport/SAConnection.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FSASessionRegistry& FSASessionRegistry::Get()
{
    static FSASessionRegistry I;
    return I;
}

FString FSASessionRegistry::MintSession()
{
    const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FRWScopeLock W(Lock, SLT_Write);
    ActiveSessions.Add(Id);
    return Id;
}

bool FSASessionRegistry::IsSessionValid(const FString& SessionId) const
{
    FRWScopeLock R(Lock, SLT_ReadOnly);
    return ActiveSessions.Contains(SessionId);
}

void FSASessionRegistry::RegisterStream(const FString& SessionId, FSAConnection* Conn)
{
    FSAConnection* Replaced = nullptr;
    {
        FRWScopeLock W(Lock, SLT_Write);
        if (FSAConnection** Existing = Streams.Find(SessionId))
        {
            Replaced = *Existing;
        }
        Streams.Add(SessionId, Conn);
    }
    if (Replaced) Replaced->OnReplacedBySessionRegistry();
}

void FSASessionRegistry::UnregisterStream(const FString& SessionId, FSAConnection* Conn)
{
    FRWScopeLock W(Lock, SLT_Write);
    if (FSAConnection** Existing = Streams.Find(SessionId))
    {
        if (*Existing == Conn) Streams.Remove(SessionId);
    }
}

bool FSASessionRegistry::SendNotification(const FString& SessionId,
                                          const TSharedRef<FJsonObject>& Notification)
{
    FSAConnection* Target = nullptr;
    {
        FRWScopeLock R(Lock, SLT_ReadOnly);
        if (FSAConnection** P = Streams.Find(SessionId)) Target = *P;
    }
    if (!Target) return false;

    FString Payload;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Payload);
    if (!FJsonSerializer::Serialize(Notification, W)) return false;

    return Target->PushSSEEvent(TEXT("message"), Payload);
}
