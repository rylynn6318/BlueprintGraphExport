#pragma once

#include "Engine/DataTable.h"

#include "BlueprintGraphExportAutomationTests.generated.h"

USTRUCT()
struct FBlueprintGraphExportAutomationNestedRefs
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<TSoftObjectPtr<UObject>> NestedSoftObjectArray;
};

USTRUCT()
struct FBlueprintGraphExportAutomationRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<TSoftObjectPtr<UObject>> SoftObjectArray;

	UPROPERTY()
	TSet<TSoftObjectPtr<UObject>> SoftObjectSet;

	UPROPERTY()
	TMap<TSoftObjectPtr<UObject>, FString> SoftObjectKeyMap;

	UPROPERTY()
	TMap<FString, TSoftObjectPtr<UObject>> SoftObjectValueMap;

	UPROPERTY()
	FBlueprintGraphExportAutomationNestedRefs NestedRefs;
};
