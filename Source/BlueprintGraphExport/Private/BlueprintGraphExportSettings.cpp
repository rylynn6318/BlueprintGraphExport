#include "BlueprintGraphExportSettings.h"

UBlueprintGraphExportSettings::UBlueprintGraphExportSettings()
{
	bEnableAutoExportOnSave = true;
	bPrettyPrintJson = true;
	bIncludeGraphVisualizationInMarkdown = true;
	MaxVisualizationNodeCount = 80;
	bEnableStartupFullSync = true;
	bOnlyRunStartupSyncWhenStale = true;
	StartupSyncDelaySeconds = 0.0f;
	OutputBaseDir = TEXT(".");
	StartupSyncManifestPath = TEXT("Saved/BlueprintGraphAnalysis/StartupSyncManifest.json");
	DocumentationRootDir = TEXT("Docs/AssetMirror");
	JsonOutputDir = TEXT("Saved/BlueprintGraphAnalysis");
	RootAssetPaths = { TEXT("/Game") };

	CategoryName = TEXT("Plugins");
	SectionName = TEXT("BlueprintGraphExport");
}

FName UBlueprintGraphExportSettings::GetCategoryName() const
{
	return CategoryName;
}

#if WITH_EDITOR
FText UBlueprintGraphExportSettings::GetSectionText() const
{
	return NSLOCTEXT("BlueprintGraphExport", "SettingsSectionText", "Blueprint Graph Export");
}

FText UBlueprintGraphExportSettings::GetSectionDescription() const
{
	return NSLOCTEXT(
		"BlueprintGraphExport",
		"SettingsSectionDescription",
		"Controls automatic Blueprint and DataAsset markdown or JSON exports, including optional Mermaid graph visualizations. Relative output paths are resolved against the configured output base directory, which defaults to the project directory."
	);
}
#endif
