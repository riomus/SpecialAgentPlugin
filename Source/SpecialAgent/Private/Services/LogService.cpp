// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/LogService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HAL/CriticalSection.h"
#include "Logging/LogCategory.h"
#include "Logging/LogMacros.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/StringOutputDevice.h"

namespace SpecialAgentLogRing
{
    struct FEntry
    {
        FDateTime Timestamp;
        FString Category;
        FString Verbosity;
        FString Message;
    };

    /**
     * Thread-safe ring buffer backed FOutputDevice. Registered with
     * GLog on first access and retained for the module lifetime.
     */
    class FRingDevice : public FOutputDevice
    {
    public:
        FRingDevice()
        {
            Entries.Reserve(Capacity);
        }

        virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            if (V == nullptr)
            {
                return;
            }
            FEntry Entry;
            Entry.Timestamp = FDateTime::UtcNow();
            Entry.Category  = Category.ToString();
            Entry.Verbosity = ::ToString(Verbosity);
            Entry.Message   = V;

            FScopeLock Lock(&CS);
            if (Entries.Num() < Capacity)
            {
                Entries.Add(MoveTemp(Entry));
            }
            else
            {
                Entries[Head] = MoveTemp(Entry);
                Head = (Head + 1) % Capacity;
            }
        }

        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
        virtual bool CanBeUsedOnAnyThread() const override { return true; }

        void Clear()
        {
            FScopeLock Lock(&CS);
            Entries.Reset();
            Head = 0;
        }

        // Returns entries in chronological order (oldest first), limited to MaxEntries.
        TArray<FEntry> Snapshot(int32 MaxEntries) const
        {
            FScopeLock Lock(&CS);
            TArray<FEntry> Ordered;
            if (Entries.Num() == 0)
            {
                return Ordered;
            }

            Ordered.Reserve(Entries.Num());
            if (Entries.Num() < Capacity)
            {
                // Not yet wrapped; Entries is already in order.
                Ordered.Append(Entries);
            }
            else
            {
                for (int32 i = 0; i < Capacity; ++i)
                {
                    Ordered.Add(Entries[(Head + i) % Capacity]);
                }
            }

            if (MaxEntries > 0 && Ordered.Num() > MaxEntries)
            {
                const int32 Start = Ordered.Num() - MaxEntries;
                TArray<FEntry> Tail;
                Tail.Append(Ordered.GetData() + Start, MaxEntries);
                return Tail;
            }
            return Ordered;
        }

    private:
        static constexpr int32 Capacity = 500;

        mutable FCriticalSection CS;
        TArray<FEntry> Entries;
        int32 Head = 0;
    };

    static FRingDevice& Get()
    {
        static FRingDevice* Device = []() -> FRingDevice*
        {
            FRingDevice* D = new FRingDevice();
            if (GLog)
            {
                GLog->AddOutputDevice(D);
            }
            return D;
        }();
        return *Device;
    }
}

FString FLogService::GetServiceDescription() const
{
    return TEXT("Tail / clear the editor log and configure log categories");
}

