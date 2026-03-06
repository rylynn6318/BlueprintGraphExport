#include "BlueprintGraphExportSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintGraphExportLibrary.h"
#include "BlueprintGraphExportLibraryInternal.h"
#include "BlueprintGraphExportSettings.h"
#include "BlueprintGraphExportSyncRunner.h"
#include "BlueprintGraphExportPathUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY(LogBlueprintGraphExportSubsystem);

namespace BlueprintGraphExportSubsystem
{
	static FString ResolveDirectorySetting(const FString& DirectorySetting, const FString& FallbackAbsolutePath = FString())
	{
		const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
		return DirectorySetting.IsEmpty()
			? FallbackAbsolutePath
			: BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(DirectorySetting, Settings);
	}

	static FString ResolveFileSetting(const FString& FileSetting, const FString& FallbackAbsolutePath)
	{
		const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
		return FileSetting.IsEmpty()
			? FallbackAbsolutePath
			: BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(FileSetting, Settings);
	}

	static FString GetDefaultDocumentationRootDir()
	{
		return BlueprintGraphExportPathUtils::GetDocumentationRootDir();
	}

	static FString GetDefaultJsonOutputDir()
	{
		return BlueprintGraphExportPathUtils::GetJsonOutputDir();
	}

	static FString GetDefaultManifestPath()
	{
		return BlueprintGraphExportPathUtils::GetManifestPath();
	}

	static FString GetRelativeMirrorPath(const FString& AssetPackagePath, const TCHAR* Extension)
	{
		FString RelativePath = AssetPackagePath;
		RelativePath.RemoveFromStart(TEXT("/"));
		return RelativePath + Extension;
	}

	static FString GetMarkdownPathForAsset(const FString& AssetPackagePath, const FString& DocumentationRootDir)
	{
		return FPaths::Combine(DocumentationRootDir, GetRelativeMirrorPath(AssetPackagePath, TEXT(".md")));
	}

	static FString GetJsonPathForAsset(const FString& AssetPackagePath, const FString& JsonOutputDir)
	{
		return FPaths::Combine(JsonOutputDir, GetRelativeMirrorPath(AssetPackagePath, TEXT(".json")));
	}

	static FString GetIndexPath(const FString& DocumentationRootDir)
	{
		return FPaths::Combine(FPaths::GetPath(DocumentationRootDir), TEXT("AssetIndex.md"));
	}

	static bool IsSupportedAsset(UObject* Asset)
	{
		return Asset && (Cast<UBlueprint>(Asset) || Cast<UDataAsset>(Asset));
	}

	static TArray<FAssetData> CollectSupportedAssetsUnderRoot(
		IAssetRegistry& AssetRegistry,
		const FString& RootAssetPath,
		const bool bRecursive = true,
		FString* OutError = nullptr
	)
	{
		TArray<FAssetData> SupportedAssets;
		if (OutError)
		{
			OutError->Reset();
		}

		FText ValidationError;
		if (!FPackageName::IsValidLongPackageName(RootAssetPath, false, &ValidationError))
		{
			const FString Error = FString::Printf(TEXT("Invalid root asset path for startup sync: %s (%s)"), *RootAssetPath, *ValidationError.ToString());
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
			if (OutError)
			{
				*OutError = Error;
			}
			return SupportedAssets;
		}

		if (!AssetRegistry.PathExists(FName(*RootAssetPath)))
		{
			const FString Error = FString::Printf(TEXT("Startup sync root asset path does not exist: %s"), *RootAssetPath);
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
			if (OutError)
			{
				*OutError = Error;
			}
			return SupportedAssets;
		}

		TArray<FAssetData> AssetDataList;
		if (!AssetRegistry.GetAssetsByPath(FName(*RootAssetPath), AssetDataList, bRecursive, false))
		{
			const FString Error = FString::Printf(TEXT("Failed to enumerate assets under startup sync root path: %s"), *RootAssetPath);
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
			if (OutError)
			{
				*OutError = Error;
			}
			return SupportedAssets;
		}

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (UObject* Asset = AssetData.GetAsset())
			{
				if (IsSupportedAsset(Asset))
				{
					SupportedAssets.Add(AssetData);
				}
			}
		}

