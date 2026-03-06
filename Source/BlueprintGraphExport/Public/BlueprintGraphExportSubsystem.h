#pragma once

#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"

#include "BlueprintGraphExportSubsystem.generated.h"

struct FAssetData;
class UPackage;

BLUEPRINTGRAPHEXPORT_API DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintGraphExportSubsystem, Log, All);

UCLASS()
class BLUEPRINTGRAPHEXPORT_API UBlueprintGraphExportSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	bool RunManualFullSync(FString& OutReasonOrError);

private:
	void HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext SaveContext);
	void HandleAssetRemoved(const FAssetData& AssetData);
	void HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);
	void HandleAssetRegistryFilesLoaded();
	bool HandleStartupSyncTicker(float DeltaTime);

	bool IsManagedPackagePath(const FString& PackagePath) const;
	void ExportManagedAssetsInPackage(UPackage* Package);
	void RebuildIndex() const;
	void ScheduleStartupSync();
	bool ShouldRunStartupFullSync(FString& OutReason, int32& OutSupportedAssetCount) const;
	bool RunStartupFullSync(int32 SupportedAssetCount);
	void CancelScheduledStartupSync();
	int32 GetManagedSupportedAssetCount() const;

	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle FilesLoadedHandle;
	FTSTicker::FDelegateHandle StartupSyncTickerHandle;
	bool bStartupSyncScheduled = false;
	bool bStartupSyncInProgress = false;
};