TArray<FMCPToolInfo> FLogService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("tail"),
        TEXT("Return the last N log messages from the in-memory ring buffer. "
             "Params: count (integer, 1..500, default 100). "
             "Workflow: call after operations to capture diagnostics; pair with log/clear to reset. "
             "Warning: buffer holds up to 500 entries and starts when the plugin loads."))
        .OptionalInteger(TEXT("count"), TEXT("Max entries to return (1..500, default 100)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("clear"),
        TEXT("Clear the in-memory log ring buffer. "
             "Workflow: call before a sequence you want to isolate, then log/tail to read it."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_categories"),
        TEXT("List a curated set of common log categories. "
             "Workflow: use before log/set_category_verbosity."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_category_verbosity"),
        TEXT("Set the verbosity level of a log category. "
             "Params: category (string, required); verbosity (enum: NoLogging, Fatal, Error, Warning, Display, Log, Verbose, VeryVerbose). "
             "Warning: unknown categories return an error."))
        .RequiredString(TEXT("category"), TEXT("Log category name (e.g. 'LogTemp')."))
        .RequiredEnum(TEXT("verbosity"),
            {TEXT("NoLogging"), TEXT("Fatal"), TEXT("Error"), TEXT("Warning"),
             TEXT("Display"), TEXT("Log"), TEXT("Verbose"), TEXT("VeryVerbose")},
            TEXT("Verbosity level."))
        .Build());

    return Tools;
}

FMCPResponse FLogService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    // Ensure the ring device is installed.
    SpecialAgentLogRing::Get();

    if (MethodName == TEXT("tail"))
    {
        int32 Count = 100;
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadInteger(Request.Params, TEXT("count"), Count);
        }
        Count = FMath::Clamp(Count, 1, 500);

        const TArray<SpecialAgentLogRing::FEntry> Entries = SpecialAgentLogRing::Get().Snapshot(Count);

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Entries.Num());
        for (const SpecialAgentLogRing::FEntry& E : Entries)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("timestamp"), E.Timestamp.ToIso8601());
            Obj->SetStringField(TEXT("category"),  E.Category);
            Obj->SetStringField(TEXT("verbosity"), E.Verbosity);
            Obj->SetStringField(TEXT("message"),   E.Message);
            Arr.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Out->SetArrayField(TEXT("entries"), Arr);
        Out->SetNumberField(TEXT("count"), Arr.Num());
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("clear"))
    {
        SpecialAgentLogRing::Get().Clear();
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: log ring cleared"));
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("list_categories"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        TArray<TSharedPtr<FJsonValue>> Arr;
        const TCHAR* Curated[] = {
            TEXT("LogTemp"), TEXT("LogCore"), TEXT("LogEngine"), TEXT("LogWorld"),
            TEXT("LogEditor"), TEXT("LogSlate"), TEXT("LogRHI"), TEXT("LogRenderer"),
            TEXT("LogLoad"), TEXT("LogSavePackage"), TEXT("LogPlayLevel"), TEXT("LogAnimation"),
            TEXT("LogAudio"), TEXT("LogPhysics"), TEXT("LogNavigation"), TEXT("LogAI"),
            TEXT("LogBlueprint"), TEXT("LogHttp"), TEXT("LogOnline"), TEXT("LogPython"),
            TEXT("LogAssetRegistry"), TEXT("LogClass"), TEXT("LogActor")
        };
        for (const TCHAR* Name : Curated)
        {
            Arr.Add(MakeShared<FJsonValueString>(Name));
        }
        Out->SetArrayField(TEXT("categories"), Arr);
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("set_category_verbosity"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString CategoryName, VerbosityName;
        if (!FMCPJson::ReadString(Request.Params, TEXT("category"), CategoryName) || CategoryName.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'category'"));
        }
        if (!FMCPJson::ReadString(Request.Params, TEXT("verbosity"), VerbosityName) || VerbosityName.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'verbosity'"));
        }

        ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
        if      (VerbosityName == TEXT("NoLogging"))   Verbosity = ELogVerbosity::NoLogging;
        else if (VerbosityName == TEXT("Fatal"))       Verbosity = ELogVerbosity::Fatal;
        else if (VerbosityName == TEXT("Error"))       Verbosity = ELogVerbosity::Error;
        else if (VerbosityName == TEXT("Warning"))     Verbosity = ELogVerbosity::Warning;
        else if (VerbosityName == TEXT("Display"))     Verbosity = ELogVerbosity::Display;
        else if (VerbosityName == TEXT("Log"))         Verbosity = ELogVerbosity::Log;
        else if (VerbosityName == TEXT("Verbose"))     Verbosity = ELogVerbosity::Verbose;
        else if (VerbosityName == TEXT("VeryVerbose")) Verbosity = ELogVerbosity::VeryVerbose;
        else
        {
            return InvalidParams(Request.Id,
                FString::Printf(TEXT("Unknown verbosity '%s'"), *VerbosityName));
        }

        // Why the console shim instead of a direct API:
        //   - FLogCategoryBase::SetVerbosity() exists but requires a pointer to a specific category
        //     instance. Category instances are per-TU globals (LogTemp, LogSlate, etc).
        //   - UE 5.7 Core does NOT expose a public lookup-by-name API; FLogSuppressionInterface only
        //     exposes AssociateSuppress/DisassociateSuppress/ProcessConfigAndCommandLine. The name
        //     -> FLogCategoryBase* multimap is private to FLogSuppressionImplementation.
        //   - The "log <Cat> <Verb>" console command is implemented by FLogSuppressionImplementation
        //     and is the supported public path. It logs one line per affected category to the
        //     provided FOutputDevice, which we capture to detect "no category matched".
        const FString Verbosity_c = VerbosityName;
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [CategoryName, Verbosity_c]() -> TSharedPtr<FJsonObject>
            {
                if (!GEngine || !GLog)
                {
                    return FMCPJson::MakeError(TEXT("GEngine/GLog unavailable"));
                }
                const FString Cmd = FString::Printf(TEXT("log %s %s"), *CategoryName, *Verbosity_c);
                FStringOutputDevice Captured;
                const bool bExecuted = GEngine->Exec(nullptr, *Cmd, Captured);

                // The LOG exec writes one "<name>  <verbosity>  ..." line per category whose
                // verbosity actually changed. Empty output => either the category name did not
                // match anything registered, OR the category was already at the requested level.
                // We cannot disambiguate without private API, so we surface an advisory.
                // Resolve IsEmpty() via the FString base explicitly (FStringOutputDevice has two
                // parent classes).
                const FString& CapturedStr = static_cast<const FString&>(Captured);
                const bool bAnyCategoryAffected = !CapturedStr.IsEmpty();

                TSharedPtr<FJsonObject> OutInner = FMCPJson::MakeSuccess();
                OutInner->SetStringField(TEXT("category"), CategoryName);
                OutInner->SetStringField(TEXT("verbosity"), Verbosity_c);
                OutInner->SetBoolField(TEXT("executed"), bExecuted);
                OutInner->SetBoolField(TEXT("any_category_affected"), bAnyCategoryAffected);
                if (bAnyCategoryAffected)
                {
                    OutInner->SetStringField(TEXT("affected"), CapturedStr.TrimStartAndEnd());
                }
                else
                {
                    OutInner->SetStringField(TEXT("advisory"), FString::Printf(
                        TEXT("No category rows were emitted by the LOG command. Either '%s' is not a "
                             "registered category, or it was already at verbosity '%s'. "
                             "Category names are case-insensitive."),
                        *CategoryName, *Verbosity_c));
                }
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: set verbosity %s = %s (exec=%d, affected=%d)"),
                    *CategoryName, *Verbosity_c, bExecuted ? 1 : 0, bAnyCategoryAffected ? 1 : 0);
                return OutInner;
            });
        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("log"), MethodName);
}
