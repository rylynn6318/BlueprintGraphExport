#pragma once

#include "CoreMinimal.h"

class UBlueprintGraphExportSettings;

namespace BlueprintGraphExportPathUtils
{
	BLUEPRINTGRAPHEXPORT_API FString ResolveOutputBaseDir(const UBlueprintGraphExportSettings* Settings = nullptr);
	BLUEPRINTGRAPHEXPORT_API FString ResolvePathAgainstOutputBase(const FString& PathValue, const UBlueprintGraphExportSettings* Settings = nullptr);
	BLUEPRINTGRAPHEXPORT_API FString GetDocumentationRootDir(const UBlueprintGraphExportSettings* Settings = nullptr);
	BLUEPRINTGRAPHEXPORT_API FString GetJsonOutputDir(const UBlueprintGraphExportSettings* Settings = nullptr);
	BLUEPRINTGRAPHEXPORT_API FString GetManifestPath(const UBlueprintGraphExportSettings* Settings = nullptr);
	BLUEPRINTGRAPHEXPORT_API FString GetAggregateOutputPath(const UBlueprintGraphExportSettings* Settings = nullptr);
}
