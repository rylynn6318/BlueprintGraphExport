#include "BlueprintGraphExportSyncRunner.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintGraphExportLibrary.h"
#include "BlueprintGraphExportLibraryInternal.h"
#include "BlueprintGraphExportPathUtils.h"
#include "BlueprintGraphExportSettings.h"
#include "BlueprintGraphExportSubsystem.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace BlueprintGraphExportSyncRunnerPrivate
{
	static FString GetDefaultSummaryPath(const UBlueprintGraphExportSettings* Settings)
	{
		return BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(
			TEXT("Saved/BlueprintGraphExport/BlueprintGraphExportRunSummary.json"),
			Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>()
		);
	}

	static FString ResolvePathValue(const FString& Value, const FString& DefaultRelativePath, const UBlueprintGraphExportSettings* Settings)
	{
		const FString EffectiveValue = Value.IsEmpty() ? DefaultRelativePath : Value;
		return BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(
			EffectiveValue,
			Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>()
		);
	}

	static FBlueprintGraphExportSyncResult MakeResult(const FBlueprintGraphExportSyncOptions& Options)
	{
		FBlueprintGraphExportSyncResult Result;
		Result.RootAssetPaths = Options.RootAssetPaths;
		Result.DocumentationRootDir = Options.DocumentationRootDir;
		Result.JsonOutputDir = Options.JsonOutputDir;
		Result.ManifestPath = Options.ManifestPath;
		Result.SummaryPath = Options.SummaryPath;
		return Result;
	}

	static void FinishResult(FBlueprintGraphExportSyncResult& Result)
	{
		Result.FinishedAtUtc = FDateTime::UtcNow();
		Result.FailedAssetCount = Result.FailedAssets.Num();
	}

	static void AddFailure(FBlueprintGraphExportSyncResult& Result, const FString& PackagePath, const FString& Error)
	{
		FBlueprintGraphExportFailedAsset Failure;
		Failure.PackagePath = PackagePath;
		Failure.Error = Error;
		Result.FailedAssets.Add(MoveTemp(Failure));
	}

	static FString StatusToString(const EBlueprintGraphExportSyncStatus Status)
	{
		switch (Status)
		{
		case EBlueprintGraphExportSyncStatus::Success:
			return TEXT("success");
		case EBlueprintGraphExportSyncStatus::SkippedUpToDate:
			return TEXT("skipped_up_to_date");
		case EBlueprintGraphExportSyncStatus::InvalidArguments:
			return TEXT("invalid_arguments");
		case EBlueprintGraphExportSyncStatus::Failed:
		default:
			return TEXT("failed");
		}
	}

	static bool SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject, const bool bPrettyPrint, FString& OutJson)
	{
		OutJson.Reset();
		if (bPrettyPrint)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
			return FJsonSerializer::Serialize(JsonObject, Writer);
		}

		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(JsonObject, Writer);
	}

	static bool SaveJsonText(const FString& JsonText, const FString& OutputPath, FString& OutError)
	{
		OutError.Reset();
		const FString Directory = FPaths::GetPath(OutputPath);
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("Failed to create JSON output directory: %s"), *Directory);
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write JSON file: %s"), *OutputPath);
			return false;
		}

		return true;
	}

	static bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJsonObject)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutJsonObject) && OutJsonObject.IsValid();
	}

	static bool EnsureAssetRegistryReady(FString& OutError)
	{
		OutError.Reset();
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		if (!AssetRegistry.IsSearchAllAssets())
		{
			AssetRegistry.SearchAllAssets(true);
		}
		if (AssetRegistry.IsLoadingAssets())
		{
			AssetRegistry.WaitForCompletion();
		}
		return true;
	}

	static bool ValidateOptions(const FBlueprintGraphExportSyncOptions& Options, FString& OutError)
	{
		OutError.Reset();
		if (Options.RootAssetPaths.IsEmpty())
		{
			OutError = TEXT("No root asset paths were configured for sync.");
			return false;
		}
		if (Options.DocumentationRootDir.IsEmpty())
		{
			OutError = TEXT("Documentation root directory resolved to an empty path.");
			return false;
		}
		if (Options.JsonOutputDir.IsEmpty())
		{
			OutError = TEXT("JSON root directory resolved to an empty path.");
			return false;
		}
		if (Options.ManifestPath.IsEmpty())
		{
			OutError = TEXT("Manifest path resolved to an empty path.");
			return false;
		}
		if (Options.SummaryPath.IsEmpty())
		{
			OutError = TEXT("Summary path resolved to an empty path.");
			return false;
		}

		for (const FString& RootPath : Options.RootAssetPaths)
		{
			FText ValidationError;
			if (!FPackageName::IsValidLongPackageName(RootPath, false, &ValidationError))
			{
				OutError = FString::Printf(TEXT("Invalid root asset path: %s (%s)"), *RootPath, *ValidationError.ToString());
				return false;
			}
		}

		return true;
	}

	static TArray<FAssetData> CollectSupportedAssetsUnderRoot(IAssetRegistry& AssetRegistry, const FString& RootAssetPath, FString& OutError)
	{
		OutError.Reset();
		TArray<FAssetData> SupportedAssets;
		if (!AssetRegistry.PathExists(FName(*RootAssetPath)))
		{
			return SupportedAssets;
		}

		TArray<FAssetData> AssetDataList;
		if (!AssetRegistry.GetAssetsByPath(FName(*RootAssetPath), AssetDataList, true, false))
		{
			OutError = FString::Printf(TEXT("Failed to enumerate assets under root asset path: %s"), *RootAssetPath);
			return SupportedAssets;
		}

		for (const FAssetData& AssetData : AssetDataList)
		{
			UObject* Asset = AssetData.FastGetAsset(false);
			if (!Asset)
			{
				Asset = AssetData.GetAsset();
			}

			if (Asset && (Cast<UBlueprint>(Asset) || Cast<UDataAsset>(Asset) || Cast<UDataTable>(Asset)))
			{
				SupportedAssets.Add(AssetData);
			}
		}

		return SupportedAssets;
	}

	static bool CollectManagedSupportedAssets(const TArray<FString>& RootAssetPaths, TArray<FAssetData>& OutAssets, FString& OutError)
	{
		OutAssets.Reset();
		OutError.Reset();

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TMap<FName, FAssetData> UniqueAssets;
		for (const FString& RootAssetPath : RootAssetPaths)
		{
			FString RootError;
			for (const FAssetData& AssetData : CollectSupportedAssetsUnderRoot(AssetRegistry, RootAssetPath, RootError))
			{
				UniqueAssets.FindOrAdd(AssetData.PackageName) = AssetData;
			}

			if (!RootError.IsEmpty())
			{
				OutError = RootError;
				return false;
			}
		}

		UniqueAssets.GenerateValueArray(OutAssets);
		OutAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			if (Left.PackageName != Right.PackageName)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			}
			return Left.AssetName.LexicalLess(Right.AssetName);
		});
		return true;
	}

	static FString GetPackageTimestampString(const FString& PackagePath)
	{
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension()))
		{
			return FString();
		}

		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Filename);
		return Timestamp != FDateTime::MinValue() ? Timestamp.ToIso8601() : FString();
	}

	static TSharedRef<FJsonObject> BuildManifestSnapshot(
		const TArray<FAssetData>& Assets,
		const FBlueprintGraphExportSyncOptions& Options,
		const FString& GeneratedAt
	)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("manifest_version"), 1);
		RootObject->SetStringField(TEXT("generated_at"), GeneratedAt);
		RootObject->SetStringField(TEXT("documentation_root"), Options.DocumentationRootDir);
		RootObject->SetStringField(TEXT("json_root"), Options.JsonOutputDir);
		RootObject->SetBoolField(TEXT("pretty_print_json"), Options.bPrettyPrintJson);
		RootObject->SetNumberField(TEXT("asset_count"), Assets.Num());

		TArray<TSharedPtr<FJsonValue>> RootPathValues;
		for (const FString& RootAssetPath : Options.RootAssetPaths)
		{
			RootPathValues.Add(MakeShared<FJsonValueString>(RootAssetPath));
		}
		RootObject->SetArrayField(TEXT("root_asset_paths"), RootPathValues);

		TArray<TSharedPtr<FJsonValue>> AssetValues;
		for (const FAssetData& AssetData : Assets)
		{
			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
			AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			AssetObject->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
			AssetObject->SetStringField(TEXT("package_timestamp"), GetPackageTimestampString(AssetData.PackageName.ToString()));
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}
		RootObject->SetArrayField(TEXT("assets"), AssetValues);
		return RootObject;
	}

	static bool GetManifestPackagePaths(const TSharedPtr<FJsonObject>& ManifestObject, TSet<FString>& OutPackagePaths)
	{
		OutPackagePaths.Reset();
		if (!ManifestObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* AssetValues = nullptr;
		if (!ManifestObject->TryGetArrayField(TEXT("assets"), AssetValues) || !AssetValues)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& AssetValue : *AssetValues)
		{
			const TSharedPtr<FJsonObject> AssetObject = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
			if (!AssetObject.IsValid())
			{
				continue;
			}

			FString PackagePath;
			if (AssetObject->TryGetStringField(TEXT("package_path"), PackagePath) && !PackagePath.IsEmpty())
			{
				OutPackagePaths.Add(PackagePath);
			}
		}

		return true;
	}

	static EBlueprintGraphExportSyncStatus EvaluateStalenessInternal(
		const FBlueprintGraphExportSyncOptions& Options,
		FBlueprintGraphExportSyncResult& Result,
		TArray<FAssetData>* OutAssets
	)
	{
		FString ValidationError;
		if (!EnsureAssetRegistryReady(ValidationError))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Failed;
			Result.Reason = ValidationError;
			return Result.Status;
		}

		if (!ValidateOptions(Options, ValidationError))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::InvalidArguments;
			Result.Reason = ValidationError;
			return Result.Status;
		}

		TArray<FAssetData> CurrentAssets;
		if (!CollectManagedSupportedAssets(Options.RootAssetPaths, CurrentAssets, ValidationError))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Failed;
			Result.Reason = ValidationError;
			return Result.Status;
		}

		Result.SupportedAssetCount = CurrentAssets.Num();
		if (OutAssets)
		{
			*OutAssets = CurrentAssets;
		}

		if (!Options.bOnlyIfStale)
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Success;
			Result.Reason = TEXT("forced by sync options");
			return Result.Status;
		}

		if (!IFileManager::Get().FileExists(*Options.ManifestPath))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Success;
			Result.Reason = TEXT("startup sync manifest is missing");
			return Result.Status;
		}

		const FString IndexPath = FPaths::Combine(FPaths::GetPath(Options.DocumentationRootDir), TEXT("AssetIndex.md"));
		if (!IFileManager::Get().FileExists(*IndexPath))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Success;
			Result.Reason = TEXT("documentation index is missing");
			return Result.Status;
		}

		for (const FAssetData& AssetData : CurrentAssets)
		{
			FString RelativePath = AssetData.PackageName.ToString();
			RelativePath.RemoveFromStart(TEXT("/"));
			const FString MarkdownPath = FPaths::Combine(Options.DocumentationRootDir, RelativePath + TEXT(".md"));
			const FString JsonPath = FPaths::Combine(Options.JsonOutputDir, RelativePath + TEXT(".json"));
			if (!IFileManager::Get().FileExists(*MarkdownPath) || !IFileManager::Get().FileExists(*JsonPath))
			{
				Result.Status = EBlueprintGraphExportSyncStatus::Success;
				Result.Reason = FString::Printf(TEXT("documentation mirror is missing for %s"), *AssetData.PackageName.ToString());
				return Result.Status;
			}
		}

		TSharedPtr<FJsonObject> ExistingManifest;
		if (!LoadJsonObjectFromFile(Options.ManifestPath, ExistingManifest) || !ExistingManifest.IsValid())
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Success;
			Result.Reason = TEXT("startup sync manifest could not be parsed");
			return Result.Status;
		}

		const TSharedRef<FJsonObject> CurrentSnapshot = BuildManifestSnapshot(CurrentAssets, Options, TEXT(""));
		ExistingManifest->SetStringField(TEXT("generated_at"), TEXT(""));

		FString CurrentJson;
		FString ExistingJson;
		if (!SerializeJsonObject(CurrentSnapshot, false, CurrentJson) || !SerializeJsonObject(ExistingManifest.ToSharedRef(), false, ExistingJson))
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Failed;
			Result.Reason = TEXT("startup sync manifest comparison failed");
			return Result.Status;
		}

		if (CurrentJson != ExistingJson)
		{
			Result.Status = EBlueprintGraphExportSyncStatus::Success;
			Result.Reason = TEXT("startup sync manifest differs from current asset state");
			return Result.Status;
		}

		Result.Status = EBlueprintGraphExportSyncStatus::SkippedUpToDate;
		Result.Reason = TEXT("startup sync manifest is current");
		return Result.Status;
	}

	static bool RemoveOrphanedDocumentationBundles(
		const FBlueprintGraphExportSyncOptions& Options,
		const TArray<FAssetData>& CurrentAssets,
		FBlueprintGraphExportSyncResult& Result,
		FString& OutError
	)
	{
		OutError.Reset();
		if (!IFileManager::Get().FileExists(*Options.ManifestPath))
		{
			return true;
		}

		TSharedPtr<FJsonObject> ExistingManifest;
		if (!LoadJsonObjectFromFile(Options.ManifestPath, ExistingManifest) || !ExistingManifest.IsValid())
		{
			OutError = FString::Printf(TEXT("Startup sync manifest could not be parsed for orphan cleanup: %s"), *Options.ManifestPath);
			return false;
		}

		TSet<FString> ManifestPackagePaths;
		if (!GetManifestPackagePaths(ExistingManifest, ManifestPackagePaths))
		{
			OutError = FString::Printf(TEXT("Startup sync manifest is missing asset package data for orphan cleanup: %s"), *Options.ManifestPath);
			return false;
		}

		TSet<FString> CurrentPackagePaths;
		for (const FAssetData& AssetData : CurrentAssets)
		{
			CurrentPackagePaths.Add(AssetData.PackageName.ToString());
		}

		FString FirstError;
		for (const FString& PackagePath : ManifestPackagePaths)
		{
			if (CurrentPackagePaths.Contains(PackagePath))
			{
				continue;
			}

			++Result.OrphanedAssetCount;

			FString RemovedMarkdownPath;
			FString RemovedJsonPath;
			FString Error;
			if (!UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
				PackagePath,
				Options.DocumentationRootDir,
				Options.JsonOutputDir,
				RemovedMarkdownPath,
				RemovedJsonPath,
				Error
			))
			{
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
					AddFailure(Result, PackagePath, Error);
					if (FirstError.IsEmpty())
					{
						FirstError = Error;
					}
				}
			}
		}

		if (!FirstError.IsEmpty())
		{
			OutError = FirstError;
			return false;
		}

		return true;
	}

	static bool ExportAssets(
		const FBlueprintGraphExportSyncOptions& Options,
		const TArray<FAssetData>& Assets,
		FBlueprintGraphExportSyncResult& Result,
		FString& OutError
	)
	{
		OutError.Reset();
		FString FirstError;
		for (const FAssetData& AssetData : Assets)
		{
			UObject* Asset = AssetData.FastGetAsset(false);
			if (!Asset)
			{
				Asset = AssetData.GetAsset();
			}

			const FString PackagePath = AssetData.PackageName.ToString();
			if (!Asset)
			{
				const FString Error = FString::Printf(TEXT("Failed to load asset for documentation export: %s"), *PackagePath);
				UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
				AddFailure(Result, PackagePath, Error);
				if (FirstError.IsEmpty())
				{
					FirstError = Error;
				}
				continue;
			}

			FString MarkdownPath;
			FString JsonPath;
			FString Error;
			if (!UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
				Asset,
				Options.DocumentationRootDir,
				Options.JsonOutputDir,
				Options.bPrettyPrintJson,
				MarkdownPath,
				JsonPath,
				Error
			))
			{
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
					AddFailure(Result, PackagePath, Error);
					if (FirstError.IsEmpty())
					{
						FirstError = Error;
					}
				}
				continue;
			}

			++Result.ExportedAssetCount;
		}

		if (!FirstError.IsEmpty())
		{
			OutError = FirstError;
			return false;
		}

		return true;
	}

	static bool WriteManifest(
		const FBlueprintGraphExportSyncOptions& Options,
		const TArray<FAssetData>& Assets,
		FString& OutError
	)
	{
		FString ManifestJson;
		if (!SerializeJsonObject(BuildManifestSnapshot(Assets, Options, FDateTime::UtcNow().ToIso8601()), false, ManifestJson))
		{
			OutError = TEXT("Failed to serialize startup sync manifest.");
			return false;
		}

		return SaveJsonText(ManifestJson, Options.ManifestPath, OutError);
	}

	static TSharedRef<FJsonObject> BuildSummaryJsonObject(const FBlueprintGraphExportSyncResult& Result)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetStringField(TEXT("status"), StatusToString(Result.Status));
		RootObject->SetStringField(TEXT("reason"), Result.Reason);
		RootObject->SetStringField(TEXT("documentation_root"), Result.DocumentationRootDir);
		RootObject->SetStringField(TEXT("json_root"), Result.JsonOutputDir);
		RootObject->SetStringField(TEXT("manifest_path"), Result.ManifestPath);
		RootObject->SetStringField(TEXT("summary_path"), Result.SummaryPath);
		RootObject->SetStringField(TEXT("index_path"), Result.IndexPath);
		RootObject->SetNumberField(TEXT("supported_asset_count"), Result.SupportedAssetCount);
		RootObject->SetNumberField(TEXT("exported_asset_count"), Result.ExportedAssetCount);
		RootObject->SetNumberField(TEXT("failed_asset_count"), Result.FailedAssetCount);
		RootObject->SetNumberField(TEXT("orphaned_asset_count"), Result.OrphanedAssetCount);
		RootObject->SetStringField(TEXT("started_at_utc"), Result.StartedAtUtc.ToIso8601());
		RootObject->SetStringField(TEXT("finished_at_utc"), Result.FinishedAtUtc.ToIso8601());
		RootObject->SetNumberField(TEXT("duration_seconds"), Result.GetDurationSeconds());

		TArray<TSharedPtr<FJsonValue>> RootValues;
		for (const FString& RootPath : Result.RootAssetPaths)
		{
			RootValues.Add(MakeShared<FJsonValueString>(RootPath));
		}
		RootObject->SetArrayField(TEXT("roots"), RootValues);

		TArray<TSharedPtr<FJsonValue>> FailedAssetValues;
		for (const FBlueprintGraphExportFailedAsset& FailedAsset : Result.FailedAssets)
		{
			TSharedPtr<FJsonObject> FailedAssetObject = MakeShared<FJsonObject>();
			FailedAssetObject->SetStringField(TEXT("package_path"), FailedAsset.PackagePath);
			FailedAssetObject->SetStringField(TEXT("error"), FailedAsset.Error);
			FailedAssetValues.Add(MakeShared<FJsonValueObject>(FailedAssetObject));
		}
		RootObject->SetArrayField(TEXT("failed_assets"), FailedAssetValues);

		return RootObject;
	}
}

