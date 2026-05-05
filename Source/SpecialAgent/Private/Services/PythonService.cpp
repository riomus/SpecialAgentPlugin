// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"

namespace
{
	struct FDeprecationEntry
	{
		FString Deprecated;
		FString Modern;
		FString Notes;
	};

	static const TArray<FDeprecationEntry>& GetDeprecationsTable()
	{
		static TArray<FDeprecationEntry> Table;
		static bool bLoaded = false;
		if (bLoaded) return Table;
		bLoaded = true;

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
		if (!Plugin.IsValid()) return Table;
		const FString Path = FPaths::Combine(Plugin->GetContentDir(), TEXT("Docs"), TEXT("deprecations.md"));

		FString Body;
		if (!FFileHelper::LoadFileToString(Body, *Path))
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: deprecations.md not found at %s"), *Path);
			return Table;
		}

		TArray<FString> Lines;
		Body.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);
		bool bSeenHeader = false;
		for (const FString& Line : Lines)
		{
			if (!Line.StartsWith(TEXT("| "))) continue;
			if (Line.Contains(TEXT("|---"))) continue;

			TArray<FString> Cols;
			Line.ParseIntoArray(Cols, TEXT("|"), /*InCullEmpty=*/false);
			if (Cols.Num() < 4) continue;

			FDeprecationEntry E;
			E.Deprecated = Cols[1].TrimStartAndEnd();
			E.Modern     = Cols[2].TrimStartAndEnd();
			E.Notes      = Cols[3].TrimStartAndEnd();

			if (!bSeenHeader)
			{
				bSeenHeader = true;
				// Skip the literal header row "| Deprecated | Modern replacement | Notes |".
				if (E.Deprecated.Equals(TEXT("Deprecated"), ESearchCase::IgnoreCase)
				    && E.Modern.StartsWith(TEXT("Modern"), ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			if (!E.Deprecated.IsEmpty() && !E.Modern.IsEmpty())
			{
				Table.Add(E);
			}
		}
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: loaded %d deprecation entries"), Table.Num());
		return Table;
	}

	// Run a Python wrapper that writes JSON to a temp file and return the parsed result.
	// On any failure, returns {success:false, error:<msg>}.
	static TSharedPtr<FJsonObject> RunPythonAndReadJson(const FString& WrappedCode, const FString& TempFile)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		IPythonScriptPlugin* P = IPythonScriptPlugin::Get();
		if (!P)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Python plugin unavailable"));
			return Result;
		}

		FPythonCommandEx Cmd;
		Cmd.Command = WrappedCode;
		Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;
		P->ExecPythonCommandEx(Cmd);

		FString Json;
		if (FFileHelper::LoadFileToString(Json, *TempFile))
		{
			TSharedPtr<FJsonObject> Parsed;
			TSharedRef<TJsonReader<>> Rd = TJsonReaderFactory<>::Create(Json);
			if (FJsonSerializer::Deserialize(Rd, Parsed) && Parsed.IsValid())
			{
				Result = Parsed;
			}
			IFileManager::Get().Delete(*TempFile);
		}
		if (!Result->HasField(TEXT("success")))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Python wrapper produced no output"));
		}
		return Result;
	}
}

FPythonService::FPythonService()
{
}

FString FPythonService::GetServiceDescription() const
{
	return TEXT("Python script execution - PRIMARY control mechanism with full UE5 API access");
}

