#include "BlueprintGraphExportLibrary.h"

#include "Algo/Unique.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintGraphExportSettings.h"
#include "BlueprintGraphExportPathUtils.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintGraphExport, Log, All);

namespace BlueprintGraphExport
{
	static FString ResolveDirectory(const FString& DirectoryPath, const FString& FallbackAbsolutePath)
	{
		const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
		return DirectoryPath.IsEmpty()
			? FallbackAbsolutePath
			: BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(DirectoryPath, Settings);
	}

	static FString GetDefaultDocumentationRootDir()
	{
		return BlueprintGraphExportPathUtils::GetDocumentationRootDir();
	}

	static FString GetDefaultJsonMirrorRootDir()
	{
		return BlueprintGraphExportPathUtils::GetJsonOutputDir();
	}

	static FString GetDefaultAggregateOutputPath()
	{
		return BlueprintGraphExportPathUtils::GetAggregateOutputPath();
	}

	static FString GetRelativeMirrorPath(const FString& AssetPackagePath, const TCHAR* Extension)
	{
		FString RelativePath = AssetPackagePath;
		RelativePath.RemoveFromStart(TEXT("/"));
		return RelativePath + Extension;
	}

	static FString GetMarkdownPathForAsset(const FString& AssetPackagePath, const FString& DocumentationRootDir)
	{
		return FPaths::Combine(DocumentationRootDir, GetRelativeMirrorPath(AssetPackagePath, TEXT(".md")));
	}

	static FString GetJsonPathForAsset(const FString& AssetPackagePath, const FString& JsonOutputDir)
	{
		return FPaths::Combine(JsonOutputDir, GetRelativeMirrorPath(AssetPackagePath, TEXT(".json")));
	}

	static FString GetIndexPath(const FString& DocumentationRootDir)
	{
		return FPaths::Combine(FPaths::GetPath(DocumentationRootDir), TEXT("AssetIndex.md"));
	}