		return SupportedAssets;
	}

	static TArray<FAssetData> CollectManagedSupportedAssets(const TArray<FString>& RootAssetPaths)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TMap<FName, FAssetData> UniqueAssets;
		for (const FString& RootAssetPath : RootAssetPaths)
		{
			for (const FAssetData& AssetData : CollectSupportedAssetsUnderRoot(AssetRegistry, RootAssetPath))
			{
				UniqueAssets.FindOrAdd(AssetData.PackageName) = AssetData;
			}
		}

		TArray<FAssetData> Result;
		UniqueAssets.GenerateValueArray(Result);
		Result.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			if (Left.PackageName != Right.PackageName)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			}
			return Left.AssetName.LexicalLess(Right.AssetName);
		});
		return Result;
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
		const TArray<FString>& RootAssetPaths,
		const FString& DocumentationRootDir,
		const FString& JsonOutputDir,
		const bool bPrettyPrintJson,
		const FString& GeneratedAt
	)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("manifest_version"), 1);
		RootObject->SetStringField(TEXT("generated_at"), GeneratedAt);
		RootObject->SetStringField(TEXT("documentation_root"), DocumentationRootDir);
		RootObject->SetStringField(TEXT("json_root"), JsonOutputDir);
		RootObject->SetBoolField(TEXT("pretty_print_json"), bPrettyPrintJson);
		RootObject->SetNumberField(TEXT("asset_count"), Assets.Num());

		TArray<TSharedPtr<FJsonValue>> RootPathValues;
		for (const FString& RootAssetPath : RootAssetPaths)
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

	static bool SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject, FString& OutJson)
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(JsonObject, Writer);
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

	static bool SaveJsonText(const FString& JsonText, const FString& OutputPath, FString& OutError)
	{
		const FString Directory = FPaths::GetPath(OutputPath);
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("Failed to create startup sync manifest directory: %s"), *Directory);
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write startup sync manifest: %s"), *OutputPath);
			return false;
		}

		return true;
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

	static bool ExportAssetsUnderRoot(
		const FString& RootAssetPath,
		const FString& DocumentationRootDir,
		const FString& JsonOutputDir,
		const bool bRecursive,
		const bool bPrettyPrintJson,
		FString& OutError
	)
	{
		OutError.Reset();

		if (!IFileManager::Get().MakeDirectory(*DocumentationRootDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create documentation root directory: %s"), *DocumentationRootDir);
			return false;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FString RootError;
		const TArray<FAssetData> AssetDataList = CollectSupportedAssetsUnderRoot(AssetRegistry, RootAssetPath, bRecursive, &RootError);
		if (!RootError.IsEmpty())
		{
			OutError = RootError;
			return false;
		}

		bool bSucceeded = true;
		FString FirstError;
		for (const FAssetData& AssetData : AssetDataList)
		{
			UObject* Asset = AssetData.FastGetAsset(false);
			if (!Asset)
			{
				Asset = AssetData.GetAsset();
			}

			if (!Asset)
			{
				const FString Error = FString::Printf(TEXT("Failed to load asset for documentation export: %s"), *AssetData.PackageName.ToString());
				UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
				bSucceeded = false;
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
				DocumentationRootDir,
				JsonOutputDir,
				bPrettyPrintJson,
				MarkdownPath,
				JsonPath,
				Error
			))
			{
				bSucceeded = false;
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
					if (FirstError.IsEmpty())
					{
						FirstError = Error;
					}
				}
			}
		}

		if (!bSucceeded)
		{
			OutError = FirstError.IsEmpty()
				? FString::Printf(TEXT("One or more assets under %s failed to export."), *RootAssetPath)
				: FirstError;
		}
		return bSucceeded;
	}

	static bool RemoveOrphanedDocumentationBundles(
		const TArray<FAssetData>& CurrentAssets,
		const FString& DocumentationRootDir,
		const FString& JsonOutputDir,
		const FString& ManifestPath,
		FString& OutError
	)
	{
		OutError.Reset();
		if (!IFileManager::Get().FileExists(*ManifestPath))
		{
			return true;
		}

		TSharedPtr<FJsonObject> ExistingManifest;
		if (!LoadJsonObjectFromFile(ManifestPath, ExistingManifest) || !ExistingManifest.IsValid())
		{
			OutError = FString::Printf(TEXT("Startup sync manifest could not be parsed for orphan cleanup: %s"), *ManifestPath);
			return false;
		}

		TSet<FString> ManifestPackagePaths;
		if (!GetManifestPackagePaths(ExistingManifest, ManifestPackagePaths))
		{
			OutError = FString::Printf(TEXT("Startup sync manifest is missing asset package data for orphan cleanup: %s"), *ManifestPath);
			return false;
		}

		TSet<FString> CurrentPackagePaths;
		for (const FAssetData& AssetData : CurrentAssets)
		{
			CurrentPackagePaths.Add(AssetData.PackageName.ToString());
		}

		bool bSucceeded = true;
		FString FirstError;
		for (const FString& PackagePath : ManifestPackagePaths)
		{
			if (CurrentPackagePaths.Contains(PackagePath))
			{
				continue;
			}

			FString RemovedMarkdownPath;
			FString RemovedJsonPath;
			FString Error;
			if (!UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
				PackagePath,
				DocumentationRootDir,
				JsonOutputDir,
				RemovedMarkdownPath,
				RemovedJsonPath,
				Error
			))
			{
				bSucceeded = false;
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
					if (FirstError.IsEmpty())
					{
						FirstError = Error;
					}
				}
			}
		}

		if (!bSucceeded)
		{
			OutError = FirstError.IsEmpty()
				? FString::Printf(TEXT("Failed to remove one or more orphaned documentation bundles from %s"), *ManifestPath)
				: FirstError;
		}
		return bSucceeded;
	}
}

void UBlueprintGraphExportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (IsRunningCommandlet())
	{
		return;
	}

	UPackage::PackageSavedWithContextEvent.AddUObject(this, &UBlueprintGraphExportSubsystem::HandlePackageSaved);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddUObject(this, &UBlueprintGraphExportSubsystem::HandleAssetRemoved);
	AssetRenamedHandle = AssetRegistry.OnAssetRenamed().AddUObject(this, &UBlueprintGraphExportSubsystem::HandleAssetRenamed);

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (Settings && Settings->bEnableStartupFullSync)
	{
		if (AssetRegistry.IsLoadingAssets())
		{
			FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddUObject(this, &UBlueprintGraphExportSubsystem::HandleAssetRegistryFilesLoaded);
		}
		else
		{
			ScheduleStartupSync();
		}
	}
}

void UBlueprintGraphExportSubsystem::Deinitialize()
{
	UPackage::PackageSavedWithContextEvent.RemoveAll(this);

	CancelScheduledStartupSync();


	if (FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		IAssetRegistry& AssetRegistry = AssetRegistryModule->Get();
		if (AssetRemovedHandle.IsValid())
		{
			AssetRegistry.OnAssetRemoved().Remove(AssetRemovedHandle);
		}
		if (AssetRenamedHandle.IsValid())
		{
			AssetRegistry.OnAssetRenamed().Remove(AssetRenamedHandle);
		}
		if (FilesLoadedHandle.IsValid())
		{
			AssetRegistry.OnFilesLoaded().Remove(FilesLoadedHandle);
		}
	}

	Super::Deinitialize();
}