double FBlueprintGraphExportSyncResult::GetDurationSeconds() const
{
	if (StartedAtUtc == FDateTime::MinValue() || FinishedAtUtc == FDateTime::MinValue())
	{
		return 0.0;
	}

	return (FinishedAtUtc - StartedAtUtc).GetTotalSeconds();
}

FBlueprintGraphExportSyncOptions FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(const UBlueprintGraphExportSettings* Settings)
{
	const UBlueprintGraphExportSettings* EffectiveSettings = Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>();

	FBlueprintGraphExportSyncOptions Options;
	Options.RootAssetPaths = EffectiveSettings ? EffectiveSettings->RootAssetPaths : TArray<FString>{ TEXT("/Game") };
	if (Options.RootAssetPaths.IsEmpty())
	{
		Options.RootAssetPaths = { TEXT("/Game") };
	}

	Options.DocumentationRootDir = BlueprintGraphExportSyncRunnerPrivate::ResolvePathValue(
		EffectiveSettings ? EffectiveSettings->DocumentationRootDir : FString(),
		TEXT("Saved/BlueprintGraphExport/Docs"),
		EffectiveSettings
	);
	Options.JsonOutputDir = BlueprintGraphExportSyncRunnerPrivate::ResolvePathValue(
		EffectiveSettings ? EffectiveSettings->JsonOutputDir : FString(),
		TEXT("Saved/BlueprintGraphExport/Json"),
		EffectiveSettings
	);
	Options.ManifestPath = BlueprintGraphExportSyncRunnerPrivate::ResolvePathValue(
		EffectiveSettings ? EffectiveSettings->StartupSyncManifestPath : FString(),
		TEXT("Saved/BlueprintGraphExport/StartupSyncManifest.json"),
		EffectiveSettings
	);
	Options.SummaryPath = BlueprintGraphExportSyncRunnerPrivate::GetDefaultSummaryPath(EffectiveSettings);
	Options.bPrettyPrintJson = EffectiveSettings ? EffectiveSettings->bPrettyPrintJson : true;
	return Options;
}

