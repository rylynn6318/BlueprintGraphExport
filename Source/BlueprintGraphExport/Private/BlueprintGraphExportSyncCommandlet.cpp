#include "BlueprintGraphExportSyncCommandlet.h"

#include "BlueprintGraphExportPathUtils.h"
#include "BlueprintGraphExportSettings.h"
#include "BlueprintGraphExportSubsystem.h"
#include "BlueprintGraphExportSyncRunner.h"
#include "Misc/Parse.h"

namespace BlueprintGraphExportSyncCommandlet
{
	static int32 GetExitCode(const EBlueprintGraphExportSyncStatus Status)
	{
		switch (Status)
		{
		case EBlueprintGraphExportSyncStatus::Success:
		case EBlueprintGraphExportSyncStatus::SkippedUpToDate:
			return 0;
		case EBlueprintGraphExportSyncStatus::InvalidArguments:
			return 2;
		case EBlueprintGraphExportSyncStatus::Failed:
		default:
			return 1;
		}
	}

	static FString ResolveOverridePath(const FString& Value, const UBlueprintGraphExportSettings* Settings)
	{
		return BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(Value, Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>());
	}

	static bool ParseRootsOverride(const FString& RawValue, TArray<FString>& OutRoots)
	{
		OutRoots.Reset();
		TArray<FString> ParsedValues;
		RawValue.ParseIntoArray(ParsedValues, TEXT(","), true);
		for (FString RootPath : ParsedValues)
		{
			RootPath.TrimStartAndEndInline();
			if (!RootPath.IsEmpty())
			{
				OutRoots.AddUnique(RootPath);
			}
		}

		return !OutRoots.IsEmpty();
	}

	static FBlueprintGraphExportSyncResult MakeInvalidArgumentsResult(
		const FBlueprintGraphExportSyncOptions& Options,
		const FString& Reason
	)
	{
		FBlueprintGraphExportSyncResult Result;
		Result.Status = EBlueprintGraphExportSyncStatus::InvalidArguments;
		Result.Reason = Reason;
		Result.RootAssetPaths = Options.RootAssetPaths;
		Result.DocumentationRootDir = Options.DocumentationRootDir;
		Result.JsonOutputDir = Options.JsonOutputDir;
		Result.ManifestPath = Options.ManifestPath;
		Result.SummaryPath = Options.SummaryPath;
		Result.StartedAtUtc = FDateTime::UtcNow();
		Result.FinishedAtUtc = Result.StartedAtUtc;
		return Result;
	}
}

UBlueprintGraphExportSyncCommandlet::UBlueprintGraphExportSyncCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
	HelpDescription = TEXT("Runs BlueprintGraphExport documentation sync in headless mode.");
}

int32 UBlueprintGraphExportSyncCommandlet::Main(const FString& Params)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	FBlueprintGraphExportSyncOptions Options = FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings);

	FString RootsValue;
	if (FParse::Value(*Params, TEXT("Roots="), RootsValue))
	{
		TArray<FString> ParsedRoots;
		if (!BlueprintGraphExportSyncCommandlet::ParseRootsOverride(RootsValue, ParsedRoots))
		{
			FBlueprintGraphExportSyncResult Result = BlueprintGraphExportSyncCommandlet::MakeInvalidArgumentsResult(
				Options,
				TEXT("The -Roots argument must include at least one valid long package path.")
			);
			FString SummaryError;
			if (!FBlueprintGraphExportSyncRunner::WriteSummary(Result, Options.bPrettyPrintJson, SummaryError) && !SummaryError.IsEmpty())
			{
				UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExportSync summary write failed: %s"), *SummaryError);
			}
			return BlueprintGraphExportSyncCommandlet::GetExitCode(Result.Status);
		}
		Options.RootAssetPaths = MoveTemp(ParsedRoots);
	}

	FString Value;
	if (FParse::Value(*Params, TEXT("DocsRoot="), Value))
	{
		Options.DocumentationRootDir = BlueprintGraphExportSyncCommandlet::ResolveOverridePath(Value, Settings);
	}
	if (FParse::Value(*Params, TEXT("JsonRoot="), Value))
	{
		Options.JsonOutputDir = BlueprintGraphExportSyncCommandlet::ResolveOverridePath(Value, Settings);
	}
	if (FParse::Value(*Params, TEXT("ManifestPath="), Value))
	{
		Options.ManifestPath = BlueprintGraphExportSyncCommandlet::ResolveOverridePath(Value, Settings);
	}
	if (FParse::Value(*Params, TEXT("SummaryPath="), Value))
	{
		Options.SummaryPath = BlueprintGraphExportSyncCommandlet::ResolveOverridePath(Value, Settings);
	}

	const bool bOnlyIfStale = FParse::Param(*Params, TEXT("OnlyIfStale"));
	const bool bForce = FParse::Param(*Params, TEXT("Force"));
	const bool bPrettyJson = FParse::Param(*Params, TEXT("PrettyJson"));
	const bool bCompactJson = FParse::Param(*Params, TEXT("CompactJson"));

	FBlueprintGraphExportSyncResult Result;
	if (bPrettyJson && bCompactJson)
	{
		Result = BlueprintGraphExportSyncCommandlet::MakeInvalidArgumentsResult(
			Options,
			TEXT("Use either -PrettyJson or -CompactJson, not both.")
		);
	}
	else
	{
		if (bPrettyJson)
		{
			Options.bPrettyPrintJson = true;
		}
		if (bCompactJson)
		{
			Options.bPrettyPrintJson = false;
		}

		Options.bOnlyIfStale = bOnlyIfStale && !bForce;
		Result = FBlueprintGraphExportSyncRunner::RunFullSync(Options);
	}

	FString SummaryError;
	if (!FBlueprintGraphExportSyncRunner::WriteSummary(Result, Options.bPrettyPrintJson, SummaryError) && !SummaryError.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExportSync summary write failed: %s"), *SummaryError);
	}

	switch (Result.Status)
	{
	case EBlueprintGraphExportSyncStatus::Success:
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("BlueprintGraphExportSync succeeded: %s"), *Result.Reason);
		break;
	case EBlueprintGraphExportSyncStatus::SkippedUpToDate:
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("BlueprintGraphExportSync skipped: %s"), *Result.Reason);
		break;
	case EBlueprintGraphExportSyncStatus::InvalidArguments:
		UE_LOG(LogBlueprintGraphExportSubsystem, Error, TEXT("BlueprintGraphExportSync invalid arguments: %s"), *Result.Reason);
		break;
	case EBlueprintGraphExportSyncStatus::Failed:
	default:
		UE_LOG(LogBlueprintGraphExportSubsystem, Error, TEXT("BlueprintGraphExportSync failed: %s"), *Result.Reason);
		break;
	}

	return BlueprintGraphExportSyncCommandlet::GetExitCode(Result.Status);
}