bool UBlueprintGraphExportSubsystem::RunManualFullSync(FString& OutReasonOrError)
{
	OutReasonOrError.Reset();

	if (bStartupSyncInProgress)
	{
		OutReasonOrError = TEXT("documentation sync is already running");
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Manual documentation sync skipped: %s"), *OutReasonOrError);
		return true;
	}

	CancelScheduledStartupSync();
	bStartupSyncInProgress = true;
	const FBlueprintGraphExportSyncResult Result = FBlueprintGraphExportSyncRunner::RunFullSync(
		FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings()
	);
	bStartupSyncInProgress = false;

	if (Result.Status == EBlueprintGraphExportSyncStatus::Success)
	{
		OutReasonOrError = FString::Printf(
			TEXT("Manual documentation sync completed for %d supported assets. Index: %s Manifest: %s"),
			Result.SupportedAssetCount,
			*Result.IndexPath,
			*Result.ManifestPath
		);
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("%s"), *OutReasonOrError);
		return true;
	}

	if (!Result.Reason.IsEmpty())
	{
		OutReasonOrError = Result.Reason;
	}
	else
	{
		OutReasonOrError = TEXT("Manual documentation sync completed with errors. See previous log entries.");
	}
	UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *OutReasonOrError);
	return false;
}

bool UBlueprintGraphExportSubsystem::IsManagedPackagePath(const FString& PackagePath) const
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings)
	{
		return false;
	}

	for (const FString& RootPath : Settings->RootAssetPaths)
	{
		if (PackagePath == RootPath)
		{
			return true;
		}

		const FString Prefix = RootPath.EndsWith(TEXT("/")) ? RootPath : RootPath + TEXT("/");
		if (PackagePath.StartsWith(Prefix))
		{
			return true;
		}
	}

	return false;
}

void UBlueprintGraphExportSubsystem::HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext SaveContext)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Package || !Settings || !Settings->bEnableAutoExportOnSave || bStartupSyncInProgress || !IsManagedPackagePath(Package->GetName()))
	{
		return;
	}

	FString ExportError;
	if (!ExportManagedAssetsInPackage(Package, ExportError))
	{
		if (!ExportError.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *ExportError);
		}
		return;
	}

	FString IndexPath;
	FString IndexError;
	if (!RebuildIndex(IndexPath, IndexError))
	{
		if (!IndexError.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *IndexError);
		}
		return;
	}

	FString ManifestPath;
	int32 SupportedAssetCount = 0;
	FString ManifestError;
	if (!RefreshStartupSyncManifest(ManifestPath, SupportedAssetCount, ManifestError) && !ManifestError.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *ManifestError);
	}
}

void UBlueprintGraphExportSubsystem::HandleAssetRemoved(const FAssetData& AssetData)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Settings->bEnableAutoExportOnSave || bStartupSyncInProgress)
	{
		return;
	}

	const FString PackagePath = AssetData.PackageName.ToString();
	if (!IsManagedPackagePath(PackagePath))
	{
		return;
	}

	FString RemovedMarkdownPath;
	FString RemovedJsonPath;
	FString Error;
	if (!UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
		PackagePath,
		BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir()),
		BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir()),
		RemovedMarkdownPath,
		RemovedJsonPath,
		Error
	))
	{
		if (!Error.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
		}
		return;
	}

	FString IndexPath;
	FString IndexError;
	if (!RebuildIndex(IndexPath, IndexError))
	{
		if (!IndexError.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *IndexError);
		}
		return;
	}

	FString ManifestPath;
	int32 SupportedAssetCount = 0;
	FString ManifestError;
	if (!RefreshStartupSyncManifest(ManifestPath, SupportedAssetCount, ManifestError) && !ManifestError.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *ManifestError);
	}
}