FBlueprintGraphExportSyncResult FBlueprintGraphExportSyncRunner::EvaluateStaleness(const FBlueprintGraphExportSyncOptions& Options)
{
	FBlueprintGraphExportSyncResult Result = BlueprintGraphExportSyncRunnerPrivate::MakeResult(Options);
	Result.StartedAtUtc = FDateTime::UtcNow();
	BlueprintGraphExportSyncRunnerPrivate::EvaluateStalenessInternal(Options, Result, nullptr);
	BlueprintGraphExportSyncRunnerPrivate::FinishResult(Result);
	return Result;
}

FBlueprintGraphExportSyncResult FBlueprintGraphExportSyncRunner::RunFullSync(const FBlueprintGraphExportSyncOptions& Options)
{
	FBlueprintGraphExportSyncResult Result = BlueprintGraphExportSyncRunnerPrivate::MakeResult(Options);
	Result.StartedAtUtc = FDateTime::UtcNow();

	TArray<FAssetData> CurrentAssets;
	BlueprintGraphExportSyncRunnerPrivate::EvaluateStalenessInternal(Options, Result, &CurrentAssets);
	if (Result.Status != EBlueprintGraphExportSyncStatus::Success)
	{
		BlueprintGraphExportSyncRunnerPrivate::FinishResult(Result);
		return Result;
	}

	FString FirstFailure;
	FString CleanupError;
	if (!BlueprintGraphExportSyncRunnerPrivate::RemoveOrphanedDocumentationBundles(Options, CurrentAssets, Result, CleanupError) && FirstFailure.IsEmpty())
	{
		FirstFailure = CleanupError;
	}

	FString ExportError;
	if (!BlueprintGraphExportSyncRunnerPrivate::ExportAssets(Options, CurrentAssets, Result, ExportError) && FirstFailure.IsEmpty())
	{
		FirstFailure = ExportError;
	}

	FString IndexError;
	if (!BlueprintGraphExportInternal::RebuildDocumentationIndexForRoots(
		Options.RootAssetPaths,
		Options.DocumentationRootDir,
		Result.IndexPath,
		IndexError
	))
	{
		if (FirstFailure.IsEmpty())
		{
			FirstFailure = IndexError;
		}
	}

	if (!FirstFailure.IsEmpty())
	{
		Result.Status = EBlueprintGraphExportSyncStatus::Failed;
		Result.Reason = FirstFailure;
		BlueprintGraphExportSyncRunnerPrivate::FinishResult(Result);
		return Result;
	}

	FString ManifestError;
	if (!BlueprintGraphExportSyncRunnerPrivate::WriteManifest(Options, CurrentAssets, ManifestError))
	{
		Result.Status = EBlueprintGraphExportSyncStatus::Failed;
		Result.Reason = ManifestError;
		BlueprintGraphExportSyncRunnerPrivate::FinishResult(Result);
		return Result;
	}

	Result.Status = EBlueprintGraphExportSyncStatus::Success;
	Result.ManifestPath = Options.ManifestPath;
	Result.Reason = Result.Reason.IsEmpty() ? TEXT("full sync completed") : Result.Reason;
	BlueprintGraphExportSyncRunnerPrivate::FinishResult(Result);
	return Result;
}

