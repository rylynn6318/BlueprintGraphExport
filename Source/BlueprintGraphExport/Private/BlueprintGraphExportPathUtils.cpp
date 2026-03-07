#include "BlueprintGraphExportPathUtils.h"

#include "CoreMinimal.h"
#include "BlueprintGraphExportSettings.h"
#include "Misc/Paths.h"

namespace BlueprintGraphExportPathUtils
{
	namespace
	{
		static FString ResolveConfiguredPath(const FString& PathValue, const FString& DefaultRelativePath, const UBlueprintGraphExportSettings* Settings)
		{
			const FString EffectivePath = PathValue.IsEmpty() ? DefaultRelativePath : PathValue;
			if (EffectivePath.IsEmpty())
			{
				return FString();
			}

			if (FPaths::IsRelative(EffectivePath))
			{
				return FPaths::ConvertRelativePathToFull(FPaths::Combine(ResolveOutputBaseDir(Settings), EffectivePath));
			}

			return FPaths::ConvertRelativePathToFull(EffectivePath);
		}
	}

	FString ResolveOutputBaseDir(const UBlueprintGraphExportSettings* Settings)
	{
		const UBlueprintGraphExportSettings* EffectiveSettings = Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>();
		const FString BaseValue = EffectiveSettings ? EffectiveSettings->OutputBaseDir : FString();
		if (BaseValue.IsEmpty() || BaseValue == TEXT("."))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		}

		if (FPaths::IsRelative(BaseValue))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), BaseValue));
		}

		return FPaths::ConvertRelativePathToFull(BaseValue);
	}

	FString ResolvePathAgainstOutputBase(const FString& PathValue, const UBlueprintGraphExportSettings* Settings)
	{
		return ResolveConfiguredPath(PathValue, FString(), Settings);
	}

	FString GetDocumentationRootDir(const UBlueprintGraphExportSettings* Settings)
	{
		const UBlueprintGraphExportSettings* EffectiveSettings = Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>();
		return ResolveConfiguredPath(EffectiveSettings ? EffectiveSettings->DocumentationRootDir : FString(), TEXT("Saved/BlueprintGraphExport/Docs"), EffectiveSettings);
	}

	FString GetJsonOutputDir(const UBlueprintGraphExportSettings* Settings)
	{
		const UBlueprintGraphExportSettings* EffectiveSettings = Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>();
		return ResolveConfiguredPath(EffectiveSettings ? EffectiveSettings->JsonOutputDir : FString(), TEXT("Saved/BlueprintGraphExport/Json"), EffectiveSettings);
	}

	FString GetManifestPath(const UBlueprintGraphExportSettings* Settings)
	{
		const UBlueprintGraphExportSettings* EffectiveSettings = Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>();
		return ResolveConfiguredPath(EffectiveSettings ? EffectiveSettings->StartupSyncManifestPath : FString(), TEXT("Saved/BlueprintGraphExport/StartupSyncManifest.json"), EffectiveSettings);
	}

	FString GetAggregateOutputPath(const UBlueprintGraphExportSettings* Settings)
	{
		return ResolveConfiguredPath(FString(), TEXT("Saved/BlueprintGraphExport/BlueprintGraphAnalysis.json"), Settings ? Settings : GetDefault<UBlueprintGraphExportSettings>());
	}
}