TArray<FMCPToolInfo> FPythonService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// execute
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("execute");
		Tool.Description = TEXT("Execute arbitrary Python code via IPythonScriptPlugin in the game thread with full UE5 API. Returns {success, stdout, stderr, execution_time}. "
			"Params: code (string, Python source; 'unreal' module is auto-imported, required); timeout (number seconds, default 30). "
			"Workflow: use as an escape hatch when no direct tool exists — prefer world/*, assets/*, blueprint/* etc. for supported operations. "
			"IMPORTANT: if your script writes viewport camera data (e.g. UnrealEditorSubsystem.set_level_viewport_camera_info) or any other viewport-client state, the pixels are NOT yet repainted when this tool returns — call viewport/force_redraw afterwards before screenshot/capture or screenshot/save, otherwise the capture still shows the previous frame. "
			"Warning: runs on the game thread and can crash the editor; side-effects are not inside an undo transaction unless you wrap them with utility/begin_transaction.");
		
		TSharedPtr<FJsonObject> CodeParam = MakeShared<FJsonObject>();
		CodeParam->SetStringField(TEXT("type"), TEXT("string"));
		CodeParam->SetStringField(TEXT("description"), TEXT("Python code to execute. Has access to 'unreal' module and all UE5 Python API."));
		Tool.Parameters->SetObjectField(TEXT("code"), CodeParam);
		
		TSharedPtr<FJsonObject> TimeoutParam = MakeShared<FJsonObject>();
		TimeoutParam->SetStringField(TEXT("type"), TEXT("number"));
		TimeoutParam->SetStringField(TEXT("description"), TEXT("Execution timeout in seconds (default: 30.0)"));
		Tool.Parameters->SetObjectField(TEXT("timeout"), TimeoutParam);
		
		Tool.RequiredParams.Add(TEXT("code"));
		Tools.Add(Tool);
	}
	
	// execute_file
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("execute_file");
		Tool.Description = TEXT("Execute a Python script file on disk via IPythonScriptPlugin. Returns {success, stdout, stderr, execution_time}. "
			"Params: file_path (string, path relative to Content/Python/ or absolute, required). "
			"Workflow: use for reusable multi-line scripts; pair with python/list_modules to discover available scripts. "
			"Warning: no arg passing — all inputs must be embedded in the script file.");
		
		TSharedPtr<FJsonObject> FilePathParam = MakeShared<FJsonObject>();
		FilePathParam->SetStringField(TEXT("type"), TEXT("string"));
		FilePathParam->SetStringField(TEXT("description"), TEXT("Path to Python file relative to Content/Python/"));
		Tool.Parameters->SetObjectField(TEXT("file_path"), FilePathParam);
		
		Tool.RequiredParams.Add(TEXT("file_path"));
		Tools.Add(Tool);
	}
	
	// list_modules
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_modules");
		Tool.Description = TEXT("List Python scripts found under the project's Content/Python/ directory. Returns {files:[relative_path], count}. "
			"Params: (none). "
			"Workflow: call before python/execute_file to discover candidate script paths. "
			"Warning: does not recurse into subdirectories.");
		Tools.Add(Tool);
	}

	// help — wraps Python help() / inspect.signature for any unreal.* symbol
	Tools.Add(FMCPToolBuilder(TEXT("help"),
		TEXT("Return docstring + signature for any unreal.* symbol via help() / inspect.\n"
		     "Params: symbol (string, required, e.g. 'unreal.EditorActorSubsystem.spawn_actor_from_class').\n"
		     "Workflow: call before guessing API; pair with python/get_function_signature for exact arg types.\n"
		     "Warning: large classes can produce long output — truncated to ~8 KB."))
		.RequiredString(TEXT("symbol"), TEXT("Fully-qualified unreal.* symbol path"))
		.Build());

	// inspect_class — list methods/properties/MRO
	Tools.Add(FMCPToolBuilder(TEXT("inspect_class"),
		TEXT("List methods, properties, and inheritance chain for an unreal.* class.\n"
		     "Params: class_name (string, required, e.g. 'unreal.EditorActorSubsystem' or bare 'EditorActorSubsystem').\n"
		     "Workflow: discovery before guessing API; gives you the menu of callable methods.\n"
		     "Warning: includes inherited members; chain is in __mro__ order; capped at 200 entries each."))
		.RequiredString(TEXT("class_name"), TEXT("Class name (full path or bare name)"))
		.Build());

	// list_subsystems — Editor/EngineSubsystem catalog
	Tools.Add(FMCPToolBuilder(TEXT("list_subsystems"),
		TEXT("List all unreal.EditorSubsystem and unreal.EngineSubsystem subclasses available in this build.\n"
		     "Params: (none).\n"
		     "Workflow: this is the modern UE5 entry-point catalog; pick a subsystem here before reaching for any *Library class.\n"
		     "Warning: only subsystems whose modules are loaded in the current editor are listed."))
		.Build());

	// search_symbol — substring search dir(unreal)
	Tools.Add(FMCPToolBuilder(TEXT("search_symbol"),
		TEXT("Substring-search dir(unreal) for matching class / function / enum names.\n"
		     "Params: substring (string, required, case-insensitive).\n"
		     "Workflow: cheapest discovery primitive; e.g. substring='Foliage' surfaces every Foliage* type/subsystem.\n"
		     "Warning: capped at 200 matches; refine substring if 'truncated':true."))
		.RequiredString(TEXT("substring"), TEXT("Case-insensitive substring to match against names in dir(unreal)"))
		.Build());

	// get_function_signature
	Tools.Add(FMCPToolBuilder(TEXT("get_function_signature"),
		TEXT("Return parameter list, types, and return type for an unreal.<Class>.<method>.\n"
		     "Params: class_name (string, required), method (string, required).\n"
		     "Workflow: confirm exact arg order/types before calling; saves wrong-arg crashes.\n"
		     "Warning: signatures come from inspect; default values may print as <unreal.Foo object>; falls back to docstring on failure."))
		.RequiredString(TEXT("class_name"), TEXT("Class name (full path or bare)"))
		.RequiredString(TEXT("method"), TEXT("Method name on that class"))
		.Build());

	// list_enum_values
	Tools.Add(FMCPToolBuilder(TEXT("list_enum_values"),
		TEXT("Dump all values of an unreal enum (e.g. unreal.ETextureSourceFormat).\n"
		     "Params: enum_name (string, required).\n"
		     "Workflow: pair with material / asset_import / foliage tools whose params expect enum values.\n"
		     "Warning: caps at 500 values; raises error if the symbol is not an enum."))
		.RequiredString(TEXT("enum_name"), TEXT("Full path or bare name of the unreal enum"))
		.Build());

	// get_asset_class_for_path
	Tools.Add(FMCPToolBuilder(TEXT("get_asset_class_for_path"),
		TEXT("Look up the Python class an asset path resolves to (so you load it via the right API).\n"
		     "Params: asset_path (string, required, e.g. '/Game/Foo/Bar' or '/Game/Foo/Bar.Bar').\n"
		     "Workflow: call before unreal.load_asset / EditorAssetSubsystem.load_asset to avoid type-cast surprises.\n"
		     "Warning: returns {exists:false} cleanly when the path is missing — does not raise."))
		.RequiredString(TEXT("asset_path"), TEXT("Content browser asset path"))
		.Build());

	// diff_against_deprecated — pure C++ scan
	Tools.Add(FMCPToolBuilder(TEXT("diff_against_deprecated"),
		TEXT("Scan a Python snippet for calls to deprecated UE5 APIs and suggest modern replacements.\n"
		     "Params: snippet (string, required, Python source to scan).\n"
		     "Workflow: paste your draft before python/execute — flags every entry from the deprecated-to-modern table and points at the modern subsystem to use instead.\n"
		     "Warning: substring match against Content/Docs/deprecations.md; false positives possible inside string literals."))
		.RequiredString(TEXT("snippet"), TEXT("Python source to scan"))
		.Build());

	return Tools;
}