bool FBlueprintGraphExportSyncRunner::RefreshManifest(
	const FBlueprintGraphExportSyncOptions& Options,
	FString& OutManifestPath,
	int32& OutSupportedAssetCount,
	FString& OutError
)
{
	OutManifestPath = Options.ManifestPath;
	OutSupportedAssetCount = 0;
	OutError.Reset();

	FString ValidationError;
	if (!BlueprintGraphExportSyncRunnerPrivate::EnsureAssetRegistryReady(ValidationError))
	{
		OutError = ValidationError;
		return false;
	}

	if (!BlueprintGraphExportSyncRunnerPrivate::ValidateOptions(Options, ValidationError))
	{
		OutError = ValidationError;
		return false;
	}

	TArray<FAssetData> Assets;
	if (!BlueprintGraphExportSyncRunnerPrivate::CollectManagedSupportedAssets(Options.RootAssetPaths, Assets, OutError))
	{
		return false;
	}

	OutSupportedAssetCount = Assets.Num();
	return BlueprintGraphExportSyncRunnerPrivate::WriteManifest(Options, Assets, OutError);
}

bool FBlueprintGraphExportSyncRunner::WriteSummary(const FBlueprintGraphExportSyncResult& Result, const bool bPrettyPrintJson, FString& OutError)
{
	FString SummaryJson;
	if (!BlueprintGraphExportSyncRunnerPrivate::SerializeJsonObject(
		BlueprintGraphExportSyncRunnerPrivate::BuildSummaryJsonObject(Result),
		bPrettyPrintJson,
		SummaryJson
	))
	{
		OutError = TEXT("Failed to serialize summary JSON.");
		return false;
	}

	return BlueprintGraphExportSyncRunnerPrivate::SaveJsonText(SummaryJson, Result.SummaryPath, OutError);
}
