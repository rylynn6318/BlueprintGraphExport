#include "BlueprintGraphExportSubsystem.h"
#include "BlueprintGraphExportSyncRunner.h"
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
		const FBlueprintGraphExportSyncResult Result = FBlueprintGraphExportSyncRunner::RunFullSync(
			FBlueprintGraphExportSyncRunner::MakeOptionsFromSettings()
		);
		if (Result.Status == EBlueprintGraphExportSyncStatus::Success)
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("BlueprintGraphExport.SyncAllDocs completed: %s"), *Result.Reason);
		}
		else if (Result.Status == EBlueprintGraphExportSyncStatus::SkippedUpToDate)
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Display, TEXT("BlueprintGraphExport.SyncAllDocs skipped: %s"), *Result.Reason);
		}
		else if (!Result.Reason.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExportSubsystem, Warning, TEXT("BlueprintGraphExport.SyncAllDocs failed: %s"), *Result.Reason);
		}
	}

	TUniquePtr<FAutoConsoleCommand> SyncAllDocsCommand;
};

IMPLEMENT_MODULE(FBlueprintGraphExportModule, BlueprintGraphExport)
