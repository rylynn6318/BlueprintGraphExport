#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Misc/DateTime.h"

class UBlueprintGraphExportSettings;

enum class EBlueprintGraphExportSyncStatus : uint8
{
	Success,
	SkippedUpToDate,
	InvalidArguments,
	Failed
};

struct FBlueprintGraphExportFailedAsset
{
	FString PackagePath;
	FString Error;
};

struct FBlueprintGraphExportSyncOptions
{
	TArray<FString> RootAssetPaths;
	FString DocumentationRootDir;
	FString JsonOutputDir;
	FString ManifestPath;
	FString SummaryPath;
	bool bOnlyIfStale = false;
	bool bPrettyPrintJson = true;
};

struct FBlueprintGraphExportSyncResult
{
	EBlueprintGraphExportSyncStatus Status = EBlueprintGraphExportSyncStatus::Failed;
	FString Reason;
	TArray<FString> RootAssetPaths;
	FString DocumentationRootDir;
	FString JsonOutputDir;
	FString ManifestPath;
	FString SummaryPath;
	int32 SupportedAssetCount = 0;
	int32 ExportedAssetCount = 0;
	int32 FailedAssetCount = 0;
	int32 OrphanedAssetCount = 0;
	TArray<FBlueprintGraphExportFailedAsset> FailedAssets;
	FString IndexPath;
	FDateTime StartedAtUtc;
	FDateTime FinishedAtUtc;

	double GetDurationSeconds() const;
};

class FBlueprintGraphExportSyncRunner
{
public:
	static FBlueprintGraphExportSyncOptions MakeOptionsFromSettings(const UBlueprintGraphExportSettings* Settings = nullptr);
	static FBlueprintGraphExportSyncResult EvaluateStaleness(const FBlueprintGraphExportSyncOptions& Options);
	static FBlueprintGraphExportSyncResult RunFullSync(const FBlueprintGraphExportSyncOptions& Options);
	static bool RefreshManifest(
		const FBlueprintGraphExportSyncOptions& Options,
		FString& OutManifestPath,
		int32& OutSupportedAssetCount,
		FString& OutError
	);
	static bool WriteSummary(const FBlueprintGraphExportSyncResult& Result, const bool bPrettyPrintJson, FString& OutError);
};
