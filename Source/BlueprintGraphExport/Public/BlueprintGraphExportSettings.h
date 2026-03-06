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

	UPROPERTY(EditAnywhere, Config, Category = "Automation")
	bool bEnableAutoExportOnSave;

	UPROPERTY(EditAnywhere, Config, Category = "Automation")
	bool bPrettyPrintJson;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync")
	bool bEnableStartupFullSync;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync")
	bool bOnlyRunStartupSyncWhenStale;

	UPROPERTY(EditAnywhere, Config, Category = "Startup Sync", meta = (ClampMin = "0.0"))
	float StartupSyncDelaySeconds;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Base directory for relative output paths. Empty or '.' uses the project directory that contains the .uproject file."))
	FString OutputBaseDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir."))
	FString StartupSyncManifestPath;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir."))
	FString DocumentationRootDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Relative paths are resolved against OutputBaseDir."))
	FString JsonOutputDir;

	UPROPERTY(EditAnywhere, Config, Category = "Paths", meta = (ToolTip = "Asset roots to scan. Defaults to /Game for plugin portability."))
	TArray<FString> RootAssetPaths;
};