	static FString EscapeMarkdown(const FString& Text)
	{
		FString Escaped = Text;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("|"), TEXT("\\|"));
		Escaped.ReplaceInline(TEXT("`"), TEXT("\\`"));
		return Escaped;
	}

	static FString NormalizeMultilineForMarkdown(const FString& Text)
	{
		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"));
		return Normalized;
	}

	static bool SaveTextToPath(const FString& Text, const FString& OutputPath, FString& OutResolvedPath, FString& OutError)
	{
		const FString ResolvedPath = BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(OutputPath);
		const FString Directory = FPaths::GetPath(ResolvedPath);
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("Failed to create output directory: %s"), *Directory);
			return false;
		}

		if (!FFileHelper::SaveStringToFile(Text, *ResolvedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to save file: %s"), *ResolvedPath);
			return false;
		}

		OutResolvedPath = ResolvedPath;
		return true;
	}

	static bool SaveJsonToPath(const FString& Json, const FString& OutputPath, FString& OutResolvedPath, FString& OutError)
	{
		return SaveTextToPath(Json, OutputPath, OutResolvedPath, OutError);
	}

	static bool SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject, const bool bPrettyPrint, FString& OutJson)
	{
		if (bPrettyPrint)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
			return FJsonSerializer::Serialize(JsonObject, Writer);
		}

		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(JsonObject, Writer);
	}

	static FString GetNodeEnabledStateString(ENodeEnabledState EnabledState)
	{
		switch (EnabledState)
		{
		case ENodeEnabledState::Enabled: return TEXT("enabled");
		case ENodeEnabledState::Disabled: return TEXT("disabled");
		case ENodeEnabledState::DevelopmentOnly: return TEXT("development_only");
		default: return TEXT("unknown");
		}
	}

	static FString GetPinDirectionString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	static FString GetContainerTypeString(const FEdGraphPinType& PinType)
	{
		if (PinType.IsArray())
		{
			return TEXT("array");
		}
		if (PinType.IsSet())
		{
			return TEXT("set");
		}
		if (PinType.IsMap())
		{
			return TEXT("map");
		}
		return TEXT("none");
	}

	static FString GetGraphType(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return TEXT("unknown");
		}

		if (FBlueprintEditorUtils::FindEventGraph(const_cast<UBlueprint*>(Blueprint)) == Graph || Blueprint->UbergraphPages.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("event");
		}
		if (Blueprint->FunctionGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("function");
		}
		if (Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("macro");
		}
		if (Blueprint->DelegateSignatureGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("delegate");
		}

		const FString GraphClassName = Graph->GetClass()->GetName();
		const FString SchemaClassName = Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetName() : FString();
		if (GraphClassName.Contains(TEXT("Anim")) || SchemaClassName.Contains(TEXT("Anim")))
		{
			return TEXT("animation");
		}

		return TEXT("unknown");
	}

	static TArray<UEdGraph*> CollectGraphs(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> AllGraphs;
		if (!Blueprint)
		{
			return AllGraphs;
		}

		Blueprint->GetAllGraphs(AllGraphs);

		auto AddUniqueGraph = [&AllGraphs](UEdGraph* Graph)
		{
			if (Graph)
			{
				AllGraphs.AddUnique(Graph);
			}
		};

		AddUniqueGraph(FBlueprintEditorUtils::FindEventGraph(Blueprint));
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			AddUniqueGraph(Graph);
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			AddUniqueGraph(Graph);
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			AddUniqueGraph(Graph);
		}
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			AddUniqueGraph(Graph);
		}

		AllGraphs.RemoveAll([](UEdGraph* Graph) { return Graph == nullptr; });
		AllGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
		{
			return Left.GetName() < Right.GetName();
		});
		return AllGraphs;
	}

	static TArray<FString> CollectDependencies(const FName PackageName)
	{
		TArray<FString> DependencyPaths;
		if (!PackageName.IsNone())
		{
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			TArray<FName> Dependencies;
			AssetRegistry.GetDependencies(PackageName, Dependencies);
			for (const FName Dependency : Dependencies)
			{
				if (!Dependency.IsNone())
				{
					DependencyPaths.Add(Dependency.ToString());
				}
			}
		}

		DependencyPaths.Sort();
		DependencyPaths.SetNum(Algo::Unique(DependencyPaths));
		return DependencyPaths;
	}

	static TArray<FString> CollectImplementedInterfaces(const UBlueprint* Blueprint)
	{
		TArray<FString> Interfaces;
		if (!Blueprint)
		{
			return Interfaces;
		}

		for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDescription.Interface)
			{
				Interfaces.Add(InterfaceDescription.Interface->GetPathName());
			}
		}

		Interfaces.Sort();
		Interfaces.SetNum(Algo::Unique(Interfaces));
		return Interfaces;
	}
	static TArray<FString> CollectMessageNodes(const UBlueprint* Blueprint)
	{
		TArray<FString> MessageNodes;
		if (!Blueprint)
		{
			return MessageNodes;
		}

		for (UEdGraph* Graph : CollectGraphs(const_cast<UBlueprint*>(Blueprint)))
		{
			if (!Graph)
			{
				continue;
			}

			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const FString NodeClassPath = Node->GetClass()->GetPathName();
				if (NodeClassPath.Contains(TEXT("K2Node_Message")) || NodeClassPath.Contains(TEXT("K2Node_CallFunction")))
				{
					const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					if (!NodeTitle.IsEmpty())
					{
						MessageNodes.Add(NodeTitle);
					}
				}
			}
		}

		MessageNodes.Sort();
		MessageNodes.SetNum(Algo::Unique(MessageNodes));
		return MessageNodes;
	}

	static TArray<FString> CollectEntryPoints(const UBlueprint* Blueprint)
	{
		TArray<FString> EntryPoints;
		if (!Blueprint)
		{
			return EntryPoints;
		}

		for (UEdGraph* Graph : CollectGraphs(const_cast<UBlueprint*>(Blueprint)))
		{
			if (!Graph || GetGraphType(Blueprint, Graph) != TEXT("event"))
			{
				continue;
			}

			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const FString NodeClassPath = Node->GetClass()->GetPathName();
				if (NodeClassPath.Contains(TEXT("K2Node_Event")) || NodeClassPath.Contains(TEXT("K2Node_Input")))
				{
					const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					if (!NodeTitle.IsEmpty())
					{
						EntryPoints.Add(NodeTitle);
					}
				}
			}
		}

		EntryPoints.Sort();
		EntryPoints.SetNum(Algo::Unique(EntryPoints));
		return EntryPoints;
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	static TSharedPtr<FJsonObject> SerializePin(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
		PinObject->SetStringField(TEXT("pin_id"), Pin ? Pin->PinId.ToString() : FString());
		PinObject->SetStringField(TEXT("pin_name"), Pin ? Pin->PinName.ToString() : FString());
		PinObject->SetStringField(TEXT("direction"), Pin ? GetPinDirectionString(Pin->Direction) : TEXT("input"));

		if (Pin)
		{
			PinObject->SetStringField(TEXT("type_category"), Pin->PinType.PinCategory.ToString());
			PinObject->SetStringField(TEXT("type_subcategory"), Pin->PinType.PinSubCategory.ToString());
			PinObject->SetStringField(TEXT("type_subcategory_object"), Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
			PinObject->SetStringField(TEXT("container_type"), GetContainerTypeString(Pin->PinType));
			PinObject->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
			PinObject->SetBoolField(TEXT("is_const"), Pin->PinType.bIsConst);
			PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			PinObject->SetStringField(TEXT("default_object_path"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());

			TArray<TSharedPtr<FJsonValue>> LinkedToValues;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNodeUnchecked())
				{
					continue;
				}

				TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
				LinkObject->SetStringField(TEXT("target_node_guid"), LinkedPin->GetOwningNodeUnchecked()->NodeGuid.ToString());
				LinkObject->SetStringField(TEXT("target_pin_name"), LinkedPin->PinName.ToString());
				LinkedToValues.Add(MakeShared<FJsonValueObject>(LinkObject));
			}
			PinObject->SetArrayField(TEXT("linked_to"), LinkedToValues);
		}
		else
		{
			PinObject->SetStringField(TEXT("type_category"), FString());
			PinObject->SetStringField(TEXT("type_subcategory"), FString());
			PinObject->SetStringField(TEXT("type_subcategory_object"), FString());
			PinObject->SetStringField(TEXT("container_type"), TEXT("none"));
			PinObject->SetBoolField(TEXT("is_reference"), false);
			PinObject->SetBoolField(TEXT("is_const"), false);
			PinObject->SetStringField(TEXT("default_value"), FString());
			PinObject->SetStringField(TEXT("default_object_path"), FString());
			PinObject->SetArrayField(TEXT("linked_to"), TArray<TSharedPtr<FJsonValue>>());
		}

		return PinObject;
	}

	static TSharedPtr<FJsonObject> SerializeNode(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		const UK2Node* K2Node = Cast<UK2Node>(Node);

		NodeObject->SetStringField(TEXT("node_guid"), Node ? Node->NodeGuid.ToString() : FString());
		NodeObject->SetStringField(TEXT("node_class"), Node ? Node->GetClass()->GetPathName() : FString());
		NodeObject->SetStringField(TEXT("node_title"), Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
		NodeObject->SetStringField(TEXT("comment"), Node ? Node->NodeComment : FString());
		NodeObject->SetNumberField(TEXT("pos_x"), Node ? Node->NodePosX : 0);
		NodeObject->SetNumberField(TEXT("pos_y"), Node ? Node->NodePosY : 0);
		NodeObject->SetStringField(TEXT("enabled_state"), Node ? GetNodeEnabledStateString(Node->GetDesiredEnabledState()) : TEXT("unknown"));
		NodeObject->SetBoolField(TEXT("is_pure"), K2Node ? K2Node->IsNodePure() : false);

		TArray<TSharedPtr<FJsonValue>> PinValues;
		if (Node)
		{
			TArray<const UEdGraphPin*> SortedPins;
			SortedPins.Reserve(Node->Pins.Num());
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				SortedPins.Add(Pin);
			}

			SortedPins.Sort([](const UEdGraphPin& Left, const UEdGraphPin& Right)
			{
				if (Left.Direction != Right.Direction)
				{
					return Left.Direction < Right.Direction;
				}
				if (Left.PinName != Right.PinName)
				{
					return Left.PinName.LexicalLess(Right.PinName);
				}
				return Left.PinId < Right.PinId;
			});

			for (const UEdGraphPin* Pin : SortedPins)
			{
				PinValues.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
			}
		}

		NodeObject->SetArrayField(TEXT("pins"), PinValues);
		return NodeObject;
	}

	static TSharedPtr<FJsonObject> SerializeGraph(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
		GraphObject->SetStringField(TEXT("graph_type"), GetGraphType(Blueprint, Graph));
		GraphObject->SetStringField(TEXT("schema_class"), (Graph && Graph->GetSchema()) ? Graph->GetSchema()->GetClass()->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		if (Graph)
		{
			TArray<UEdGraphNode*> SortedNodes = Graph->Nodes;
			SortedNodes.RemoveAll([](const UEdGraphNode* Node) { return Node == nullptr; });
			SortedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				const FString LeftTitle = Left.GetNodeTitle(ENodeTitleType::ListView).ToString();
				const FString RightTitle = Right.GetNodeTitle(ENodeTitleType::ListView).ToString();
				const int32 TitleCompare = LeftTitle.Compare(RightTitle, ESearchCase::CaseSensitive);
				if (TitleCompare != 0)
				{
					return TitleCompare < 0;
				}
				return Left.NodeGuid < Right.NodeGuid;
			});

			GraphObject->SetNumberField(TEXT("node_count"), SortedNodes.Num());
			for (const UEdGraphNode* Node : SortedNodes)
			{
				NodeValues.Add(MakeShared<FJsonValueObject>(SerializeNode(Node)));
			}
		}
		else
		{
			GraphObject->SetNumberField(TEXT("node_count"), 0);
		}

		GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
		return GraphObject;
	}

	static TArray<FString> CollectPropertyReferenceTargets(const FProperty* Property, const void* Container, const int32 ArrayIndex)
	{
		TArray<FString> Targets;
		if (!Property || !Container)
		{
			return Targets;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const void* ValuePtr = ObjectProperty->ContainerPtrToValuePtr<void>(Container, ArrayIndex);
			if (const UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr))
			{
				Targets.Add(ReferencedObject->GetPathName());
			}
		}
		else if (CastField<FSoftObjectProperty>(Property) || CastField<FSoftClassProperty>(Property))
		{
			const FSoftObjectPtr* SoftObjectPtr = Property->ContainerPtrToValuePtr<FSoftObjectPtr>(Container, ArrayIndex);
			if (SoftObjectPtr && !SoftObjectPtr->ToSoftObjectPath().IsNull())
			{
				Targets.Add(SoftObjectPtr->ToSoftObjectPath().ToString());
			}
		}
		else if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			UClass* const* ClassValue = ClassProperty->ContainerPtrToValuePtr<UClass*>(Container, ArrayIndex);
			if (ClassValue && *ClassValue)
			{
				Targets.Add((*ClassValue)->GetPathName());
			}
		}

		Targets.Sort();
		Targets.SetNum(Algo::Unique(Targets));
		return Targets;
	}
	static TSharedPtr<FJsonObject> SerializeDataAsset(UDataAsset* DataAsset, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("asset_kind"), TEXT("data_asset"));
		AssetObject->SetStringField(TEXT("asset_path"), AssetPath);
		AssetObject->SetStringField(TEXT("asset_name"), DataAsset ? DataAsset->GetName() : FString());
		AssetObject->SetStringField(TEXT("asset_class"), DataAsset ? DataAsset->GetClass()->GetPathName() : FString());
		AssetObject->SetStringField(TEXT("parent_class_path"), (DataAsset && DataAsset->GetClass() && DataAsset->GetClass()->GetSuperClass()) ? DataAsset->GetClass()->GetSuperClass()->GetPathName() : FString());
		AssetObject->SetArrayField(TEXT("dependencies"), ToJsonStringArray(CollectDependencies(FName(*AssetPath))));

		TArray<TSharedPtr<FJsonValue>> PropertyValues;
		if (DataAsset)
		{
			for (TFieldIterator<FProperty> PropertyIterator(DataAsset->GetClass(), EFieldIterationFlags::IncludeSuper); PropertyIterator; ++PropertyIterator)
			{
				FProperty* Property = *PropertyIterator;
				if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}

				for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
				{
					FString ValueText;
					Property->ExportText_InContainer(ArrayIndex, ValueText, DataAsset, DataAsset, DataAsset, PPF_Copy);

					TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
					const FString PropertyName = Property->ArrayDim > 1
						? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), ArrayIndex)
						: Property->GetName();
					PropertyObject->SetStringField(TEXT("property_name"), PropertyName);
					PropertyObject->SetStringField(TEXT("property_type"), Property->GetCPPType());
					PropertyObject->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
					PropertyObject->SetStringField(TEXT("value_text"), ValueText);
					PropertyObject->SetArrayField(TEXT("reference_targets"), ToJsonStringArray(CollectPropertyReferenceTargets(Property, DataAsset, ArrayIndex)));
					PropertyValues.Add(MakeShared<FJsonValueObject>(PropertyObject));
				}
			}
		}

		AssetObject->SetArrayField(TEXT("properties"), PropertyValues);
		return AssetObject;
	}

	static TSharedPtr<FJsonObject> SerializeBlueprintAsset(UBlueprint* Blueprint, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("asset_kind"), TEXT("blueprint"));
		AssetObject->SetStringField(TEXT("asset_path"), AssetPath);
		AssetObject->SetStringField(TEXT("asset_name"), Blueprint ? Blueprint->GetName() : FString());
		AssetObject->SetStringField(TEXT("asset_class"), Blueprint ? Blueprint->GetClass()->GetName() : FString());

		const UEnum* BlueprintTypeEnum = StaticEnum<EBlueprintType>();
		const FString BlueprintTypeString = (Blueprint && BlueprintTypeEnum)
			? BlueprintTypeEnum->GetNameStringByValue(static_cast<int64>(Blueprint->BlueprintType))
			: FString();
		AssetObject->SetStringField(TEXT("blueprint_type"), BlueprintTypeString);
		AssetObject->SetStringField(TEXT("parent_class_path"), (Blueprint && Blueprint->ParentClass) ? Blueprint->ParentClass->GetPathName() : FString());
		AssetObject->SetStringField(TEXT("generated_class_path"), (Blueprint && Blueprint->GeneratedClass) ? Blueprint->GeneratedClass->GetPathName() : FString());
		AssetObject->SetArrayField(TEXT("implemented_interfaces"), ToJsonStringArray(CollectImplementedInterfaces(Blueprint)));
		AssetObject->SetArrayField(TEXT("dependencies"), ToJsonStringArray(CollectDependencies(FName(*AssetPath))));

		TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
		SummaryObject->SetArrayField(TEXT("message_nodes"), ToJsonStringArray(CollectMessageNodes(Blueprint)));
		SummaryObject->SetArrayField(TEXT("entry_points"), ToJsonStringArray(CollectEntryPoints(Blueprint)));
		AssetObject->SetObjectField(TEXT("summary"), SummaryObject);

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		for (UEdGraph* Graph : CollectGraphs(Blueprint))
		{
			GraphValues.Add(MakeShared<FJsonValueObject>(SerializeGraph(Blueprint, Graph)));
		}
		AssetObject->SetArrayField(TEXT("graphs"), GraphValues);
		AssetObject->SetNumberField(TEXT("graph_count"), GraphValues.Num());

		return AssetObject;
	}

	static bool ResolveSupportedAsset(UObject* Asset, FString& OutAssetPath, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("Input asset is null.");
			return false;
		}

		if (!Cast<UBlueprint>(Asset) && !Cast<UDataAsset>(Asset))
		{
			OutError = TEXT("Input asset is not a supported Blueprint or DataAsset.");
			return false;
		}

		OutAssetPath = Asset->GetPackage() ? Asset->GetPackage()->GetName() : FString();
		if (OutAssetPath.IsEmpty())
		{
			OutAssetPath = Asset->GetPathName();
		}

		return true;
	}

	static bool ResolveBlueprintObject(UObject* BlueprintAsset, UBlueprint*& OutBlueprint, FString& OutAssetPath, FString& OutError)
	{
		OutBlueprint = Cast<UBlueprint>(BlueprintAsset);
		if (!OutBlueprint)
		{
			OutError = TEXT("Input asset is not a Blueprint-derived asset.");
			return false;
		}

		OutAssetPath = BlueprintAsset->GetPackage() ? BlueprintAsset->GetPackage()->GetName() : FString();
		if (OutAssetPath.IsEmpty())
		{
			OutAssetPath = BlueprintAsset->GetPathName();
		}

		return true;
	}

	static TSharedPtr<FJsonObject> SerializeSupportedAsset(UObject* Asset, const FString& AssetPath)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			return SerializeBlueprintAsset(Blueprint, AssetPath);
		}
		if (UDataAsset* DataAsset = Cast<UDataAsset>(Asset))
		{
			return SerializeDataAsset(DataAsset, AssetPath);
		}
		return nullptr;
	}

	static TSharedRef<FJsonObject> BuildDocument(const FString& RootAssetPath, const TArray<TSharedPtr<FJsonObject>>& AssetObjects)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetStringField(TEXT("generated_at"), FDateTime::UtcNow().ToIso8601());
		RootObject->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		RootObject->SetStringField(TEXT("root_asset_path"), RootAssetPath);

		TArray<TSharedPtr<FJsonValue>> AssetValues;
		for (const TSharedPtr<FJsonObject>& AssetObject : AssetObjects)
		{
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}
		RootObject->SetArrayField(TEXT("assets"), AssetValues);
		return RootObject;
	}

	static TArray<FString> GetStringArray(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		TArray<FString> Result;
		if (!Object.IsValid())
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue))
			{
				Result.Add(StringValue);
			}
		}

		return Result;
	}

	static FString JoinAsBacktickList(const TArray<FString>& Values)
	{
		if (Values.IsEmpty())
		{
			return TEXT("<none>");
		}

		TArray<FString> Parts;
		for (const FString& Value : Values)
		{
			Parts.Add(FString::Printf(TEXT("`%s`"), *EscapeMarkdown(Value)));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	static void AppendStringListSection(TArray<FString>& Lines, const FString& Label, const TArray<FString>& Values)
	{
		Lines.Add(FString::Printf(TEXT("- %s: %s"), *Label, *JoinAsBacktickList(Values)));
	}
	static FString BuildBlueprintMarkdown(const TSharedPtr<FJsonObject>& AssetObject)
	{
		const FString AssetPath = AssetObject->GetStringField(TEXT("asset_path"));
		const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
		AssetObject->TryGetArrayField(TEXT("graphs"), GraphValues);

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("# `%s`"), *EscapeMarkdown(AssetPath)));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## Metadata"));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("- asset_kind: `blueprint`"));
		Lines.Add(FString::Printf(TEXT("- asset_name: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_name")))));
		Lines.Add(FString::Printf(TEXT("- asset_class: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_class")))));
		Lines.Add(FString::Printf(TEXT("- blueprint_type: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("blueprint_type")))));
		Lines.Add(FString::Printf(TEXT("- parent_class_path: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("parent_class_path")))));
		Lines.Add(FString::Printf(TEXT("- generated_class_path: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("generated_class_path")))));
		Lines.Add(FString::Printf(TEXT("- graph_count: `%d`"), GraphValues ? GraphValues->Num() : 0));
		AppendStringListSection(Lines, TEXT("implemented_interfaces"), GetStringArray(AssetObject, TEXT("implemented_interfaces")));
		AppendStringListSection(Lines, TEXT("dependencies"), GetStringArray(AssetObject, TEXT("dependencies")));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## Summary"));
		Lines.Add(TEXT(""));
		const TSharedPtr<FJsonObject>* SummaryObject = nullptr;
		if (AssetObject->TryGetObjectField(TEXT("summary"), SummaryObject) && SummaryObject && SummaryObject->IsValid())
		{
			AppendStringListSection(Lines, TEXT("message_nodes"), GetStringArray(*SummaryObject, TEXT("message_nodes")));
			AppendStringListSection(Lines, TEXT("entry_points"), GetStringArray(*SummaryObject, TEXT("entry_points")));
		}
		else
		{
			Lines.Add(TEXT("- message_nodes: <none>"));
			Lines.Add(TEXT("- entry_points: <none>"));
		}
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## Graphs"));
		Lines.Add(TEXT(""));

		if (GraphValues)
		{
			for (const TSharedPtr<FJsonValue>& GraphValue : *GraphValues)
			{
				const TSharedPtr<FJsonObject> GraphObject = GraphValue->AsObject();
				if (!GraphObject.IsValid())
				{
					continue;
				}

				Lines.Add(FString::Printf(TEXT("### `%s`"), *EscapeMarkdown(GraphObject->GetStringField(TEXT("graph_name")))));
				Lines.Add(TEXT(""));
				Lines.Add(FString::Printf(TEXT("- graph_type: `%s`"), *EscapeMarkdown(GraphObject->GetStringField(TEXT("graph_type")))));
				Lines.Add(FString::Printf(TEXT("- schema_class: `%s`"), *EscapeMarkdown(GraphObject->GetStringField(TEXT("schema_class")))));
				Lines.Add(FString::Printf(TEXT("- node_count: `%d`"), static_cast<int32>(GraphObject->GetNumberField(TEXT("node_count")))));
				Lines.Add(TEXT(""));

				const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
				if (GraphObject->TryGetArrayField(TEXT("nodes"), NodeValues) && NodeValues)
				{
					int32 NodeIndex = 0;
					for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
					{
						++NodeIndex;
						const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
						if (!NodeObject.IsValid())
						{
							continue;
						}

						Lines.Add(FString::Printf(TEXT("#### Node %03d - `%s`"), NodeIndex, *EscapeMarkdown(NodeObject->GetStringField(TEXT("node_title")))));
						Lines.Add(TEXT(""));
						Lines.Add(FString::Printf(TEXT("- node_guid: `%s`"), *EscapeMarkdown(NodeObject->GetStringField(TEXT("node_guid")))));
						Lines.Add(FString::Printf(TEXT("- node_class: `%s`"), *EscapeMarkdown(NodeObject->GetStringField(TEXT("node_class")))));
						Lines.Add(FString::Printf(TEXT("- pos: `(%d, %d)`"), static_cast<int32>(NodeObject->GetNumberField(TEXT("pos_x"))), static_cast<int32>(NodeObject->GetNumberField(TEXT("pos_y")))));
						Lines.Add(FString::Printf(TEXT("- enabled_state: `%s`"), *EscapeMarkdown(NodeObject->GetStringField(TEXT("enabled_state")))));
						Lines.Add(FString::Printf(TEXT("- is_pure: `%s`"), NodeObject->GetBoolField(TEXT("is_pure")) ? TEXT("true") : TEXT("false")));
						const FString Comment = NormalizeMultilineForMarkdown(NodeObject->GetStringField(TEXT("comment")));
						Lines.Add(FString::Printf(TEXT("- comment: %s"), Comment.IsEmpty() ? TEXT("<none>") : *EscapeMarkdown(Comment).Replace(TEXT("\n"), TEXT("<br>"))));

						const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
						if (NodeObject->TryGetArrayField(TEXT("pins"), PinValues) && PinValues && PinValues->Num() > 0)
						{
							Lines.Add(FString::Printf(TEXT("- pin_count: `%d`"), PinValues->Num()));
							Lines.Add(TEXT("- pins:"));
							for (const TSharedPtr<FJsonValue>& PinValue : *PinValues)
							{
								const TSharedPtr<FJsonObject> PinObject = PinValue->AsObject();
								if (!PinObject.IsValid())
								{
									continue;
								}

								TArray<FString> Extras;
								Extras.Add(FString::Printf(TEXT("container=%s"), *PinObject->GetStringField(TEXT("container_type"))));
								Extras.Add(FString::Printf(TEXT("ref=%s"), PinObject->GetBoolField(TEXT("is_reference")) ? TEXT("true") : TEXT("false")));
								Extras.Add(FString::Printf(TEXT("const=%s"), PinObject->GetBoolField(TEXT("is_const")) ? TEXT("true") : TEXT("false")));
								if (!PinObject->GetStringField(TEXT("type_subcategory_object")).IsEmpty())
								{
									Extras.Add(FString::Printf(TEXT("subobj=%s"), *PinObject->GetStringField(TEXT("type_subcategory_object"))));
								}
								if (!PinObject->GetStringField(TEXT("default_value")).IsEmpty())
								{
									Extras.Add(FString::Printf(TEXT("default=\"%s\""), *NormalizeMultilineForMarkdown(PinObject->GetStringField(TEXT("default_value"))).Replace(TEXT("\n"), TEXT("\\n"))));
								}
								if (!PinObject->GetStringField(TEXT("default_object_path")).IsEmpty())
								{
									Extras.Add(FString::Printf(TEXT("default_object=%s"), *PinObject->GetStringField(TEXT("default_object_path"))));
								}

								TArray<FString> LinkStrings;
								const TArray<TSharedPtr<FJsonValue>>* LinkedToValues = nullptr;
								if (PinObject->TryGetArrayField(TEXT("linked_to"), LinkedToValues) && LinkedToValues)
								{
									for (const TSharedPtr<FJsonValue>& LinkValue : *LinkedToValues)
									{
										const TSharedPtr<FJsonObject> LinkObject = LinkValue->AsObject();
										if (LinkObject.IsValid())
										{
											LinkStrings.Add(FString::Printf(TEXT("%s:%s"), *LinkObject->GetStringField(TEXT("target_node_guid")), *LinkObject->GetStringField(TEXT("target_pin_name"))));
										}
									}
								}

								const FString TypeText = FString::Printf(TEXT("%s/%s"), *PinObject->GetStringField(TEXT("type_category")), *PinObject->GetStringField(TEXT("type_subcategory")));
								Lines.Add(FString::Printf(TEXT("  - `[%s] %s` `%s` %s links=%s"), *EscapeMarkdown(PinObject->GetStringField(TEXT("direction"))), *EscapeMarkdown(PinObject->GetStringField(TEXT("pin_name"))), *EscapeMarkdown(TypeText), *EscapeMarkdown(FString::Join(Extras, TEXT(" "))), LinkStrings.IsEmpty() ? TEXT("<none>") : *EscapeMarkdown(FString::Join(LinkStrings, TEXT(", ")))));
							}
						}
						else
						{
							Lines.Add(TEXT("- pin_count: `0`"));
						}
						Lines.Add(TEXT(""));
					}
				}
			}
		}

		return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	}

	static FString BuildDataAssetMarkdown(const TSharedPtr<FJsonObject>& AssetObject)
	{
		const FString AssetPath = AssetObject->GetStringField(TEXT("asset_path"));
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("# `%s`"), *EscapeMarkdown(AssetPath)));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## Metadata"));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("- asset_kind: `data_asset`"));
		Lines.Add(FString::Printf(TEXT("- asset_name: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_name")))));
		Lines.Add(FString::Printf(TEXT("- asset_class: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_class")))));
		Lines.Add(FString::Printf(TEXT("- parent_class_path: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("parent_class_path")))));
		AppendStringListSection(Lines, TEXT("dependencies"), GetStringArray(AssetObject, TEXT("dependencies")));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## Properties"));
		Lines.Add(TEXT(""));

		const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
		if (AssetObject->TryGetArrayField(TEXT("properties"), PropertyValues) && PropertyValues && PropertyValues->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertyValues)
			{
				const TSharedPtr<FJsonObject> PropertyObject = PropertyValue->AsObject();
				if (!PropertyObject.IsValid())
				{
					continue;
				}

				Lines.Add(FString::Printf(TEXT("### `%s`"), *EscapeMarkdown(PropertyObject->GetStringField(TEXT("property_name")))));
				Lines.Add(TEXT(""));
				Lines.Add(FString::Printf(TEXT("- type: `%s`"), *EscapeMarkdown(PropertyObject->GetStringField(TEXT("property_type")))));
				Lines.Add(FString::Printf(TEXT("- property_class: `%s`"), *EscapeMarkdown(PropertyObject->GetStringField(TEXT("property_class")))));
				AppendStringListSection(Lines, TEXT("reference_targets"), GetStringArray(PropertyObject, TEXT("reference_targets")));
				Lines.Add(TEXT("- value:"));
				Lines.Add(TEXT("```text"));
				Lines.Add(NormalizeMultilineForMarkdown(PropertyObject->GetStringField(TEXT("value_text"))).Replace(TEXT("```"), TEXT("'''")));
				Lines.Add(TEXT("```"));
				Lines.Add(TEXT(""));
			}
		}
		else
		{
			Lines.Add(TEXT("No editable properties were exported."));
			Lines.Add(TEXT(""));
		}

		return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	}

	static FString BuildAssetMarkdown(const TSharedPtr<FJsonObject>& AssetObject)
	{
		if (!AssetObject.IsValid())
		{
			return FString();
		}

		return AssetObject->GetStringField(TEXT("asset_kind")) == TEXT("data_asset")
			? BuildDataAssetMarkdown(AssetObject)
			: BuildBlueprintMarkdown(AssetObject);
	}

	static bool IsValidRootAssetPath(const FString& RootAssetPath, FString& OutError)
	{
		FText PathValidationError;
		if (!FPackageName::IsValidLongPackageName(RootAssetPath, false, &PathValidationError))
		{
			OutError = FString::Printf(TEXT("Invalid root asset path: %s (%s)"), *RootAssetPath, *PathValidationError.ToString());
			return false;
		}
		return true;
	}

	static TArray<FAssetData> CollectSupportedAssetDataUnderRoot(const FString& RootAssetPath, const bool bRecursive, FString& OutError)
	{
		TArray<FAssetData> SupportedAssets;
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		if (!AssetRegistry.PathExists(FName(*RootAssetPath)))
		{
			OutError = FString::Printf(TEXT("Root asset path does not exist: %s"), *RootAssetPath);
			return SupportedAssets;
		}

		TArray<FAssetData> AssetDataList;
		if (!AssetRegistry.GetAssetsByPath(FName(*RootAssetPath), AssetDataList, bRecursive, false))
		{
			OutError = FString::Printf(TEXT("Failed to enumerate assets under path: %s"), *RootAssetPath);
			return SupportedAssets;
		}

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (UObject* Asset = AssetData.GetAsset())
			{
				if (Cast<UBlueprint>(Asset) || Cast<UDataAsset>(Asset))
				{
					SupportedAssets.Add(AssetData);
				}
			}
		}

		SupportedAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			if (Left.PackageName != Right.PackageName)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			}
			return Left.AssetName.LexicalLess(Right.AssetName);
		});
		return SupportedAssets;
	}

	static bool ExportAssetMarkdownOnly(UObject* Asset, const FString& OutputPath, FString& OutResolvedPath, FString& OutError)
	{
		FString AssetPath;
		if (!ResolveSupportedAsset(Asset, AssetPath, OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> AssetObject = SerializeSupportedAsset(Asset, AssetPath);
		if (!AssetObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to serialize asset: %s"), *AssetPath);
			return false;
		}

		const FString Markdown = BuildAssetMarkdown(AssetObject);
		const FString ResolvedOutputPath = OutputPath.IsEmpty() ? GetMarkdownPathForAsset(AssetPath, GetDefaultDocumentationRootDir()) : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(OutputPath);
		return SaveTextToPath(Markdown, ResolvedOutputPath, OutResolvedPath, OutError);
	}

	static FString BuildDocumentationIndexMarkdown(const FString& DocumentationRootDir, const TArray<TSharedPtr<FJsonObject>>& AssetObjects)
	{
		TArray<FString> Lines;
		Lines.Add(TEXT("# Asset Documentation Index"));
		Lines.Add(TEXT(""));
		Lines.Add(FString::Printf(TEXT("- generated_at: `%s`"), *FDateTime::UtcNow().ToIso8601()));
		Lines.Add(FString::Printf(TEXT("- docs_root: `%s`"), *EscapeMarkdown(DocumentationRootDir)));
		Lines.Add(FString::Printf(TEXT("- asset_count: `%d`"), AssetObjects.Num()));
		Lines.Add(TEXT(""));

		const FString RootFolderName = FPaths::GetCleanFilename(DocumentationRootDir);
		for (const TSharedPtr<FJsonObject>& AssetObject : AssetObjects)
		{
			if (!AssetObject.IsValid())
			{
				continue;
			}

			const FString AssetPath = AssetObject->GetStringField(TEXT("asset_path"));
			const FString LinkPath = FString::Printf(TEXT("%s/%s"), *RootFolderName, *GetRelativeMirrorPath(AssetPath, TEXT(".md")));
			Lines.Add(FString::Printf(TEXT("## [`%s`](%s)"), *EscapeMarkdown(AssetPath), *LinkPath.Replace(TEXT("\\"), TEXT("/"))));
			Lines.Add(TEXT(""));
			Lines.Add(FString::Printf(TEXT("- asset_kind: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_kind")))));
			Lines.Add(FString::Printf(TEXT("- asset_class: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("asset_class")))));
			Lines.Add(FString::Printf(TEXT("- parent_class_path: `%s`"), *EscapeMarkdown(AssetObject->GetStringField(TEXT("parent_class_path")))));
			AppendStringListSection(Lines, TEXT("dependencies"), GetStringArray(AssetObject, TEXT("dependencies")));
			if (AssetObject->GetStringField(TEXT("asset_kind")) == TEXT("blueprint"))
			{
				AppendStringListSection(Lines, TEXT("implemented_interfaces"), GetStringArray(AssetObject, TEXT("implemented_interfaces")));
				const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
				AssetObject->TryGetArrayField(TEXT("graphs"), Graphs);
				Lines.Add(FString::Printf(TEXT("- graph_count: `%d`"), Graphs ? Graphs->Num() : 0));
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
				AssetObject->TryGetArrayField(TEXT("properties"), Properties);
				Lines.Add(FString::Printf(TEXT("- property_count: `%d`"), Properties ? Properties->Num() : 0));
			}
			Lines.Add(TEXT(""));
		}

		return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	}
}
FString UBlueprintGraphExportLibrary::GetBlueprintGraphJson(UObject* BlueprintAsset, bool bPrettyPrint)
{
	UBlueprint* Blueprint = nullptr;
	FString AssetPath;
	FString Error;
	if (!BlueprintGraphExport::ResolveBlueprintObject(BlueprintAsset, Blueprint, AssetPath, Error))
	{
		UE_LOG(LogBlueprintGraphExport, Warning, TEXT("%s"), *Error);
		return FString();
	}

	FString Json;
	const TSharedPtr<FJsonObject> AssetObject = BlueprintGraphExport::SerializeBlueprintAsset(Blueprint, AssetPath);
	if (!BlueprintGraphExport::SerializeJsonObject(AssetObject.ToSharedRef(), bPrettyPrint, Json))
	{
		UE_LOG(LogBlueprintGraphExport, Warning, TEXT("Failed to serialize blueprint graph JSON for %s"), *AssetPath);
		return FString();
	}

	return Json;
}

bool UBlueprintGraphExportLibrary::ExportBlueprintAssetToJson(
	UObject* BlueprintAsset,
	const FString& OutputPath,
	bool bPrettyPrint,
	FString& OutResolvedPath,
	FString& OutError
)
{
	OutResolvedPath.Reset();
	OutError.Reset();

	UBlueprint* Blueprint = nullptr;
	FString AssetPath;
	if (!BlueprintGraphExport::ResolveBlueprintObject(BlueprintAsset, Blueprint, AssetPath, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonObject>> Assets = { BlueprintGraphExport::SerializeBlueprintAsset(Blueprint, AssetPath) };
	const TSharedRef<FJsonObject> Document = BlueprintGraphExport::BuildDocument(AssetPath, Assets);

	FString Json;
	if (!BlueprintGraphExport::SerializeJsonObject(Document, bPrettyPrint, Json))
	{
		OutError = FString::Printf(TEXT("Failed to serialize JSON for asset %s"), *AssetPath);
		return false;
	}

	const FString ResolvedOutputPath = OutputPath.IsEmpty() ? BlueprintGraphExport::GetDefaultAggregateOutputPath() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(OutputPath);
	return BlueprintGraphExport::SaveJsonToPath(Json, ResolvedOutputPath, OutResolvedPath, OutError);
}

bool UBlueprintGraphExportLibrary::ExportBlueprintsUnderPathToJson(
	const FString& RootAssetPath,
	const FString& OutputPath,
	bool bRecursive,
	bool bPrettyPrint,
	FString& OutResolvedPath,
	FString& OutError
)
{
	OutResolvedPath.Reset();
	OutError.Reset();

	if (!BlueprintGraphExport::IsValidRootAssetPath(RootAssetPath, OutError))
	{
		return false;
	}

	TArray<FAssetData> AssetDataList = BlueprintGraphExport::CollectSupportedAssetDataUnderRoot(RootAssetPath, bRecursive, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}

	TArray<TSharedPtr<FJsonObject>> AssetObjects;
	for (const FAssetData& AssetData : AssetDataList)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
		{
			AssetObjects.Add(BlueprintGraphExport::SerializeBlueprintAsset(Blueprint, AssetData.PackageName.ToString()));
		}
	}

	const TSharedRef<FJsonObject> Document = BlueprintGraphExport::BuildDocument(RootAssetPath, AssetObjects);

	FString Json;
	if (!BlueprintGraphExport::SerializeJsonObject(Document, bPrettyPrint, Json))
	{
		OutError = FString::Printf(TEXT("Failed to serialize JSON for assets under %s"), *RootAssetPath);
		return false;
	}

	const FString ResolvedOutputPath = OutputPath.IsEmpty() ? BlueprintGraphExport::GetDefaultAggregateOutputPath() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(OutputPath);
	return BlueprintGraphExport::SaveJsonToPath(Json, ResolvedOutputPath, OutResolvedPath, OutError);
}

bool UBlueprintGraphExportLibrary::ExportAssetToMarkdown(
	UObject* Asset,
	const FString& OutputPath,
	bool& bOutSupported,
	FString& OutResolvedPath,
	FString& OutError
)
{
	OutResolvedPath.Reset();
	OutError.Reset();
	bOutSupported = false;

	FString AssetPath;
	if (!BlueprintGraphExport::ResolveSupportedAsset(Asset, AssetPath, OutError))
	{
		return false;
	}

	bOutSupported = true;
	return BlueprintGraphExport::ExportAssetMarkdownOnly(Asset, OutputPath, OutResolvedPath, OutError);
}

bool UBlueprintGraphExportLibrary::ExportAssetsUnderPathToMarkdown(
	const FString& RootAssetPath,
	const FString& OutputRootDir,
	bool bRecursive,
	FString& OutResolvedDir,
	FString& OutError
)
{
	OutResolvedDir.Reset();
	OutError.Reset();

	if (!BlueprintGraphExport::IsValidRootAssetPath(RootAssetPath, OutError))
	{
		return false;
	}

	const FString DocumentationRootDir = OutputRootDir.IsEmpty() ? BlueprintGraphExport::GetDefaultDocumentationRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(OutputRootDir);
	if (!IFileManager::Get().MakeDirectory(*DocumentationRootDir, true))
	{
		OutError = FString::Printf(TEXT("Failed to create documentation root directory: %s"), *DocumentationRootDir);
		return false;
	}

	TArray<FAssetData> AssetDataList = BlueprintGraphExport::CollectSupportedAssetDataUnderRoot(RootAssetPath, bRecursive, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	const bool bPrettyPrintJson = Settings ? Settings->bPrettyPrintJson : true;

	for (const FAssetData& AssetData : AssetDataList)
	{
		if (UObject* Asset = AssetData.GetAsset())
		{
			FString MarkdownPath;
			FString JsonPath;
			FString Error;
			if (!ExportAssetDocumentationBundle(Asset, DocumentationRootDir, FString(), bPrettyPrintJson, MarkdownPath, JsonPath, Error) && !Error.IsEmpty())
			{
				UE_LOG(LogBlueprintGraphExport, Warning, TEXT("%s"), *Error);
			}
		}
	}

	FString IndexPath;
	FString IndexError;
	RebuildDocumentationIndex(DocumentationRootDir, IndexPath, IndexError);
	if (!IndexError.IsEmpty())
	{
		UE_LOG(LogBlueprintGraphExport, Warning, TEXT("%s"), *IndexError);
	}

	OutResolvedDir = DocumentationRootDir;
	return true;
}

bool UBlueprintGraphExportLibrary::RebuildDocumentationIndex(
	const FString& DocsRootDir,
	FString& OutIndexPath,
	FString& OutError
)
{
	OutIndexPath.Reset();
	OutError.Reset();

	const FString DocumentationRootDir = DocsRootDir.IsEmpty() ? BlueprintGraphExport::GetDefaultDocumentationRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(DocsRootDir);
	if (!IFileManager::Get().MakeDirectory(*DocumentationRootDir, true))
	{
		OutError = FString::Printf(TEXT("Failed to create documentation root directory: %s"), *DocumentationRootDir);
		return false;
	}

	const UBlueprintGraphExportSettings* Settings = GetDefault<UBlueprintGraphExportSettings>();
	const TArray<FString> RootPaths = Settings ? Settings->RootAssetPaths : TArray<FString>{ TEXT("/Game") };

	TArray<TSharedPtr<FJsonObject>> AssetObjects;
	for (const FString& RootPath : RootPaths)
	{
		FString RootError;
		if (!BlueprintGraphExport::IsValidRootAssetPath(RootPath, RootError))
		{
			UE_LOG(LogBlueprintGraphExport, Warning, TEXT("%s"), *RootError);
			continue;
		}

		TArray<FAssetData> AssetDataList = BlueprintGraphExport::CollectSupportedAssetDataUnderRoot(RootPath, true, RootError);
		if (!RootError.IsEmpty())
		{
			UE_LOG(LogBlueprintGraphExport, Warning, TEXT("%s"), *RootError);
			continue;
		}

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (UObject* Asset = AssetData.GetAsset())
			{
				const TSharedPtr<FJsonObject> AssetObject = BlueprintGraphExport::SerializeSupportedAsset(Asset, AssetData.PackageName.ToString());
				if (AssetObject.IsValid())
				{
					AssetObjects.Add(AssetObject);
				}
			}
		}
	}

	AssetObjects.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		if (!Left.IsValid())
		{
			return false;
		}
		if (!Right.IsValid())
		{
			return true;
		}
		return Left->GetStringField(TEXT("asset_path")) < Right->GetStringField(TEXT("asset_path"));
	});

	const FString IndexMarkdown = BlueprintGraphExport::BuildDocumentationIndexMarkdown(DocumentationRootDir, AssetObjects);
	return BlueprintGraphExport::SaveTextToPath(IndexMarkdown, BlueprintGraphExport::GetIndexPath(DocumentationRootDir), OutIndexPath, OutError);
}

bool UBlueprintGraphExportLibrary::ExportAssetDocumentationBundle(
	UObject* Asset,
	const FString& DocumentationRootDir,
	const FString& JsonOutputDir,
	bool bPrettyPrintJson,
	FString& OutMarkdownPath,
	FString& OutJsonPath,
	FString& OutError
)
{
	OutMarkdownPath.Reset();
	OutJsonPath.Reset();
	OutError.Reset();

	FString AssetPath;
	if (!BlueprintGraphExport::ResolveSupportedAsset(Asset, AssetPath, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> AssetObject = BlueprintGraphExport::SerializeSupportedAsset(Asset, AssetPath);
	if (!AssetObject.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to serialize asset: %s"), *AssetPath);
		return false;
	}

	const FString ResolvedDocsRoot = DocumentationRootDir.IsEmpty() ? BlueprintGraphExport::GetDefaultDocumentationRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(DocumentationRootDir);
	const FString ResolvedJsonRoot = JsonOutputDir.IsEmpty() ? BlueprintGraphExport::GetDefaultJsonMirrorRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(JsonOutputDir);

	const FString Markdown = BlueprintGraphExport::BuildAssetMarkdown(AssetObject);
	if (!BlueprintGraphExport::SaveTextToPath(Markdown, BlueprintGraphExport::GetMarkdownPathForAsset(AssetPath, ResolvedDocsRoot), OutMarkdownPath, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonObject>> AssetObjects = { AssetObject };
	const TSharedRef<FJsonObject> Document = BlueprintGraphExport::BuildDocument(AssetPath, AssetObjects);
	FString Json;
	if (!BlueprintGraphExport::SerializeJsonObject(Document, bPrettyPrintJson, Json))
	{
		OutError = FString::Printf(TEXT("Failed to serialize JSON for asset %s"), *AssetPath);
		return false;
	}

	return BlueprintGraphExport::SaveJsonToPath(Json, BlueprintGraphExport::GetJsonPathForAsset(AssetPath, ResolvedJsonRoot), OutJsonPath, OutError);
}

bool UBlueprintGraphExportLibrary::RemoveAssetDocumentationBundle(
	const FString& AssetPackagePath,
	const FString& DocumentationRootDir,
	const FString& JsonOutputDir,
	FString& OutRemovedMarkdownPath,
	FString& OutRemovedJsonPath,
	FString& OutError
)
{
	OutRemovedMarkdownPath.Reset();
	OutRemovedJsonPath.Reset();
	OutError.Reset();

	if (AssetPackagePath.IsEmpty())
	{
		OutError = TEXT("Asset package path is empty.");
		return false;
	}

	const FString ResolvedDocsRoot = DocumentationRootDir.IsEmpty() ? BlueprintGraphExport::GetDefaultDocumentationRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(DocumentationRootDir);
	const FString ResolvedJsonRoot = JsonOutputDir.IsEmpty() ? BlueprintGraphExport::GetDefaultJsonMirrorRootDir() : BlueprintGraphExportPathUtils::ResolvePathAgainstOutputBase(JsonOutputDir);
	const FString MarkdownPath = BlueprintGraphExport::GetMarkdownPathForAsset(AssetPackagePath, ResolvedDocsRoot);
	const FString JsonPath = BlueprintGraphExport::GetJsonPathForAsset(AssetPackagePath, ResolvedJsonRoot);

	IFileManager& FileManager = IFileManager::Get();
	if (FileManager.FileExists(*MarkdownPath))
	{
		if (!FileManager.Delete(*MarkdownPath, false, true, true))
		{
			OutError = FString::Printf(TEXT("Failed to delete markdown file: %s"), *MarkdownPath);
			return false;
		}
		OutRemovedMarkdownPath = MarkdownPath;
	}

	if (FileManager.FileExists(*JsonPath))
	{
		if (!FileManager.Delete(*JsonPath, false, true, true))
		{
			OutError = FString::Printf(TEXT("Failed to delete JSON file: %s"), *JsonPath);
			return false;
		}
		OutRemovedJsonPath = JsonPath;
	}

	return true;
}





