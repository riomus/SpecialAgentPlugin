// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/RenderQueueService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Graph/MovieGraphConfig.h"
#include "LevelSequence.h"
#include "MoviePipelineInProcessExecutor.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"

FString FRenderQueueService::GetServiceDescription() const
{
    return TEXT("Movie Render Queue sequence rendering");
}

FMCPResponse FRenderQueueService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("queue_sequence")) return HandleQueueSequence(Request);
    if (MethodName == TEXT("set_output"))     return HandleSetOutput(Request);
    if (MethodName == TEXT("get_status"))     return HandleGetStatus(Request);
    if (MethodName == TEXT("start_render"))   return HandleStartRender(Request, Ctx);

    return MethodNotFound(Request.Id, TEXT("render_queue"), MethodName);
}

TArray<FMCPToolInfo> FRenderQueueService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("queue_sequence"),
        TEXT("Add a Level Sequence as a new render job in the Movie Pipeline Queue. Returns the job index. "
             "Params: sequence_path (string, /Game/... ULevelSequence), job_name (string, optional), "
             "config_path (string, optional asset path to a UMoviePipelinePrimaryConfig or UMovieGraphConfig). "
             "Workflow: queue_sequence -> set_output (classic configs only) -> (render via UI or executor). "
             "Warning: does not start the render; use the Movie Render Queue UI or an executor. "
             "When config_path points at a UMovieGraphConfig the graph preset is applied via SetGraphPreset "
             "and set_output is not applicable (edit the graph's Output node instead)."))
        .RequiredString(TEXT("sequence_path"), TEXT("Asset path to ULevelSequence"))
        .OptionalString(TEXT("job_name"), TEXT("Human-readable job name shown in the queue"))
        .OptionalString(TEXT("config_path"), TEXT("Asset path to UMoviePipelinePrimaryConfig or UMovieGraphConfig (graph preset)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_output"),
        TEXT("Configure the output setting of a queued job: directory, resolution, file-name format. "
             "Params: job_index (integer, 0-based), output_directory (string, absolute/abs or {project_dir}/...), "
             "resolution_x (integer, pixels), resolution_y (integer, pixels), filename_format (string, optional). "
             "Workflow: queue_sequence -> set_output. "
             "Warning: only graph-config-less jobs; for graph jobs, use the graph's Output node."))
        .RequiredInteger(TEXT("job_index"), TEXT("Zero-based index in the queue"))
        .OptionalString(TEXT("output_directory"), TEXT("Output directory (absolute or project-relative tokens allowed)"))
        .OptionalInteger(TEXT("resolution_x"), TEXT("Output width in pixels"))
        .OptionalInteger(TEXT("resolution_y"), TEXT("Output height in pixels"))
        .OptionalString(TEXT("filename_format"), TEXT("File-name format string, e.g. {sequence_name}.{frame_number}"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_status"),
        TEXT("Report the state of the Movie Pipeline Queue: queued/running/complete counts and per-job status. "
             "Params: none. "
             "Workflow: call after queue_sequence / during render to monitor progress. "
             "Warning: 'complete' here means is_consumed==true; fresh jobs show 'queued'."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("start_render"),
        TEXT("Start rendering the current Movie Render Queue using the in-process executor. "
             "Returns {success, frames_rendered?}. Emits notifications/progress every 500 ms based on "
             "UMoviePipelineExecutorBase::GetStatusProgress(). "
             "Params: (none). "
             "Workflow: queue_sequence -> set_output -> start_render. "
             "Warning: blocks the handler until rendering finishes — can take minutes to hours. "
             "Emits notifications/progress over the session's SSE stream, so call from a client with "
             "Accept: text/event-stream if you want live progress."))
        .Build());

    return Tools;
}

namespace
{
    UMoviePipelineQueue* GetQueue()
    {
        if (!GEditor) return nullptr;
        UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
        return Subsystem ? Subsystem->GetQueue() : nullptr;
    }

    UMoviePipelineQueueSubsystem* GetQueueSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    }
}

