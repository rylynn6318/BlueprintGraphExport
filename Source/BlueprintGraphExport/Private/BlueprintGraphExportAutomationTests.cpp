#include "BlueprintGraphExportAutomationTests.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintGraphExportLibrary.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace BlueprintGraphExportAutomationTests
{
	static UObject* CreateReferenceObject(const FString& PackagePath, const FString& ObjectName)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		return Package ? NewObject<UDataTable>(Package, FName(*ObjectName), RF_Public | RF_Standalone) : nullptr;
	}

	static bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	static TSharedPtr<FJsonObject> GetFirstObjectFromArrayField(const TSharedPtr<FJsonObject>& ParentObject, const FString& FieldName)
	{
		if (!ParentObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!ParentObject->TryGetArrayField(FieldName, Values) || !Values || Values->Num() == 0)
		{
			return nullptr;
		}

		return (*Values)[0].IsValid() ? (*Values)[0]->AsObject() : nullptr;
	}

	static TSharedPtr<FJsonObject> FindColumnByPropertyName(const TSharedPtr<FJsonObject>& RowObject, const FString& PropertyName)
	{
		if (!RowObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* ColumnValues = nullptr;
		if (!RowObject->TryGetArrayField(TEXT("columns"), ColumnValues) || !ColumnValues)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& ColumnValue : *ColumnValues)
		{
			const TSharedPtr<FJsonObject> ColumnObject = ColumnValue.IsValid() ? ColumnValue->AsObject() : nullptr;
			if (ColumnObject.IsValid() && ColumnObject->GetStringField(TEXT("property_name")) == PropertyName)
			{
				return ColumnObject;
			}
		}

		return nullptr;
	}

	static TArray<FString> GetReferenceTargets(const TSharedPtr<FJsonObject>& ColumnObject)
	{
		TArray<FString> Targets;
		if (!ColumnObject.IsValid())
		{
			return Targets;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!ColumnObject->TryGetArrayField(TEXT("reference_targets"), Values) || !Values)
		{
			return Targets;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue))
			{
				Targets.Add(StringValue);
			}
		}

		return Targets;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintGraphExportDataTableReferenceTargetsTest,
	"BlueprintGraphExport.DataTable.ReferenceTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FBlueprintGraphExportDataTableReferenceTargetsTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintGraphExportAutomation"), TEXT("DataTableReferenceTargets"));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);

	const FString DocumentationRootDir = FPaths::Combine(TestRoot, TEXT("Docs"));
	const FString JsonOutputDir = FPaths::Combine(TestRoot, TEXT("Json"));

	UObject* ArrayReference = BlueprintGraphExportAutomationTests::CreateReferenceObject(TEXT("/BlueprintGraphExportTests/ArrayReference"), TEXT("ArrayReference"));
	UObject* SetReference = BlueprintGraphExportAutomationTests::CreateReferenceObject(TEXT("/BlueprintGraphExportTests/SetReference"), TEXT("SetReference"));
	UObject* MapKeyReference = BlueprintGraphExportAutomationTests::CreateReferenceObject(TEXT("/BlueprintGraphExportTests/MapKeyReference"), TEXT("MapKeyReference"));
	UObject* MapValueReference = BlueprintGraphExportAutomationTests::CreateReferenceObject(TEXT("/BlueprintGraphExportTests/MapValueReference"), TEXT("MapValueReference"));
	UObject* NestedReference = BlueprintGraphExportAutomationTests::CreateReferenceObject(TEXT("/BlueprintGraphExportTests/NestedReference"), TEXT("NestedReference"));

	TestNotNull(TEXT("Array reference object should exist"), ArrayReference);
	TestNotNull(TEXT("Set reference object should exist"), SetReference);
	TestNotNull(TEXT("Map key reference object should exist"), MapKeyReference);
	TestNotNull(TEXT("Map value reference object should exist"), MapValueReference);
	TestNotNull(TEXT("Nested reference object should exist"), NestedReference);

	UPackage* TablePackage = CreatePackage(TEXT("/BlueprintGraphExportTests/DT_ReferenceTargets"));
	TestNotNull(TEXT("Table package should exist"), TablePackage);

	UDataTable* DataTable = TablePackage ? NewObject<UDataTable>(TablePackage, TEXT("DT_ReferenceTargets"), RF_Public | RF_Standalone) : nullptr;
	TestNotNull(TEXT("Data table should exist"), DataTable);
	if (!DataTable)
	{
		return false;
	}

	DataTable->RowStruct = FBlueprintGraphExportAutomationRow::StaticStruct();

	FBlueprintGraphExportAutomationRow Row;
	Row.SoftObjectArray.Add(TSoftObjectPtr<UObject>(ArrayReference));
	Row.SoftObjectSet.Add(TSoftObjectPtr<UObject>(SetReference));
	Row.SoftObjectKeyMap.Add(TSoftObjectPtr<UObject>(MapKeyReference), TEXT("KeyPayload"));
	Row.SoftObjectValueMap.Add(TEXT("ValuePayload"), TSoftObjectPtr<UObject>(MapValueReference));
	Row.NestedRefs.NestedSoftObjectArray.Add(TSoftObjectPtr<UObject>(NestedReference));
	DataTable->AddRow(TEXT("RowA"), Row);

	FString MarkdownPath;
	FString JsonPath;
	FString Error;
	const bool bExported = UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
		DataTable,
		DocumentationRootDir,
		JsonOutputDir,
		true,
		MarkdownPath,
		JsonPath,
		Error
	);
	TestTrue(FString::Printf(TEXT("Data table export should succeed: %s"), *Error), bExported);
	if (!bExported)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	TestTrue(TEXT("Exported JSON should load"), BlueprintGraphExportAutomationTests::LoadJsonObjectFromFile(JsonPath, RootObject));
	if (!RootObject.IsValid())
	{
		return false;
	}

	FString MarkdownText;
	TestTrue(TEXT("Exported markdown should load"), FFileHelper::LoadFileToString(MarkdownText, *MarkdownPath));

	const TSharedPtr<FJsonObject> AssetObject = BlueprintGraphExportAutomationTests::GetFirstObjectFromArrayField(RootObject, TEXT("assets"));
	TestTrue(TEXT("Exported JSON should contain an asset object"), AssetObject.IsValid());
	if (!AssetObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> RowObject = BlueprintGraphExportAutomationTests::GetFirstObjectFromArrayField(AssetObject, TEXT("rows"));
	TestTrue(TEXT("Exported JSON should contain a row object"), RowObject.IsValid());
	if (!RowObject.IsValid())
	{
		return false;
	}

	const TArray<FString> ArrayTargets = BlueprintGraphExportAutomationTests::GetReferenceTargets(
		BlueprintGraphExportAutomationTests::FindColumnByPropertyName(RowObject, TEXT("SoftObjectArray"))
	);
	const TArray<FString> SetTargets = BlueprintGraphExportAutomationTests::GetReferenceTargets(
		BlueprintGraphExportAutomationTests::FindColumnByPropertyName(RowObject, TEXT("SoftObjectSet"))
	);
	const TArray<FString> MapKeyTargets = BlueprintGraphExportAutomationTests::GetReferenceTargets(
		BlueprintGraphExportAutomationTests::FindColumnByPropertyName(RowObject, TEXT("SoftObjectKeyMap"))
	);
	const TArray<FString> MapValueTargets = BlueprintGraphExportAutomationTests::GetReferenceTargets(
		BlueprintGraphExportAutomationTests::FindColumnByPropertyName(RowObject, TEXT("SoftObjectValueMap"))
	);
	const TArray<FString> NestedTargets = BlueprintGraphExportAutomationTests::GetReferenceTargets(
		BlueprintGraphExportAutomationTests::FindColumnByPropertyName(RowObject, TEXT("NestedRefs"))
	);

	TestTrue(TEXT("Array references should include the array object path"), ArrayTargets.Contains(ArrayReference->GetPathName()));
	TestTrue(TEXT("Set references should include the set object path"), SetTargets.Contains(SetReference->GetPathName()));
	TestTrue(TEXT("Map key references should include the key object path"), MapKeyTargets.Contains(MapKeyReference->GetPathName()));
	TestTrue(TEXT("Map value references should include the value object path"), MapValueTargets.Contains(MapValueReference->GetPathName()));
	TestTrue(TEXT("Nested struct references should include the nested object path"), NestedTargets.Contains(NestedReference->GetPathName()));
	TestTrue(TEXT("Markdown should include the array reference path"), MarkdownText.Contains(ArrayReference->GetPathName()));
	TestTrue(TEXT("Markdown should include the set reference path"), MarkdownText.Contains(SetReference->GetPathName()));
	TestTrue(TEXT("Markdown should include the map key reference path"), MarkdownText.Contains(MapKeyReference->GetPathName()));
	TestTrue(TEXT("Markdown should include the map value reference path"), MarkdownText.Contains(MapValueReference->GetPathName()));
	TestTrue(TEXT("Markdown should include the nested reference path"), MarkdownText.Contains(NestedReference->GetPathName()));

	return true;
}

#endif
