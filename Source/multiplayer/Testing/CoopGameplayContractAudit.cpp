// Copyright Epic Games, Inc. All Rights Reserved.
// 编辑器显式只读审计：落盘蓝图的调用/写入清单 + SkipSave 编译；不是运行、画面或通关验收。

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogCoopContractAudit, Log, All);

namespace
{
	// 清单保留节点位置和成员声明者，供人工核对状态归属；不是可达性分析或第二套蓝图解释器。
	TSharedRef<FJsonObject> AuditNode(const UEdGraph& Graph, const UEdGraphNode& Node)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("graph"), Graph.GetPathName());
		Row->SetStringField(TEXT("node"), Node.GetName());
		Row->SetStringField(TEXT("nodeClass"), Node.GetClass()->GetPathName());
		Row->SetStringField(TEXT("title"), Node.GetNodeTitle(ENodeTitleType::ListView).ToString());
		Row->SetStringField(TEXT("guid"), Node.NodeGuid.ToString());
		return Row;
	}

	void AuditBlueprint(UBlueprint& Blueprint, TArray<TSharedPtr<FJsonValue>>& Rows, int32& Errors, int32& Warnings)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset"), Blueprint.GetPathName());
		Row->SetStringField(TEXT("parent"), GetPathNameSafe(Blueprint.ParentClass));
		Row->SetStringField(TEXT("generatedClassBeforeCompile"), GetPathNameSafe(Blueprint.GeneratedClass));
		Row->SetStringField(TEXT("inventoryStage"), TEXT("LoadedSavedGraphsBeforeExplicitCompile"));
		TArray<TSharedPtr<FJsonValue>> Calls, Writes, Events;
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
				{
					auto Item = AuditNode(*Graph, *Node);
					const UFunction* Function = Call->GetTargetFunction();
					Item->SetStringField(TEXT("member"), Call->FunctionReference.GetMemberName().ToString());
					Item->SetStringField(TEXT("resolvedFunction"), GetPathNameSafe(Function));
					Item->SetBoolField(TEXT("resolved"), Function != nullptr);
					Item->SetBoolField(TEXT("native"), Function && Function->HasAnyFunctionFlags(FUNC_Native));
					Item->SetBoolField(TEXT("projectNativeOwner"), Function && Function->GetOutermost()->GetName() == TEXT("/Script/multiplayer"));
					Item->SetBoolField(TEXT("authorityOnly"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintAuthorityOnly));
					Calls.Add(MakeShared<FJsonValueObject>(Item));
				}
				if (const auto* Set = Cast<UK2Node_VariableSet>(Node))
				{
					auto Item = AuditNode(*Graph, *Node);
					const FProperty* Property = Set->GetPropertyForVariable();
					Item->SetStringField(TEXT("member"), Set->GetVarNameString());
					Item->SetStringField(TEXT("resolvedProperty"), Property ? Property->GetPathName() : FString());
					Item->SetStringField(TEXT("owner"), Property && Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetPathName() : FString());
					Item->SetBoolField(TEXT("resolved"), Property != nullptr);
					Item->SetBoolField(TEXT("replicated"), Property && Property->HasAnyPropertyFlags(CPF_Net));
					Writes.Add(MakeShared<FJsonValueObject>(Item));
				}
				if (const auto* Event = Cast<UK2Node_Event>(Node))
				{
					auto Item = AuditNode(*Graph, *Node);
					Item->SetStringField(TEXT("member"), Event->EventReference.GetMemberName().ToString());
					Item->SetStringField(TEXT("signature"), GetPathNameSafe(Event->FindEventSignatureFunction()));
					Events.Add(MakeShared<FJsonValueObject>(Item));
				}
				if (Cast<UK2Node_FunctionEntry>(Node))
				{
					auto Item = AuditNode(*Graph, *Node);
					Item->SetStringField(TEXT("member"), Graph->GetName());
					Events.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}
		Row->SetArrayField(TEXT("functionCalls"), Calls);
		Row->SetArrayField(TEXT("variableSets"), Writes);
		Row->SetArrayField(TEXT("eventEntries"), Events);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipSave, &Results);
		Errors += Results.NumErrors;
		Warnings += Results.NumWarnings;
		const bool bCompiled = Results.NumErrors == 0 && Blueprint.GeneratedClass && Blueprint.Status != BS_Error;
		if (!bCompiled && Results.NumErrors == 0) ++Errors;
		Row->SetBoolField(TEXT("compiled"), bCompiled);
		Row->SetNumberField(TEXT("compileErrors"), Results.NumErrors);
		Row->SetNumberField(TEXT("compileWarnings"), Results.NumWarnings);
		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const auto& Message : Results.Messages) Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
		Row->SetArrayField(TEXT("compilerMessages"), Messages);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		UE_LOG(LogCoopContractAudit, Display, TEXT("COOP_CONTRACT asset=%s compiled=%d errors=%d warnings=%d calls=%d writes=%d events=%d"),
			*Blueprint.GetPathName(), bCompiled, Results.NumErrors, Results.NumWarnings, Calls.Num(), Writes.Num(), Events.Num());
	}

	void RunCoopGameplayContractAudit(const TArray<FString>& Args)
	{
		if (Args.Num() > 1 || (GEditor && GEditor->PlayWorld))
		{
			UE_LOG(LogCoopContractAudit, Error, TEXT("Run outside PIE: coop.AuditGameplayContracts [Report.json]"));
			return;
		}
		const FString Output = Args.IsEmpty()
			? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GameplayContractAudit"), FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT("Report.json"))
			: FPaths::ConvertRelativePathToFull(Args[0].TrimStartAndEnd().TrimQuotes());
		if (IFileManager::Get().FileExists(*Output))
		{
			UE_LOG(LogCoopContractAudit, Error, TEXT("Refusing to replace an existing audit: %s"), *Output);
			return;
		}
		auto Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("scope"), TEXT("All Blueprints recursively under /Game/Combat, /Game/UI and /Game/ThirdPerson/Blueprints; saved CoopEscape/DSMenu level scripts only"));
		Report->SetStringField(TEXT("interpretation"), TEXT("Static saved-graph inventory and SkipSave compilation only. Nodes may be disconnected. No gameplay, weak-network, visual, complete playthrough or vendor-asset-health certification."));
		Report->SetBoolField(TEXT("saveRequested"), false);
		int32 Errors = 0, Warnings = 0;
		TArray<TSharedPtr<FJsonValue>> Rows, Failures, Files, Maps;
		TMap<FString, FMD5Hash> BeforeHashes;
		TSet<FName> InitiallyDirtyPackages;
		const auto RecordFile = [&](const FString& Package, bool bMap)
		{
			if (const UPackage* Existing = FindPackage(nullptr, *Package); Existing && Existing->IsDirty())
				InitiallyDirtyPackages.Add(FName(*Package));
			const FString File = FPackageName::LongPackageNameToFilename(Package,
				bMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
			BeforeHashes.FindOrAdd(File) = FMD5Hash::HashFile(*File);
		};
		const auto Fail = [&](const FString& Path)
		{
			++Errors;
			Failures.Add(MakeShared<FJsonValueString>(Path));
			UE_LOG(LogCoopContractAudit, Error, TEXT("Failed to load expected contract asset: %s"), *Path);
		};
		auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const TArray<FString> Paths{ TEXT("/Game/Combat"), TEXT("/Game/UI"), TEXT("/Game/ThirdPerson/Blueprints") };
		Registry.ScanPathsSynchronous(Paths, false);
		FARFilter Filter;
		for (const FString& Path : Paths) Filter.PackagePaths.Add(FName(*Path));
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		const TArray<FString> MapPaths{ TEXT("/Game/Combat/Maps/CoopEscape"), TEXT("/Game/UI/DSMenu") };
		// 在任何显式编译之前记录全部受审包，避免漏掉编译依赖时对后续包的意外磁盘变化。
		for (const FAssetData& Asset : Assets) RecordFile(Asset.PackageName.ToString(), false);
		for (const FString& Map : MapPaths) RecordFile(Map, true);
		TSet<FName> FoundPackages;
		TArray<TStrongObjectPtr<UBlueprint>> Blueprints;
		for (const FAssetData& Asset : Assets)
		{
			FoundPackages.Add(Asset.PackageName);
			if (InitiallyDirtyPackages.Contains(Asset.PackageName))
			{
				Fail(TEXT("Unsaved in-memory package; use a fresh audit process: ") + Asset.PackageName.ToString());
				continue;
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			if (!Blueprint) { Fail(Asset.GetSoftObjectPath().ToString()); continue; }
			Blueprints.Emplace(Blueprint);
			AuditBlueprint(*Blueprint, Rows, Errors, Warnings);
		}
		// 不能把空清单或漏掉核心入口算成审计通过。
		for (const TCHAR* Required : { TEXT("/Game/UI/BP_CoopGameState"), TEXT("/Game/UI/BP_CoopHUD"),
			TEXT("/Game/UI/BP_MenuController"), TEXT("/Game/UI/BP_MenuGameMode"), TEXT("/Game/UI/WBPmainmenu"), TEXT("/Game/UI/winandquit"),
			TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"), TEXT("/Game/Combat/BP_CombatGameMode"),
			TEXT("/Game/Combat/Presentation/BP_CombatPlayer"), TEXT("/Game/Combat/Presentation/BP_CombatEnemy") })
			if (!FoundPackages.Contains(FName(Required))) Fail(Required);
		for (const FString& Map : MapPaths)
		{
			if (InitiallyDirtyPackages.Contains(FName(*Map))) { Fail(TEXT("Unsaved in-memory map: ") + Map); continue; }
			TStrongObjectPtr<UWorld> World(LoadObject<UWorld>(nullptr, *Map));
			if (!World.IsValid() || !World->PersistentLevel) { Fail(Map); continue; }
			// true = 不创建缺失的 LevelScriptBlueprint；加载不初始化或运行关卡。
			UBlueprint* Script = World->PersistentLevel->GetLevelScriptBlueprint(true);
			auto MapRow = MakeShared<FJsonObject>();
			MapRow->SetStringField(TEXT("map"), Map);
			MapRow->SetBoolField(TEXT("hasSavedLevelScript"), Script != nullptr);
			Maps.Add(MakeShared<FJsonValueObject>(MapRow));
			if (Script) AuditBlueprint(*Script, Rows, Errors, Warnings);
		}
		for (const auto& Entry : BeforeHashes)
		{
			auto File = MakeShared<FJsonObject>();
			const FMD5Hash After = FMD5Hash::HashFile(*Entry.Key);
			const bool bUnchanged = Entry.Value.IsValid() && After.IsValid() && Entry.Value == After;
			if (!bUnchanged) ++Errors;
			File->SetStringField(TEXT("file"), Entry.Key);
			File->SetBoolField(TEXT("unchanged"), bUnchanged);
			Files.Add(MakeShared<FJsonValueObject>(File));
		}
		Report->SetArrayField(TEXT("assets"), Rows);
		Report->SetArrayField(TEXT("loadFailures"), Failures);
		Report->SetArrayField(TEXT("maps"), Maps);
		Report->SetArrayField(TEXT("auditedPackageDiskChecks"), Files);
		Report->SetStringField(TEXT("diskChecksScope"), TEXT("Listed .uasset/.umap files only; not a complete hash audit of vendor dependencies or external actors."));
		Report->SetNumberField(TEXT("errors"), Errors);
		Report->SetNumberField(TEXT("warnings"), Warnings);
		Report->SetStringField(TEXT("status"), Errors == 0 ? TEXT("passed") : TEXT("failed"));
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Report, Writer);
		const bool bWritten = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true)
			&& FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bWritten)
		{
			UE_LOG(LogCoopContractAudit, Error, TEXT("Cannot write audit report: %s"), *Output);
		}
		else
		{
			UE_LOG(LogCoopContractAudit, Display, TEXT("COOP_CONTRACT_REPORT status=%s assets=%d errors=%d warnings=%d output=%s"),
				Errors == 0 ? TEXT("passed") : TEXT("failed"), Rows.Num(), Errors, Warnings, *Output);
		}
	}

	FAutoConsoleCommand CoopGameplayContractAudit(TEXT("coop.AuditGameplayContracts"),
		TEXT("Read saved project Blueprint contracts, compile with SkipSave, write JSON; never saves Content. [Report.json]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunCoopGameplayContractAudit));
}

#endif
