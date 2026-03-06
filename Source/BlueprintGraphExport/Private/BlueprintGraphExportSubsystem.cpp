#include "BlueprintGraphExportSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintGraphExportLibrary.h"
#include "BlueprintGraphExportSettings.h"
#include "BlueprintGraphExportPathUtils.h"
#include "Dom/JsonObject.h"
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

	static TArray<FAssetData> CollectSupportedAssetsUnderRoot(IAssetRegistry& AssetRegistry, const FString& RootAssetPath)
	{
		TArray<FAssetData> SupportedAssets;

		FText ValidationError;
		if (!FPackageName::IsValidLongPackageName(RootAssetPath, false, &ValidationError))
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Invalid root asset path for startup sync: %s (%s)"), *RootAssetPath, *ValidationError.ToString());
			return SupportedAssets;
		}

		if (!AssetRegistry.PathExists(FName(*RootAssetPath)))
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Startup sync root asset path does not exist: %s"), *RootAssetPath);
			return SupportedAssets;
		}

		TArray<FAssetData> AssetDataList;
		if (!AssetRegistry.GetAssetsByPath(FName(*RootAssetPath), AssetDataList, true, false))
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Failed to enumerate assets under startup sync root path: %s"), *RootAssetPath);
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
}

void UBlueprintGraphExportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

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

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	const int32 RootPathCount = Settings ? Settings->RootAssetPaths.Num() : 0;
	const int32 SupportedAssetCount = GetManagedSupportedAssetCount();

	UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Manual documentation sync started for %d supported assets across %d root paths."), SupportedAssetCount, RootPathCount);
	bStartupSyncInProgress = true;
	const bool bSucceeded = RunStartupFullSync(SupportedAssetCount);
	bStartupSyncInProgress = false;

	const FString DocumentationRootDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings ? Settings->DocumentationRootDir : FString(), BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir());
	const FString ManifestPath = BlueprintGraphExportSubsystem::ResolveFileSetting(Settings ? Settings->StartupSyncManifestPath : FString(), BlueprintGraphExportSubsystem::GetDefaultManifestPath());
	const FString IndexPath = BlueprintGraphExportSubsystem::GetIndexPath(DocumentationRootDir);

	if (bSucceeded)
	{
		OutReasonOrError = FString::Printf(TEXT("Manual documentation sync completed for %d supported assets. Index: %s Manifest: %s"), SupportedAssetCount, *IndexPath, *ManifestPath);
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("%s"), *OutReasonOrError);
	}
	else
	{
		OutReasonOrError = TEXT("Manual documentation sync completed with errors. See previous log entries.");
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *OutReasonOrError);
	}

	return bSucceeded;
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

	ExportManagedAssetsInPackage(Package);
	RebuildIndex();
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
	UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
		PackagePath,
		BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir()),
		BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir()),
		RemovedMarkdownPath,
		RemovedJsonPath,
		Error
	);

	if (!Error.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
	}

	RebuildIndex();
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
	if (IsManagedPackagePath(OldPackagePath))
	{
		FString RemovedMarkdownPath;
		FString RemovedJsonPath;
		FString Error;
		UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
			OldPackagePath,
			DocumentationRootDir,
			JsonOutputDir,
			RemovedMarkdownPath,
			RemovedJsonPath,
			Error
		);

		if (!Error.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
		}
	}

	if (IsManagedPackagePath(AssetData.PackageName.ToString()))
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
				if (!Error.IsEmpty())
				{
					UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
				}
			}
		}
	}

	RebuildIndex();
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

	FString Reason;
	int32 SupportedAssetCount = 0;
	const bool bShouldRun = ShouldRunStartupFullSync(Reason, SupportedAssetCount);
	if (!bShouldRun)
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync skipped: %s"), *Reason);
		return false;
	}

	UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync started for %d supported assets: %s"), SupportedAssetCount, *Reason);
	bStartupSyncInProgress = true;
	const bool bSucceeded = RunStartupFullSync(SupportedAssetCount);
	bStartupSyncInProgress = false;

	if (bSucceeded)
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Startup documentation sync completed for %d supported assets."), SupportedAssetCount);
	}
	else
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Startup documentation sync completed with errors. See previous log entries."));
	}

	return false;
}

void UBlueprintGraphExportSubsystem::ExportManagedAssetsInPackage(UPackage* Package)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings || !Package)
	{
		return;
	}

	TArray<UObject*> PackageObjects;
	GetObjectsWithPackage(Package, PackageObjects, false);

	const FString DocumentationRootDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir());
	const FString JsonOutputDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir());

	for (UObject* Object : PackageObjects)
	{
		if (!Object || Object->GetOuter() != Package)
		{
			continue;
		}

		FString MarkdownPath;
		FString JsonPath;
		FString Error;
		if (!UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
			Object,
			DocumentationRootDir,
			JsonOutputDir,
			Settings->bPrettyPrintJson,
			MarkdownPath,
			JsonPath,
			Error
		))
		{
			if (!Error.IsEmpty())
			{
				UE_LOG(LogBlueprintGraphExportSubsystem, Verbose, TEXT("%s"), *Error);
			}
		}
	}
}

