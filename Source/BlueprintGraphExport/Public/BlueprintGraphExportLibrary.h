#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"

#include "BlueprintGraphExportLibrary.generated.h"

UCLASS()
class BLUEPRINTGRAPHEXPORT_API UBlueprintGraphExportLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Blueprint Graph Export", meta = (CPP_Default_bPrettyPrint = "true"))
	static FString GetBlueprintGraphJson(UObject* BlueprintAsset, bool bPrettyPrint = true);

	UFUNCTION(BlueprintCallable, Category = "Blueprint Graph Export", meta = (CPP_Default_OutputPath = "", CPP_Default_bPrettyPrint = "true"))
	static bool ExportBlueprintAssetToJson(
		UObject* BlueprintAsset,
		const FString& OutputPath,
		bool bPrettyPrint,
		FString& OutResolvedPath,
		FString& OutError
	);

	UFUNCTION(
		BlueprintCallable,
		Category = "Blueprint Graph Export",
		meta = (
			CPP_Default_RootAssetPath = "/Game",
			CPP_Default_OutputPath = "",
			CPP_Default_bRecursive = "true",
			CPP_Default_bPrettyPrint = "true"
		)
	)
	static bool ExportBlueprintsUnderPathToJson(
		const FString& RootAssetPath,
		const FString& OutputPath,
		bool bRecursive,
		bool bPrettyPrint,
		FString& OutResolvedPath,
		FString& OutError
	);

	UFUNCTION(BlueprintCallable, Category = "Blueprint Graph Export", meta = (CPP_Default_OutputPath = ""))
	static bool ExportAssetToMarkdown(
		UObject* Asset,
		const FString& OutputPath,
		bool& bOutSupported,
		FString& OutResolvedPath,
		FString& OutError
	);

	UFUNCTION(
		BlueprintCallable,
		Category = "Blueprint Graph Export",
		meta = (
			CPP_Default_RootAssetPath = "/Game",
			CPP_Default_OutputRootDir = "",
			CPP_Default_bRecursive = "true"
		)
	)
	static bool ExportAssetsUnderPathToMarkdown(
		const FString& RootAssetPath,
		const FString& OutputRootDir,
		bool bRecursive,
		FString& OutResolvedDir,
		FString& OutError
	);

	UFUNCTION(BlueprintCallable, Category = "Blueprint Graph Export", meta = (CPP_Default_DocsRootDir = ""))
	static bool RebuildDocumentationIndex(
		const FString& DocsRootDir,
		FString& OutIndexPath,
		FString& OutError
	);

	static bool ExportAssetDocumentationBundle(
		UObject* Asset,
		const FString& DocumentationRootDir,
		const FString& JsonOutputDir,
		bool bPrettyPrintJson,
		FString& OutMarkdownPath,
		FString& OutJsonPath,
		FString& OutError
	);

	static bool RemoveAssetDocumentationBundle(
		const FString& AssetPackagePath,
		const FString& DocumentationRootDir,
		const FString& JsonOutputDir,
		FString& OutRemovedMarkdownPath,
		FString& OutRemovedJsonPath,
		FString& OutError
	);
};

