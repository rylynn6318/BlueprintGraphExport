#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"

namespace BlueprintGraphExportInternal
{
	bool RebuildDocumentationIndexForRoots(
		const TArray<FString>& RootPaths,
		const FString& DocsRootDir,
		FString& OutIndexPath,
		FString& OutError
	);
}
