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
        TEXT("Add a Level Sequence as a new job in the Movie Pipeline Queue. Returns {success, job_index, job_name, sequence_path, config_kind:'classic'|'graph', config_path?}; pass job_index to render_queue/set_output, get_status, and start_render. "
             "Params: sequence_path (string, /Game virtual path to a ULevelSequence, required); job_name (string, optional, defaults to the sequence's asset name); config_path (string, optional /Game path to a UMoviePipelinePrimaryConfig or UMovieGraphConfig). "
             "Workflow: queue_sequence -> render_queue/set_output (classic configs only) -> render_queue/start_render; for graph configs edit the graph's Output node instead of set_output. "
             "Warning: does NOT start rendering (use render_queue/start_render or the Movie Render Queue UI). A classic config auto-seeds a default Output setting so set_output works; a UMovieGraphConfig is applied via SetGraphPreset and set_output is not applicable. Returns an error if sequence_path or config_path fails to load or config_path is neither config type."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game virtual path to a ULevelSequence asset"))
        .OptionalString(TEXT("job_name"), TEXT("Human-readable job name shown in the queue (defaults to the sequence asset name)"))
        .OptionalString(TEXT("config_path"), TEXT("/Game path to a UMoviePipelinePrimaryConfig (classic) or UMovieGraphConfig (graph preset)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_output"),
        TEXT("Configure the Output setting of a queued classic-config job (directory, resolution, file-name format). Returns {success, job_index, output_directory, resolution_x, resolution_y, filename_format} reflecting the post-edit values. "
             "Params: job_index (integer, 0-based index from queue_sequence, required); output_directory (string, optional; absolute path or MRQ tokens like {project_dir}); resolution_x (integer px, optional); resolution_y (integer px, optional); filename_format (string, optional, e.g. {sequence_name}.{frame_number}). "
             "Workflow: render_queue/queue_sequence -> set_output -> render_queue/start_render; only fields you pass are changed. "
             "Warning: you must supply at least one of output_directory/resolution_x/resolution_y/filename_format or the call errors. Only works on classic-config jobs -- a graph-config job has no primary configuration and returns an error (edit the graph's Output node instead). Errors on an out-of-range job_index."))
        .RequiredInteger(TEXT("job_index"), TEXT("Zero-based job index from queue_sequence"))
        .OptionalString(TEXT("output_directory"), TEXT("Output directory: absolute path or MRQ tokens (e.g. {project_dir})"))
        .OptionalInteger(TEXT("resolution_x"), TEXT("Output width in pixels"))
        .OptionalInteger(TEXT("resolution_y"), TEXT("Output height in pixels"))
        .OptionalString(TEXT("filename_format"), TEXT("File-name format string, e.g. {sequence_name}.{frame_number}"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_status"),
        TEXT("Report the Movie Pipeline Queue state. Returns {success, is_rendering, total, queued, running, complete, jobs:[{job_index, job_name, sequence_path, status:'queued'|'complete', status_message, progress (0..1), config_kind:'classic'|'graph'}]}. "
             "Params: (none). "
             "Workflow: call after render_queue/queue_sequence to confirm jobs landed, and during render_queue/start_render to poll progress. "
             "Warning: read-only. A job's status is 'complete' only once it has been consumed by a render -- freshly queued jobs always read 'queued', so this does not by itself prove a render succeeded."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("start_render"),
        TEXT("Render the current Movie Render Queue with the in-process executor and block until it finishes. Returns {success, timed_out}. "
             "Polls progress every 500 ms from UMoviePipelineExecutorBase::GetStatusProgress() and emits notifications/progress (0..1) over the session's SSE stream. "
             "Params: (none). "
             "Workflow: render_queue/queue_sequence -> render_queue/set_output -> start_render; poll render_queue/get_status separately if you are not consuming the SSE stream. "
             "Warning: blocks the handler until the render completes -- can take minutes to hours; there is a 6-hour hard timeout (timed_out=true if hit). Errors if a render is already running or the queue is empty. For live progress, call from a client sending Accept: text/event-stream."))
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
