#include "BlueprintGraphExportSubsystem.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FBlueprintGraphExportModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		SyncAllDocsCommand = MakeUnique<FAutoConsoleCommand>(
			TEXT("BlueprintGraphExport.SyncAllDocs"),
			TEXT("Runs a full BlueprintGraphExport documentation sync for all configured root asset paths."),
			FConsoleCommandDelegate::CreateRaw(this, &FBlueprintGraphExportModule::ExecuteSyncAllDocs)
		);
	}

	virtual void ShutdownModule() override
	{
		SyncAllDocsCommand.Reset();
	}

private:
	void ExecuteSyncAllDocs()
	{
		if (!GEditor)
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExport.SyncAllDocs failed: editor subsystem is unavailable."));
			return;
		}

		UBlueprintGraphExportSubsystem* ExportSubsystem = GEditor->GetEditorSubsystem<UBlueprintGraphExportSubsystem>();
		if (!ExportSubsystem)
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExport.SyncAllDocs failed: BlueprintGraphExport subsystem is unavailable."));
			return;
		}

		FString Result;
		if (!ExportSubsystem->RunManualFullSync(Result) && !Result.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExport.SyncAllDocs failed: %s"), *Result);
		}
	}

	TUniquePtr<FAutoConsoleCommand> SyncAllDocsCommand;
};

IMPLEMENT_MODULE(FBlueprintGraphExportModule, BlueprintGraphExport)