FMCPResponse FRenderQueueService::HandleQueueSequence(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }

    FString JobName;
    FMCPJson::ReadString(Request.Params, TEXT("job_name"), JobName);

    FString ConfigPath;
    const bool bHasConfigPath = FMCPJson::ReadString(Request.Params, TEXT("config_path"), ConfigPath) && !ConfigPath.IsEmpty();

    auto Task = [SequencePath, JobName, ConfigPath, bHasConfigPath]() -> TSharedPtr<FJsonObject>
    {
        UMoviePipelineQueue* Queue = GetQueue();
        if (!Queue)
        {
            return FMCPJson::MakeError(TEXT("Movie Pipeline Queue subsystem unavailable"));
        }

        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

        // Resolve optional config asset and decide which branch to use.
        UMovieGraphConfig* GraphConfig = nullptr;
        UMoviePipelinePrimaryConfig* ClassicConfig = nullptr;
        if (bHasConfigPath)
        {
            UObject* ConfigObj = LoadObject<UObject>(nullptr, *ConfigPath);
            if (!ConfigObj)
            {
                return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load config: %s"), *ConfigPath));
            }
            GraphConfig = Cast<UMovieGraphConfig>(ConfigObj);
            if (!GraphConfig)
            {
                ClassicConfig = Cast<UMoviePipelinePrimaryConfig>(ConfigObj);
            }
            if (!GraphConfig && !ClassicConfig)
            {
                return FMCPJson::MakeError(FString::Printf(
                    TEXT("config_path '%s' is neither UMovieGraphConfig nor UMoviePipelinePrimaryConfig (got %s)"),
                    *ConfigPath, *ConfigObj->GetClass()->GetName()));
            }
        }

        UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
        if (!Job)
        {
            return FMCPJson::MakeError(TEXT("AllocateNewJob returned null"));
        }

        Job->SetSequence(FSoftObjectPath(Sequence));
        Job->JobName = JobName.IsEmpty() ? Sequence->GetName() : JobName;

        FString ConfigKind;
        if (GraphConfig)
        {
            // Graph-config path (UE 5.5+): assign the preset; the graph's Output node supplies output settings.
            Job->SetGraphPreset(GraphConfig);
            ConfigKind = TEXT("graph");
        }
        else
        {
            // Classic path: either use the provided UMoviePipelinePrimaryConfig, or the auto-allocated default.
            if (ClassicConfig)
            {
                Job->SetConfiguration(ClassicConfig);
            }

            // Seed a default Output setting on the classic config so set_output works without re-adding.
            if (UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration())
            {
                Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass());
            }
            ConfigKind = TEXT("classic");
        }

        const int32 JobIndex = Queue->GetJobs().Find(Job);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("job_index"), JobIndex);
        Result->SetStringField(TEXT("job_name"), Job->JobName);
        Result->SetStringField(TEXT("sequence_path"), SequencePath);
        Result->SetStringField(TEXT("config_kind"), ConfigKind);
        if (bHasConfigPath)
        {
            Result->SetStringField(TEXT("config_path"), ConfigPath);
        }
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderQueueService::HandleSetOutput(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    int32 JobIndex = -1;
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("job_index"), JobIndex))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'job_index'"));
    }

    FString OutputDir;
    const bool bHasDir = FMCPJson::ReadString(Request.Params, TEXT("output_directory"), OutputDir);
    int32 ResX = 0, ResY = 0;
    const bool bHasResX = FMCPJson::ReadInteger(Request.Params, TEXT("resolution_x"), ResX);
    const bool bHasResY = FMCPJson::ReadInteger(Request.Params, TEXT("resolution_y"), ResY);
    FString FileNameFormat;
    const bool bHasFormat = FMCPJson::ReadString(Request.Params, TEXT("filename_format"), FileNameFormat);

    if (!bHasDir && !bHasResX && !bHasResY && !bHasFormat)
    {
        return InvalidParams(Request.Id, TEXT("Provide at least one of output_directory/resolution_x/resolution_y/filename_format"));
    }

    auto Task = [JobIndex, OutputDir, ResX, ResY, FileNameFormat,
                 bHasDir, bHasResX, bHasResY, bHasFormat]() -> TSharedPtr<FJsonObject>
    {
        UMoviePipelineQueue* Queue = GetQueue();
        if (!Queue)
        {
            return FMCPJson::MakeError(TEXT("Movie Pipeline Queue subsystem unavailable"));
        }

        TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
        if (!Jobs.IsValidIndex(JobIndex))
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Invalid job_index %d (queue has %d jobs)"), JobIndex, Jobs.Num()));
        }

        UMoviePipelineExecutorJob* Job = Jobs[JobIndex];
        UMoviePipelinePrimaryConfig* Config = Job ? Job->GetConfiguration() : nullptr;
        if (!Config)
        {
            return FMCPJson::MakeError(TEXT("Job has no primary configuration (graph-config jobs not supported)"));
        }

        UMoviePipelineOutputSetting* OutputSetting = Cast<UMoviePipelineOutputSetting>(
            Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
        if (!OutputSetting)
        {
            return FMCPJson::MakeError(TEXT("Failed to get UMoviePipelineOutputSetting"));
        }

        if (bHasDir)
        {
            OutputSetting->OutputDirectory.Path = OutputDir;
        }
        if (bHasResX)
        {
            OutputSetting->OutputResolution.X = ResX;
        }
        if (bHasResY)
        {
            OutputSetting->OutputResolution.Y = ResY;
        }
        if (bHasFormat)
        {
            OutputSetting->FileNameFormat = FileNameFormat;
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("job_index"), JobIndex);
        Result->SetStringField(TEXT("output_directory"), OutputSetting->OutputDirectory.Path);
        Result->SetNumberField(TEXT("resolution_x"), OutputSetting->OutputResolution.X);
        Result->SetNumberField(TEXT("resolution_y"), OutputSetting->OutputResolution.Y);
        Result->SetStringField(TEXT("filename_format"), OutputSetting->FileNameFormat);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderQueueService::HandleGetStatus(const FMCPRequest& Request)
{
    auto Task = []() -> TSharedPtr<FJsonObject>
    {
        UMoviePipelineQueueSubsystem* Subsystem = GetQueueSubsystem();
        if (!Subsystem)
        {
            return FMCPJson::MakeError(TEXT("Movie Pipeline Queue subsystem unavailable"));
        }

        UMoviePipelineQueue* Queue = Subsystem->GetQueue();
        TArray<UMoviePipelineExecutorJob*> Jobs = Queue ? Queue->GetJobs() : TArray<UMoviePipelineExecutorJob*>();

        int32 QueuedCount = 0, CompleteCount = 0;
        TArray<TSharedPtr<FJsonValue>> JobArr;
        for (int32 i = 0; i < Jobs.Num(); ++i)
        {
            UMoviePipelineExecutorJob* Job = Jobs[i];
            if (!Job) continue;

            const bool bConsumed = Job->IsConsumed();
            const FString State = bConsumed ? TEXT("complete") : TEXT("queued");
            if (bConsumed) { ++CompleteCount; } else { ++QueuedCount; }

            TSharedPtr<FJsonObject> JobObj = MakeShared<FJsonObject>();
            JobObj->SetNumberField(TEXT("job_index"), i);
            JobObj->SetStringField(TEXT("job_name"), Job->JobName);
            JobObj->SetStringField(TEXT("sequence_path"), Job->Sequence.ToString());
            JobObj->SetStringField(TEXT("status"), State);
            JobObj->SetStringField(TEXT("status_message"), Job->GetStatusMessage());
            JobObj->SetNumberField(TEXT("progress"), Job->GetStatusProgress());
            JobObj->SetStringField(TEXT("config_kind"),
                Job->IsUsingGraphConfiguration() ? TEXT("graph") : TEXT("classic"));
            JobArr.Add(MakeShared<FJsonValueObject>(JobObj));
        }

        const bool bRendering = Subsystem->IsRendering();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetBoolField(TEXT("is_rendering"), bRendering);
        Result->SetNumberField(TEXT("total"), Jobs.Num());
        Result->SetNumberField(TEXT("queued"), QueuedCount);
        Result->SetNumberField(TEXT("running"), bRendering ? 1 : 0);
        Result->SetNumberField(TEXT("complete"), CompleteCount);
        Result->SetArrayField(TEXT("jobs"), JobArr);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderQueueService::HandleStartRender(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
    const auto SendProgress = Ctx.SendProgress;
    SendProgress(0.0, 1.0, TEXT("movie render queue: starting"));

    // Step 1: kick on the game thread
    struct FKick { bool bOk = false; FString Error; };
    auto KickTask = []() -> FKick
    {
        FKick K;
        if (!GEditor) { K.Error = TEXT("GEditor unavailable"); return K; }
        UMoviePipelineQueueSubsystem* Subsys = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
        if (!Subsys) { K.Error = TEXT("MoviePipelineQueueSubsystem not available"); return K; }
        if (Subsys->IsRendering())
        {
            K.Error = TEXT("A render is already in progress");
            return K;
        }
        UMoviePipelineQueue* Queue = Subsys->GetQueue();
        if (!Queue || Queue->GetJobs().Num() == 0)
        {
            K.Error = TEXT("Queue is empty — call render_queue/queue_sequence first");
            return K;
        }
        UMoviePipelineExecutorBase* Exec = Subsys->RenderQueueWithExecutor(UMoviePipelineInProcessExecutor::StaticClass());
        if (!Exec) { K.Error = TEXT("RenderQueueWithExecutor returned null"); return K; }
        K.bOk = true;
        return K;
    };
    FKick Kick = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<FKick>(KickTask);
    if (!Kick.bOk)
    {
        SendProgress(1.0, 1.0, FString::Printf(TEXT("movie render queue: failed — %s"), *Kick.Error));
        TSharedPtr<FJsonObject> Err = FMCPJson::MakeError(Kick.Error);
        return FMCPResponse::Success(Request.Id, Err);
    }

    // Step 2: poll loop
    struct FPoll { bool bActive = false; float Progress = 0.f; };
    const double StartTime = FPlatformTime::Seconds();
    constexpr double HardTimeoutSeconds = 6 * 60 * 60;  // 6 hours — sanity cap
    bool bTimedOut = false;
    float LastEmittedProgress = -1.f;

    while (true)
    {
        auto PollTask = []() -> FPoll
        {
            FPoll P;
            if (!GEditor) return P;
            UMoviePipelineQueueSubsystem* Subsys = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
            if (!Subsys) return P;
            UMoviePipelineExecutorBase* Exec = Subsys->GetActiveExecutor();
            if (!Exec) return P;  // finished / never started
            P.bActive = Subsys->IsRendering();
            P.Progress = FMath::Clamp(Exec->GetStatusProgress(), 0.f, 1.f);
            return P;
        };
        const FPoll P = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<FPoll>(PollTask);

        // Emit only when progress has moved — keeps the SSE stream quiet when executor is idle.
        if (P.Progress != LastEmittedProgress)
        {
            SendProgress(double(P.Progress), 1.0,
                FString::Printf(TEXT("movie render queue: %.1f%%"), P.Progress * 100.f));
            LastEmittedProgress = P.Progress;
        }

        if (!P.bActive) break;
        if (FPlatformTime::Seconds() - StartTime > HardTimeoutSeconds)
        {
            bTimedOut = true;
            break;
        }
        FPlatformProcess::Sleep(0.5f);
    }

    SendProgress(1.0, 1.0, bTimedOut
        ? TEXT("movie render queue: timeout (6h cap)")
        : TEXT("movie render queue: complete"));

    TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
    Result->SetBoolField(TEXT("timed_out"), bTimedOut);
    return FMCPResponse::Success(Request.Id, Result);
}