void UBlueprintGraphExportSubsystem::RebuildIndex() const
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings)
	{
		return;
	}

	FString IndexPath;
	FString Error;
	if (!UBlueprintGraphExportLibrary::RebuildDocumentationIndex(
		BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir()),
		IndexPath,
		Error
	))
	{
		if (!Error.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("%s"), *Error);
		}
	}
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

	const TArray<FAssetData> Assets = BlueprintGraphExportSubsystem::CollectManagedSupportedAssets(Settings->RootAssetPaths);
	OutSupportedAssetCount = Assets.Num();

	const FString DocumentationRootDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir());
	const FString JsonOutputDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir());
	const FString ManifestPath = BlueprintGraphExportSubsystem::ResolveFileSetting(Settings->StartupSyncManifestPath, BlueprintGraphExportSubsystem::GetDefaultManifestPath());

	if (!Settings->bOnlyRunStartupSyncWhenStale)
	{
		OutReason = TEXT("forced by startup sync settings");
		return true;
	}

	if (!IFileManager::Get().FileExists(*ManifestPath))
	{
		OutReason = TEXT("startup sync manifest is missing");
		return true;
	}

	if (!IFileManager::Get().FileExists(*BlueprintGraphExportSubsystem::GetIndexPath(DocumentationRootDir)))
	{
		OutReason = TEXT("documentation index is missing");
		return true;
	}

	for (const FAssetData& AssetData : Assets)
	{
		const FString PackagePath = AssetData.PackageName.ToString();
		if (!IFileManager::Get().FileExists(*BlueprintGraphExportSubsystem::GetMarkdownPathForAsset(PackagePath, DocumentationRootDir)) ||
			!IFileManager::Get().FileExists(*BlueprintGraphExportSubsystem::GetJsonPathForAsset(PackagePath, JsonOutputDir)))
		{
			OutReason = FString::Printf(TEXT("documentation mirror is missing for %s"), *PackagePath);
			return true;
		}
	}

	TSharedPtr<FJsonObject> ExistingManifest;
	if (!BlueprintGraphExportSubsystem::LoadJsonObjectFromFile(ManifestPath, ExistingManifest) || !ExistingManifest.IsValid())
	{
		OutReason = TEXT("startup sync manifest could not be parsed");
		return true;
	}

	const TSharedRef<FJsonObject> CurrentSnapshot = BlueprintGraphExportSubsystem::BuildManifestSnapshot(
		Assets,
		Settings->RootAssetPaths,
		DocumentationRootDir,
		JsonOutputDir,
		Settings->bPrettyPrintJson,
		TEXT("")
	);
	ExistingManifest->SetStringField(TEXT("generated_at"), TEXT(""));

	FString CurrentJson;
	FString ExistingJson;
	if (!BlueprintGraphExportSubsystem::SerializeJsonObject(CurrentSnapshot, CurrentJson) || !BlueprintGraphExportSubsystem::SerializeJsonObject(ExistingManifest.ToSharedRef(), ExistingJson))
	{
		OutReason = TEXT("startup sync manifest comparison failed");
		return true;
	}

	if (CurrentJson != ExistingJson)
	{
		OutReason = TEXT("startup sync manifest differs from current asset state");
		return true;
	}

	OutReason = TEXT("startup sync manifest is current");
	return false;
}

bool UBlueprintGraphExportSubsystem::RunStartupFullSync(int32 SupportedAssetCount)
{
	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	if (!Settings)
	{
		return false;
	}

	const FString DocumentationRootDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->DocumentationRootDir, BlueprintGraphExportSubsystem::GetDefaultDocumentationRootDir());
	const FString JsonOutputDir = BlueprintGraphExportSubsystem::ResolveDirectorySetting(Settings->JsonOutputDir, BlueprintGraphExportSubsystem::GetDefaultJsonOutputDir());
	const FString ManifestPath = BlueprintGraphExportSubsystem::ResolveFileSetting(Settings->StartupSyncManifestPath, BlueprintGraphExportSubsystem::GetDefaultManifestPath());

	bool bSucceeded = true;
	for (const FString& RootAssetPath : Settings->RootAssetPaths)
	{
		FString ResolvedDir;
		FString Error;
		if (!UBlueprintGraphExportLibrary::ExportAssetsUnderPathToMarkdown(RootAssetPath, DocumentationRootDir, true, ResolvedDir, Error))
		{
			bSucceeded = false;
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Documentation sync failed for root %s: %s"), *RootAssetPath, *Error);
		}
	}

	FString IndexPath;
	FString IndexError;
	if (!UBlueprintGraphExportLibrary::RebuildDocumentationIndex(DocumentationRootDir, IndexPath, IndexError))
	{
		bSucceeded = false;
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Documentation index rebuild failed: %s"), *IndexError);
	}

	if (!bSucceeded)
	{
		return false;
	}

	const TArray<FAssetData> Assets = BlueprintGraphExportSubsystem::CollectManagedSupportedAssets(Settings->RootAssetPaths);
	const TSharedRef<FJsonObject> ManifestSnapshot = BlueprintGraphExportSubsystem::BuildManifestSnapshot(
		Assets,
		Settings->RootAssetPaths,
		DocumentationRootDir,
		JsonOutputDir,
		Settings->bPrettyPrintJson,
		FDateTime::UtcNow().ToIso8601()
	);

	FString ManifestJson;
	if (!BlueprintGraphExportSubsystem::SerializeJsonObject(ManifestSnapshot, ManifestJson))
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Documentation sync completed but manifest serialization failed."));
		return false;
	}

	FString ManifestError;
	if (!BlueprintGraphExportSubsystem::SaveJsonText(ManifestJson, ManifestPath, ManifestError))
	{
		UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("Documentation sync completed but manifest write failed: %s"), *ManifestError);
		return false;
	}

	UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("Documentation sync manifest updated at %s for %d supported assets."), *ManifestPath, SupportedAssetCount);
	return true;
}