void UBlueprintGraphExportSubsystem::HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Settings->bEnableAutoExportOnSave || bStartupSyncInProgress)
	{
		return;
	}

	const FString DocumentationRootDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir());
	const FString JsonOutputDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir());
	const FString OldPackagePath = FPackageName::ObjectPathToPackageName(OldObjectPath);
	const bool bOldManaged = IsManagedPackagePath(OldPackagePath);
	const bool bNewManaged = IsManagedPackagePath(AssetData.PackageName.ToString());
	if (!bOldManaged && !bNewManaged)
	{
		return;
	}

	bool bSucceeded = true;
	if (bOldManaged)
	{
		FString RemovedMarkdownPath;
		FString RemovedJsonPath;
		FString Error;
		if (!UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
			OldPackagePath,
			DocumentationRootDir,
			JsonOutputDir,
			RemovedMarkdownPath,
			RemovedJsonPath,
			Error
		))
		{
			bSucceeded = false;
			if (!Error.IsEmpty())
			{
				UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
			}
		}
	}

	if (bNewManaged)
	{
		if (UObject* Asset = AssetData.FastGetAsset(false))
		{
			FString MarkdownPath;
			FString JsonPath;
			FString Error;
			if (!UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
				Asset,
				DocumentationRootDir,
				JsonOutputDir,
				Settings->bPrettyPrintJson,
				MarkdownPath,
				JsonPath,
				Error
			))
			{
				bSucceeded = false;
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
				}
			}
		}
		else
		{
			bSucceeded = false;
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Failed to load renamed asset for documentation export: %s"), *AssetData.PackageName.ToString());
		}
	}

	if (!bSucceeded)
	{
		return;
	}

	FString IndexPath;
	FString IndexError;
	if (!RebuildIndex(IndexPath, IndexError))
	{
		if (!IndexError.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *IndexError);
		}
		return;
	}

	FString ManifestPath;
	int32 SupportedAssetCount = 0;
	FString ManifestError;
	if (!RefreshStartupSyncManifest(ManifestPath, SupportedAssetCount, ManifestError) && !ManifestError.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *ManifestError);
	}
}

void UBlueprintGraphExportSubsystem::HandleAssetRegistryFilesLoaded()
{
	if (FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		if (FilesLoadedHandle.IsValid())
		{
			AssetRegistryModule->Get().OnFilesLoaded().Remove(FilesLoadedHandle);
			FilesLoadedHandle.Reset();
		}
	}

	ScheduleStartupSync();
}

bool UBlueprintGraphExportSubsystem::HandleStartupSyncTicker(float DeltaTime)
{
	StartupSyncTickerHandle.Reset();
	bStartupSyncScheduled = false;

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Settings->bEnableStartupFullSync)
	{
		return false;
	}

	bStartupSyncInProgress = true;
	FBlueprintGraphExportSyncOptions Options = FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings);
	Options.bOnlyIfStale = Settings->bOnlyRunStartupSyncWhenStale;
	const FBlueprintGraphExportSyncResult Result = FBlueprintGraphExportSyncRunner::RunFullSync(Options);
	bStartupSyncInProgress = false;

	if (Result.Status == EBlueprintGraphExportSyncStatus::SkippedUpToDate)
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync skipped: %s"), *Result.Reason);
		return false;
	}

	if (Result.Status == EBlueprintGraphExportSyncStatus::Success)
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync completed for %d supported assets."), Result.SupportedAssetCount);
	}
	else if (!Result.Reason.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Startup documentation sync completed with errors: %s"), *Result.Reason);
	}
	else
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Startup documentation sync completed with errors. See previous log entries."));
	}

	return false;
}

bool UBlueprintGraphExportSubsystem::ExportManagedAssetsInPackage(UPackage* Package, FString& OutError)
{
	OutError.Reset();
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Package)
	{
		OutError = TEXT("BlueprintGraphExport settings or package was unavailable for incremental export.");
		return false;
	}

	TArray<UObject*> PackageObjects;
	GetObjectsWithPackage(Package, PackageObjects, false);

	const FBlueprintGraphExportSyncOptions Options = FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings);

	bool bSucceeded = true;
	FString FirstError;
	for (UObject* Object : PackageObjects)
	{
		if (!Object || Object->GetOuter() != Package || (!Cast<UBlueprint>(Object) && !Cast<UDataAsset>(Object)))
		{
			continue;
		}

		FString MarkdownPath;
		FString JsonPath;
		FString Error;
		if (!UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
			Object,
			Options.DocumentationRootDir,
			Options.JsonOutputDir,
			Options.bPrettyPrintJson,
			MarkdownPath,
			JsonPath,
			Error
		))
		{
			bSucceeded = false;
			if (!Error.IsEmpty())
			{
				UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
				if (FirstError.IsEmpty())
				{
					FirstError = Error;
				}
			}
		}
	}

	if (!bSucceeded)
	{
		OutError = FirstError.IsEmpty()
			? FString::Printf(TEXT("One or more assets in package %s failed to export."), *Package->GetName())
			: FirstError;
	}
	return bSucceeded;
}

