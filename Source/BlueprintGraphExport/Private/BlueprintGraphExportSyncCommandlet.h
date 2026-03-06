#pragma once

#include "Commandlets/Commandlet.h"

#include "BlueprintGraphExportSyncCommandlet.generated.h"

UCLASS()
class BLUEPRINTGRAPHEXPORT_API UBlueprintGraphExportSyncCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintGraphExportSyncCommandlet();

	virtual int32 Main(const FString& Params) override;
};