FMCPResponse FPythonService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("execute")) return HandleExecute(Request);
	if (MethodName == TEXT("execute_file")) return HandleExecuteFile(Request);
	if (MethodName == TEXT("list_modules")) return HandleListModules(Request);
	if (MethodName == TEXT("help")) return HandleHelp(Request);
	if (MethodName == TEXT("inspect_class")) return HandleInspectClass(Request);
	if (MethodName == TEXT("list_subsystems")) return HandleListSubsystems(Request);
	if (MethodName == TEXT("search_symbol")) return HandleSearchSymbol(Request);
	if (MethodName == TEXT("get_function_signature")) return HandleGetFunctionSignature(Request);
	if (MethodName == TEXT("list_enum_values")) return HandleListEnumValues(Request);
	if (MethodName == TEXT("get_asset_class_for_path")) return HandleGetAssetClassForPath(Request);
	if (MethodName == TEXT("diff_against_deprecated")) return HandleDiffAgainstDeprecated(Request);

	return MethodNotFound(Request.Id, TEXT("python"), MethodName);
}

FMCPResponse FPythonService::HandleExecute(const FMCPRequest& Request)
{
	// Get parameters
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Code;
	if (!Request.Params->TryGetStringField(TEXT("code"), Code))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'code'"));
	}

	float Timeout = 30.0f;
	Request.Params->TryGetNumberField(TEXT("timeout"), Timeout);

	// Execute on game thread
	auto ExecuteTask = [Code]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Check if Python plugin is available
		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		if (!PythonPlugin)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("stdout"), TEXT(""));
			Result->SetStringField(TEXT("stderr"), TEXT("Python Script Plugin is not available. Make sure it is enabled in Project Settings."));
			Result->SetNumberField(TEXT("execution_time"), 0.0);
			return Result;
		}

		// Execute Python code
		double StartTime = FPlatformTime::Seconds();
		
		// Generate a unique temporary file path
		FString TempDir = FPaths::ProjectIntermediateDir();
		FString TempFile = FPaths::Combine(TempDir, TEXT("mcp_python_output.txt"));
		
		// Wrap user code to capture stdout/stderr and write to temp file
		FString IndentedCode = TEXT("    ") + Code.Replace(TEXT("\n"), TEXT("\n    "));
		
		FString WrappedCode = FString::Printf(TEXT(
			"import sys\n"
			"import io\n"
			"import json\n"
			"_stdout_capture = io.StringIO()\n"
			"_stderr_capture = io.StringIO()\n"
			"_old_stdout = sys.stdout\n"
			"_old_stderr = sys.stderr\n"
			"sys.stdout = _stdout_capture\n"
			"sys.stderr = _stderr_capture\n"
			"_exec_success = True\n"
			"try:\n"
			"%s\n"
			"except Exception as _e:\n"
			"    _exec_success = False\n"
			"    import traceback\n"
			"    sys.stderr.write(traceback.format_exc())\n"
			"finally:\n"
			"    sys.stdout = _old_stdout\n"
			"    sys.stderr = _old_stderr\n"
			"    # Write result to temp file\n"
			"    with open(r'%s', 'w', encoding='utf-8') as _f:\n"
			"        import json\n"
			"        json.dump({\n"
			"            'stdout': _stdout_capture.getvalue(),\n"
			"            'stderr': _stderr_capture.getvalue(),\n"
			"            'success': _exec_success\n"
			"        }, _f)\n"
		), *IndentedCode, *TempFile);
		
		FPythonCommandEx PythonCommand;
		PythonCommand.Command = WrappedCode;
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public;

		bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);
		
		// Read the JSON result from the temp file
		FString JsonString;
		FString StdOut;
		FString StdErr;
		
		if (FFileHelper::LoadFileToString(JsonString, *TempFile))
		{
			// Parse the JSON
			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
			if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
			{
				JsonObject->TryGetStringField(TEXT("stdout"), StdOut);
				JsonObject->TryGetStringField(TEXT("stderr"), StdErr);
				JsonObject->TryGetBoolField(TEXT("success"), bSuccess);
				
				UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Successfully retrieved output from temp file"));
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: Failed to parse JSON from temp file: %s"), *JsonString);
				StdErr = TEXT("Failed to parse execution result");
				bSuccess = false;
			}
			
			// Clean up temp file
			IFileManager::Get().Delete(*TempFile);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: Failed to read temp file: %s"), *TempFile);
			StdErr = TEXT("Failed to read execution result");
			bSuccess = false;
		}
		
		double ExecutionTime = FPlatformTime::Seconds() - StartTime;

		// Build result
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetStringField(TEXT("stdout"), StdOut);
		Result->SetStringField(TEXT("stderr"), StdErr);
		Result->SetNumberField(TEXT("execution_time"), ExecutionTime);

		if (!bSuccess)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: Python execution failed in %.3f seconds: %s"), 
				ExecutionTime, *StdErr);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Python execution succeeded in %.3f seconds"), ExecutionTime);
		}

		return Result;
	};

	// Execute synchronously on game thread
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(ExecuteTask);

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPythonService::HandleExecuteFile(const FMCPRequest& Request)
{
	// Get parameters
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FilePath;
	if (!Request.Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'file_path'"));
	}

	// Read file content
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("stderr"), FString::Printf(TEXT("Failed to read file: %s"), *FilePath));
		return FMCPResponse::Success(Request.Id, Result);
	}

	// Execute the file content as Python code
	auto ExecuteTask = [FileContent, FilePath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		if (!PythonPlugin)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("stderr"), TEXT("Python Script Plugin is not available"));
			return Result;
		}

		double StartTime = FPlatformTime::Seconds();

		FPythonCommandEx PythonCommand;
		PythonCommand.Command = FileContent;
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

		bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);
		
		double ExecutionTime = FPlatformTime::Seconds() - StartTime;

		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetStringField(TEXT("stdout"), PythonCommand.CommandResult);
		Result->SetStringField(TEXT("stderr"), bSuccess ? TEXT("") : PythonCommand.CommandResult);
		Result->SetNumberField(TEXT("execution_time"), ExecutionTime);
		Result->SetStringField(TEXT("file_path"), FilePath);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Python file execution %s in %.3f seconds: %s"), 
			bSuccess ? TEXT("succeeded") : TEXT("failed"), ExecutionTime, *FilePath);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(ExecuteTask);

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPythonService::HandleListModules(const FMCPRequest& Request)
{
	// Execute on game thread
	auto ListTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		if (!PythonPlugin)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Python Script Plugin is not available"));
			return Result;
		}

		// Execute Python code to list available modules
		FPythonCommandEx PythonCommand;
		PythonCommand.Command = TEXT(
			"import sys\n"
			"import json\n"
			"modules = []\n"
			"for name in sorted(sys.modules.keys()):\n"
			"    if not name.startswith('_'):\n"
			"        modules.append(name)\n"
			"print(json.dumps(modules[:100]))  # Limit to first 100\n"
		);
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
		
		bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);

		if (bSuccess && !PythonCommand.CommandResult.IsEmpty())
		{
			// Parse the JSON output
			TSharedPtr<FJsonValue> JsonValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PythonCommand.CommandResult);
			
			if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue->Type == EJson::Array)
			{
				Result->SetBoolField(TEXT("success"), true);
				Result->SetArrayField(TEXT("modules"), JsonValue->AsArray());
			}
			else
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), TEXT("Failed to parse module list"));
			}
		}
		else
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to list modules"));
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(ListTask);

	return FMCPResponse::Success(Request.Id, Result);
}

