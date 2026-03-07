#pragma once

#include "Engine/DeveloperSettings.h"

#include "BlueprintGraphExportSettings.generated.h"

UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Blueprint Graph Export"))
class BLUEPRINTGRAPHEXPORT_API UBlueprintGraphExportSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UBlueprintGraphExportSettings();

	virtual FName GetCategoryName() const override;

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif

	UPROPERTY(EditAnywhere, Config, Category = "Automation", meta = (ToolTip = "Automatically re-export supported assets when they are saved in the editor. Enabled by default."))
	bool bEnableAutoExportOnSave;

	UPROPERTY(EditAnywhere, Config, Category = "Automation")
	bool bPrettyPrintJson;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync", meta = (ToolTip = "Optionally rebuild the configured documentation roots when the editor starts. Disabled by default because scanning large projects can be expensive."))
	bool bEnableStartupFullSync;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync", meta = (ToolTip = "When startup full sync is enabled, only run it if the generated outputs are stale."))
	bool bOnlyRunStartupSyncWhenStale;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync", meta = (ClampMin = "0.0", ToolTip = "Delay before the optional startup full sync begins after the asset registry is ready."))
	float StartupSyncDelaySeconds;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Base directory for relative output paths. Empty or '.' uses the project directory that contains the .uproject file."))
	FString OutputBaseDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir. Default is Saved/BlueprintGraphExport/StartupSyncManifest.json."))
	FString StartupSyncManifestPath;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir. Default is Saved/BlueprintGraphExport/Docs."))
	FString DocumentationRootDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir. Default is Saved/BlueprintGraphExport/Json."))
	FString JsonOutputDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Asset roots to scan. Defaults to /Game for plugin portability."))
	TArray<FString> RootAssetPaths;
};