bool UBlueprintGraphExportSubsystem::RebuildIndex(FString& OutIndexPath, FString& OutError) const
{
	OutIndexPath.Reset();
	OutError.Reset();
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings)
	{
		OutError = TEXT("BlueprintGraphExport settings were unavailable for index rebuild.");
		return false;
	}

	const FBlueprintGraphExportSyncOptions Options = FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings);
	return BlueprintGraphExportInternal::RebuildDocumentationIndexForRoots(
		Options.RootAssetPaths,
		Options.DocumentationRootDir,
		OutIndexPath,
		OutError
	);
}

bool UBlueprintGraphExportSubsystem::RefreshStartupSyncManifest(FString& OutManifestPath, int32& OutSupportedAssetCount, FString& OutError) const
{
	OutManifestPath.Reset();
	OutSupportedAssetCount = 0;
	OutError.Reset();

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings)
	{
		OutError = TEXT("BlueprintGraphExport settings were unavailable for manifest refresh.");
		return false;
	}

	return FBlueprintGraphExportSyncRunner::RefreshManifest(
		FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings),
		OutManifestPath,
		OutSupportedAssetCount,
		OutError
	);
}

void UBlueprintGraphExportSubsystem::CancelScheduledStartupSync()
{
	if (StartupSyncTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartupSyncTickerHandle);
		StartupSyncTickerHandle.Reset();
	}

	bStartupSyncScheduled = false;
}

int32 UBlueprintGraphExportSubsystem::GetManagedSupportedAssetCount() const
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	return Settings ? BlueprintGraphExportSubsystem::CollectManagedSupportedAssets(Settings->RootAssetPaths).Num() : 0;
}

void UBlueprintGraphExportSubsystem::ScheduleStartupSync()
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Settings->bEnableStartupFullSync || bStartupSyncScheduled || bStartupSyncInProgress)
	{
		return;
	}

	const float DelaySeconds = FMath::Max(0.0f, Settings->StartupSyncDelaySeconds);
	StartupSyncTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UBlueprintGraphExportSubsystem::HandleStartupSyncTicker),
		DelaySeconds
	);
	bStartupSyncScheduled = true;

	UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync scheduled after Asset Registry load (delay=%.2fs)."), DelaySeconds);
}

bool UBlueprintGraphExportSubsystem::ShouldRunStartupFullSync(FString& OutReason, int32& OutSupportedAssetCount) const
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Settings->bEnableStartupFullSync)
	{
		OutReason = TEXT("startup sync is disabled");
		OutSupportedAssetCount = 0;
		return false;
	}

	FBlueprintGraphExportSyncOptions Options = FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings(Settings);
	Options.bOnlyIfStale = Settings->bOnlyRunStartupSyncWhenStale;

	const FBlueprintGraphExportSyncResult Result = FBlueprintGraphExportSyncRunner::EvaluateStaleness(Options);
	OutReason = Result.Reason;
	OutSupportedAssetCount = Result.SupportedAssetCount;
	return Result.Status == EBlueprintGraphExportSyncStatus::Success;
}

bool UBlueprintGraphExportSubsystem::RunStartupFullSync()
{
	return FBlueprintGraphExportSyncRunner::RunFullSync(
		FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings()
	).Status == EBlueprintGraphExportSyncStatus::Success;
}