// ----------------------------------------------------------------------------
// Live introspection tools — wrap IPythonScriptPlugin::ExecPythonCommandEx
// ----------------------------------------------------------------------------

namespace
{
	// Sanitize an LLM-supplied identifier so it can be embedded into a Python
	// triple-single-quoted literal without breaking the wrapper. We strip ''' and
	// any control character; '.', '_', alnum and minus pass through.
	static FString SafePyLiteral(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("'''"), TEXT(""));
		Out.ReplaceInline(TEXT("\r"), TEXT(""));
		Out.ReplaceInline(TEXT("\n"), TEXT(""));
		return Out;
	}

	static FString MakeTempJsonPath(const TCHAR* Stem)
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(),
			FString::Printf(TEXT("mcp_python_%s.json"), Stem));
	}
}

FMCPResponse FPythonService::HandleHelp(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString Symbol;
	if (!Request.Params->TryGetStringField(TEXT("symbol"), Symbol))
		return InvalidParams(Request.Id, TEXT("Missing 'symbol'"));

	const FString SafeSymbol = SafePyLiteral(Symbol);
	const FString TempFile = MakeTempJsonPath(TEXT("help"));

	auto Task = [SafeSymbol, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import io, sys, json, inspect, importlib\n"
			"_buf = io.StringIO()\n"
			"_old = sys.stdout\n"
			"sys.stdout = _buf\n"
			"_doc = ''\n"
			"_sig = ''\n"
			"_ok = True\n"
			"_sym = '''%s'''\n"
			"try:\n"
			"    _parts = _sym.split('.')\n"
			"    _obj = importlib.import_module(_parts[0])\n"
			"    for _p in _parts[1:]:\n"
			"        _obj = getattr(_obj, _p)\n"
			"    help(_obj)\n"
			"    _doc = _buf.getvalue()[:8000]\n"
			"    try:\n"
			"        _sig = str(inspect.signature(_obj))\n"
			"    except Exception:\n"
			"        _sig = ''\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _doc = repr(_e)\n"
			"finally:\n"
			"    sys.stdout = _old\n"
			"    with open(r'%s', 'w', encoding='utf-8') as _f:\n"
			"        json.dump({'symbol':_sym,'doc':_doc,'signature':_sig,'success':_ok}, _f)\n"
		), *SafeSymbol, *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleInspectClass(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ClassName;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
		return InvalidParams(Request.Id, TEXT("Missing 'class_name'"));

	const FString Safe = SafePyLiteral(ClassName);
	const FString TempFile = MakeTempJsonPath(TEXT("inspect_class"));

	auto Task = [Safe, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json, inspect, importlib\n"
			"import unreal\n"
			"_name = '''%s'''\n"
			"_ok = True\n"
			"_err = ''\n"
			"_methods = []\n"
			"_props = []\n"
			"_mro = []\n"
			"try:\n"
			"    _parts = _name.split('.')\n"
			"    if _parts[0] == 'unreal' and len(_parts) > 1:\n"
			"        _cls = unreal\n"
			"        for _p in _parts[1:]:\n"
			"            _cls = getattr(_cls, _p)\n"
			"    else:\n"
			"        _cls = getattr(unreal, _parts[-1])\n"
			"    if not inspect.isclass(_cls):\n"
			"        raise TypeError('not a class: ' + _name)\n"
			"    _mro = [c.__name__ for c in _cls.__mro__]\n"
			"    for _n, _v in inspect.getmembers(_cls):\n"
			"        if _n.startswith('_'): continue\n"
			"        if callable(_v):\n"
			"            try: _sig = str(inspect.signature(_v))\n"
			"            except Exception: _sig = ''\n"
			"            _methods.append({'name': _n, 'signature': _sig})\n"
			"        else:\n"
			"            _props.append({'name': _n, 'type': type(_v).__name__})\n"
			"        if len(_methods) >= 200 and len(_props) >= 200: break\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _err = repr(_e)\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'class':_name,'success':_ok,'error':_err,"
			"'methods':_methods[:200],'properties':_props[:200],'mro':_mro}, _f)\n"
		), *Safe, *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleListSubsystems(const FMCPRequest& Request)
{
	const FString TempFile = MakeTempJsonPath(TEXT("list_subsystems"));

	auto Task = [TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json\n"
			"import unreal\n"
			"_subs = []\n"
			"_ok = True\n"
			"_err = ''\n"
			"try:\n"
			"    for _base_name in ('EditorSubsystem','EngineSubsystem'):\n"
			"        _base = getattr(unreal, _base_name, None)\n"
			"        if _base is None: continue\n"
			"        for _cls in _base.__subclasses__():\n"
			"            _doc = (_cls.__doc__ or '').strip().splitlines()\n"
			"            _first = _doc[0] if _doc else ''\n"
			"            _subs.append({'name':_cls.__name__,'base':_base_name,'doc_first_line':_first[:200]})\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _err = repr(_e)\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'success':_ok,'error':_err,'subsystems':_subs,'count':len(_subs)}, _f)\n"
		), *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleSearchSymbol(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString Substring;
	if (!Request.Params->TryGetStringField(TEXT("substring"), Substring))
		return InvalidParams(Request.Id, TEXT("Missing 'substring'"));

	const FString Safe = SafePyLiteral(Substring);
	const FString TempFile = MakeTempJsonPath(TEXT("search_symbol"));

	auto Task = [Safe, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json\n"
			"import unreal\n"
			"_q = '''%s'''.lower()\n"
			"_all = [n for n in dir(unreal) if _q in n.lower()]\n"
			"_truncated = len(_all) > 200\n"
			"_matches = _all[:200]\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'success':True,'substring':'''%s''',"
			"'matches':_matches,'count':len(_matches),'truncated':_truncated}, _f)\n"
		), *Safe, *TempFile, *Safe);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleGetFunctionSignature(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ClassName, Method;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
		return InvalidParams(Request.Id, TEXT("Missing 'class_name'"));
	if (!Request.Params->TryGetStringField(TEXT("method"), Method))
		return InvalidParams(Request.Id, TEXT("Missing 'method'"));

	const FString SafeC = SafePyLiteral(ClassName);
	const FString SafeM = SafePyLiteral(Method);
	const FString TempFile = MakeTempJsonPath(TEXT("get_function_signature"));

	auto Task = [SafeC, SafeM, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json, inspect\n"
			"import unreal\n"
			"_cn = '''%s'''\n"
			"_mn = '''%s'''\n"
			"_ok = True\n"
			"_err = ''\n"
			"_sig = ''\n"
			"_doc = ''\n"
			"_params = []\n"
			"_returns = ''\n"
			"try:\n"
			"    _parts = _cn.split('.')\n"
			"    if _parts[0] == 'unreal' and len(_parts) > 1:\n"
			"        _cls = unreal\n"
			"        for _p in _parts[1:]:\n"
			"            _cls = getattr(_cls, _p)\n"
			"    else:\n"
			"        _cls = getattr(unreal, _parts[-1])\n"
			"    _fn = getattr(_cls, _mn)\n"
			"    _doc = ((_fn.__doc__ or '').strip().splitlines() or [''])[0][:400]\n"
			"    try:\n"
			"        _s = inspect.signature(_fn)\n"
			"        _sig = str(_s)\n"
			"        for _pname, _pp in _s.parameters.items():\n"
			"            _params.append({'name':_pname,'kind':str(_pp.kind),"
			"'default': '' if _pp.default is inspect.Parameter.empty else repr(_pp.default),"
			"'annotation': '' if _pp.annotation is inspect.Parameter.empty else repr(_pp.annotation)})\n"
			"        _returns = '' if _s.return_annotation is inspect.Signature.empty else repr(_s.return_annotation)\n"
			"    except Exception:\n"
			"        pass\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _err = repr(_e)\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'success':_ok,'error':_err,'class':_cn,'method':_mn,"
			"'signature':_sig,'params':_params,'return_annotation':_returns,'doc':_doc}, _f)\n"
		), *SafeC, *SafeM, *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleListEnumValues(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString EnumName;
	if (!Request.Params->TryGetStringField(TEXT("enum_name"), EnumName))
		return InvalidParams(Request.Id, TEXT("Missing 'enum_name'"));

	const FString Safe = SafePyLiteral(EnumName);
	const FString TempFile = MakeTempJsonPath(TEXT("list_enum_values"));

	auto Task = [Safe, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json\n"
			"import unreal\n"
			"_en = '''%s'''\n"
			"_ok = True\n"
			"_err = ''\n"
			"_values = []\n"
			"try:\n"
			"    _parts = _en.split('.')\n"
			"    if _parts[0] == 'unreal' and len(_parts) > 1:\n"
			"        _cls = unreal\n"
			"        for _p in _parts[1:]:\n"
			"            _cls = getattr(_cls, _p)\n"
			"    else:\n"
			"        _cls = getattr(unreal, _parts[-1])\n"
			"    try:\n"
			"        for _m in list(_cls):\n"
			"            try: _v = int(_m)\n"
			"            except Exception: _v = -1\n"
			"            _values.append({'name': str(getattr(_m,'name',_m)), 'value': _v})\n"
			"            if len(_values) >= 500: break\n"
			"    except TypeError:\n"
			"        raise TypeError(_en + ' is not iterable as an enum')\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _err = repr(_e)\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'success':_ok,'error':_err,'enum':_en,"
			"'values':_values,'count':len(_values)}, _f)\n"
		), *Safe, *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPythonService::HandleGetAssetClassForPath(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

	const FString Safe = SafePyLiteral(AssetPath);
	const FString TempFile = MakeTempJsonPath(TEXT("get_asset_class_for_path"));

	auto Task = [Safe, TempFile]() -> TSharedPtr<FJsonObject>
	{
		const FString Wrap = FString::Printf(TEXT(
			"import json\n"
			"import unreal\n"
			"_ap = '''%s'''\n"
			"_ok = True\n"
			"_err = ''\n"
			"_cls = ''\n"
			"_pkg = ''\n"
			"_exists = False\n"
			"try:\n"
			"    _eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)\n"
			"    _data = _eas.find_asset_data(_ap)\n"
			"    if _data is not None and _data.is_valid():\n"
			"        _exists = True\n"
			"        try: _cls = str(_data.asset_class_path)\n"
			"        except Exception:\n"
			"            try: _cls = str(_data.asset_class)\n"
			"            except Exception: _cls = ''\n"
			"        try: _pkg = str(_data.package_name)\n"
			"        except Exception: _pkg = ''\n"
			"except Exception as _e:\n"
			"    _ok = False\n"
			"    _err = repr(_e)\n"
			"with open(r'%s','w',encoding='utf-8') as _f:\n"
			"    json.dump({'success':_ok,'error':_err,'asset_path':_ap,"
			"'class_path':_cls,'package_name':_pkg,'exists':_exists}, _f)\n"
		), *Safe, *TempFile);

		return RunPythonAndReadJson(Wrap, TempFile);
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

// ----------------------------------------------------------------------------
// diff_against_deprecated — pure C++ scan (no Python execution)
// ----------------------------------------------------------------------------

FMCPResponse FPythonService::HandleDiffAgainstDeprecated(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
		return InvalidParams(Request.Id, TEXT("Missing params"));

	FString Snippet;
	if (!Request.Params->TryGetStringField(TEXT("snippet"), Snippet))
		return InvalidParams(Request.Id, TEXT("Missing 'snippet'"));

	const TArray<FDeprecationEntry>& Table = GetDeprecationsTable();

	TArray<FString> Lines;
	Snippet.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);

	TArray<TSharedPtr<FJsonValue>> Findings;
	for (const FDeprecationEntry& E : Table)
	{
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Lines[i].Contains(E.Deprecated))
			{
				TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("deprecated"), E.Deprecated);
				F->SetStringField(TEXT("modern"), E.Modern);
				F->SetStringField(TEXT("notes"), E.Notes);
				F->SetNumberField(TEXT("line"), i + 1);
				Findings.Add(MakeShared<FJsonValueObject>(F));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("findings"), Findings);
	Result->SetNumberField(TEXT("count"), Findings.Num());
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: diff_against_deprecated -> %d findings"), Findings.Num());
	return FMCPResponse::Success(Request.Id, Result);
}

