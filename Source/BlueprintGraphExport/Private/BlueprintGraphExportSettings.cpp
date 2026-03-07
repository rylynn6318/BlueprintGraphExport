#include "BlueprintGraphExportSettings.h"

UBlueprintGraphExportSettings::UBlueprintGraphExportSettings()
{
	bEnableAutoExportOnSave = true;
	bPrettyPrintJson = true;
	bEnableStartupFullSync = false;
	bOnlyRunStartupSyncWhenStale = true;
	StartupSyncDelaySeconds = 0.0f;
	OutputBaseDir = TEXT(".");
	StartupSyncManifestPath = TEXT("Saved/BlueprintGraphExport/StartupSyncManifest.json");
	DocumentationRootDir = TEXT("Saved/BlueprintGraphExport/Docs");
	JsonOutputDir = TEXT("Saved/BlueprintGraphExport/Json");
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
		"Controls automatic Blueprint, DataAsset, and DataTable markdown or JSON exports. Save-triggered export is enabled by default, while startup full sync is optional because it can be expensive on large projects. Relative output paths are resolved against the configured output base directory, which defaults to the project directory."
	);
}
#endif
