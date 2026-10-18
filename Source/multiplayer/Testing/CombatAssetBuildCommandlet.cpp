#include "Testing/CombatAssetBuildCommandlet.h"

#if WITH_EDITOR
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CapsuleComponent.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GameplayTagContainer.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogCombatAssetBuild, Log, All);

// 只在显式离线命令中工作，不启动客户端、服务器或运行时战斗。
UCombatAssetBuildCommandlet::UCombatAssetBuildCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

#if WITH_EDITOR
namespace CombatAssets
{
const FString Folder = TEXT("/Game/Combat/Presentation");
const FString AnimFolder = Folder / TEXT("Animations");
const FString ConfigPackage = TEXT("/Game/Combat/DA_CombatConfig");
const FString EnemyConfigPackage = TEXT("/Game/Combat/DA_EnemyCombatConfig");
const TCHAR* Sources[] = {
	TEXT("/Game/Mage/Animations/Sequence2/02_Attack/06_Attack/AS_Attack_01_02_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/02_Attack/09_Buff/AS_Buff_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/08_Hit/01_Hit/AS_Hit_Death_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/02_Attack/07_Skill_Attack/AS_Skill_01_01_Start_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/02_Attack/07_Skill_Attack/AS_Skill_01_01_Loop_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/02_Attack/07_Skill_Attack/AS_Skill_01_01_End_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/08_Hit/01_Hit/AS_Hit_F_Seq"),
	TEXT("/Game/Mage/Animations/Sequence2/08_Hit/14_Block/AS_Block_Loop_Seq")
};
const TCHAR* Names[] = { TEXT("AS_Attack"), TEXT("AS_Heal"), TEXT("AS_Death"), TEXT("AS_CastWindup"),
	TEXT("AS_CastRelease"), TEXT("AS_CastRecovery"), TEXT("AS_Hit"), TEXT("AS_Defend") };
const TCHAR* MontageNames[] = { TEXT("AM_Attack"), TEXT("AM_Heal"), TEXT("AM_Death"), TEXT("AM_AreaCast"), TEXT("AM_Hit") };

// 编译蓝图与动画构建可能执行 GC，本次输入和输出都显式保持存活。
TArray<TStrongObjectPtr<UObject>> References;
template<typename T> T* Keep(T* Object) { if (Object) References.Emplace(Object); return Object; }
FString ObjectPath(const FString& Package) { return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package); }
template<typename T> T* Load(const FString& Package) { return Keep(LoadObject<T>(nullptr, *ObjectPath(Package))); }

// 只保存调用方显式传入的新包，不 SaveAll，不保存源动画、原骨架或地图。
bool Save(UObject* Object)
{
	if (!Object || (!Object->GetPackage()->GetName().StartsWith(Folder + TEXT("/"))
		&& Object->GetPackage()->GetName() != ConfigPackage
		&& Object->GetPackage()->GetName() != EnemyConfigPackage
		&& Object->GetPackage()->GetName() != TEXT("/Game/Combat/Escape/WBP_EscapeCombatVisuals"))) return false;
	UPackage* Package = Object->GetPackage();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Object,
		*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
}

bool Compile(UBlueprint* Blueprint)
{
	FCompilerResultsLog Results;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &Results);
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_COMPILE asset=%s errors=%d warnings=%d"),
		*Blueprint->GetPathName(), Results.NumErrors, Results.NumWarnings);
	return Results.NumErrors == 0 && Blueprint->Status != BS_Error;
}

// 菜单的 UIOnly 会跨地图保留；只给现有战斗 HUD 的 BeginPlay 补回游戏输入，不重建任何界面。
bool FixCombatInput()
{
	const FString HUDPackage = TEXT("/Game/Combat/UI/BP_CombatHUD");
	UBlueprint* HUD = Load<UBlueprint>(HUDPackage);
	const auto Fail = [](const TCHAR* Reason)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_INPUT_FIX %s; existing HUD was not saved."), Reason);
		return false;
	};
	if (!HUD || !HUD->ParentClass || !HUD->ParentClass->IsChildOf(AHUD::StaticClass()))
		return Fail(TEXT("Expected the existing AHUD blueprint"));

	const FName InputName(TEXT("CombatInput_GameOnly"));
	const FName CursorName(TEXT("CombatInput_HideCursor"));
	const FName OwnerName(TEXT("CombatInput_GetOwner"));
	UK2Node_Event* Begin = nullptr;
	TArray<UEdGraph*> Graphs;
	HUD->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
			{
				if (Begin) return Fail(TEXT("Multiple BeginPlay events require manual inspection"));
				Begin = Event;
			}
			if (const auto* Call = Cast<UK2Node_CallFunction>(Node); Call
				&& Call->FunctionReference.GetMemberName().ToString().StartsWith(TEXT("SetInputMode_")) && Node->GetFName() != InputName)
				return Fail(TEXT("An existing input-mode policy requires manual inspection"));
			if (const auto* Set = Cast<UK2Node_VariableSet>(Node); Set
				&& Set->GetVarName() == TEXT("bShowMouseCursor") && Node->GetFName() != CursorName)
				return Fail(TEXT("An existing cursor policy requires manual inspection"));
		}
	}
	UEdGraphPin* BeginOut = Begin ? Begin->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
	if (!BeginOut || BeginOut->LinkedTo.Num() != 1)
		return Fail(TEXT("Expected one connected, existing BeginPlay execution chain"));
	UEdGraph* Graph = Begin->GetGraph();
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const auto Linked = [](const UEdGraphPin* A, const UEdGraphPin* B)
	{
		return A && B && A->LinkedTo.Num() == 1 && A->LinkedTo.Contains(B);
	};
	UK2Node_CallFunction* Input = FindObject<UK2Node_CallFunction>(Graph, *InputName.ToString());
	UK2Node_CallFunction* Owner = FindObject<UK2Node_CallFunction>(Graph, *OwnerName.ToString());
	UK2Node_VariableSet* Cursor = FindObject<UK2Node_VariableSet>(Graph, *CursorName.ToString());
	if (Input || Owner || Cursor)
	{
		// 重复运行只核对自己加过的入口，不再加节点，也不重存资源。
		if (!Input || !Owner || !Cursor || Input->FunctionReference.GetMemberName() != TEXT("SetInputMode_GameOnly")
			|| Owner->FunctionReference.GetMemberName() != TEXT("GetOwningPlayerController") || Cursor->GetVarName() != TEXT("bShowMouseCursor")
			|| !Linked(BeginOut, Input->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			|| !Linked(Input->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Cursor->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			|| !Linked(Input->FindPin(TEXT("PlayerController"), EGPD_Input), Owner->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output))
			|| !Linked(Cursor->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input), Owner->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output))
			|| !Cursor->FindPin(TEXT("bShowMouseCursor"), EGPD_Input)
			|| Cursor->FindPin(TEXT("bShowMouseCursor"), EGPD_Input)->DefaultValue != TEXT("false")
			|| !Cursor->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)
			|| Cursor->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->LinkedTo.Num() != 1)
			return Fail(TEXT("The previous input patch was edited; inspect it before applying again"));
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_INPUT_FIX asset=%s alreadyApplied=1 saved=0"), *HUD->GetPathName());
		return true;
	}

	UFunction* InputFunction = UWidgetBlueprintLibrary::StaticClass()->FindFunctionByName(TEXT("SetInputMode_GameOnly"));
	UFunction* OwnerFunction = AHUD::StaticClass()->FindFunctionByName(TEXT("GetOwningPlayerController"));
	if (!InputFunction || !OwnerFunction) return Fail(TEXT("Required engine input functions are unavailable"));
	const auto AddCall = [Graph, Begin](FName Name, UFunction* Function, int32 OffsetX, int32 OffsetY)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, Name);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid(); Node->SetFromFunction(Function); Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
		Node->NodePosX = Begin->NodePosX + OffsetX; Node->NodePosY = Begin->NodePosY + OffsetY;
		return Node;
	};
	Owner = AddCall(OwnerName, OwnerFunction, -960, 200);
	Input = AddCall(InputName, InputFunction, -640, 0);
	Cursor = NewObject<UK2Node_VariableSet>(Graph, CursorName);
	Graph->AddNode(Cursor, false, false);
	Cursor->CreateNewGuid();
	Cursor->VariableReference.SetExternalMember(TEXT("bShowMouseCursor"), APlayerController::StaticClass());
	Cursor->PostPlacedNewNode(); Cursor->AllocateDefaultPins();
	Cursor->NodePosX = Begin->NodePosX - 320; Cursor->NodePosY = Begin->NodePosY;
	Input->NodeComment = TEXT("离开连接菜单后恢复游戏输入；随后继续原 HUD 初始化");
	Cursor->NodeComment = TEXT("隐藏游戏中的菜单鼠标；不修改死亡界面或其他输入规则");
	UEdGraphPin* OwnerResult = Owner->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
	UEdGraphPin* OriginalNext = BeginOut->LinkedTo[0];
	BeginOut->BreakLinkTo(OriginalNext);
	if (!Schema->TryCreateConnection(BeginOut, Input->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
		|| !Schema->TryCreateConnection(Input->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Cursor->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
		|| !Schema->TryCreateConnection(Cursor->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), OriginalNext)
		|| !Schema->TryCreateConnection(OwnerResult, Input->FindPin(TEXT("PlayerController"), EGPD_Input))
		|| !Schema->TryCreateConnection(OwnerResult, Cursor->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
		return Fail(TEXT("Could not connect the input prefix to the preserved BeginPlay chain"));
	Schema->TrySetDefaultValue(*Cursor->FindPin(TEXT("bShowMouseCursor"), EGPD_Input), TEXT("false"));
	if (!Compile(HUD)) return false;
	// 此窄操作只允许保存实际使用的 HUD 包，不扩大原工具的保存范围。
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
	const bool bSaved = HUD->GetPackage()->GetName() == HUDPackage && UPackage::SavePackage(HUD->GetPackage(), HUD,
		*FPackageName::LongPackageNameToFilename(HUDPackage, FPackageName::GetAssetPackageExtension()), Args);
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_INPUT_FIX asset=%s addedNodes=3 originalChain=preserved saved=%d"), *HUD->GetPathName(), bSaved);
	return bSaved;
}

// 使用现成 Rig 的临时副本烘焙骨轨，不靠更换 Skeleton 指针冒充重定向。
UIKRetargeter* PrepareRetargeter(USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh)
{
	UIKRigDefinition* SourceOriginal = Load<UIKRigDefinition>(TEXT("/Game/Mage/Demo/Mannequins/Rigs/IK_Mannequin"));
	UIKRigDefinition* TargetOriginal = Load<UIKRigDefinition>(TEXT("/Game/Characters/Mannequins/Rigs/IK_Mannequin"));
	if (!SourceOriginal || !TargetOriginal) return nullptr;
	UIKRigDefinition* Source = Keep(DuplicateObject<UIKRigDefinition>(SourceOriginal, GetTransientPackage()));
	UIKRigDefinition* Target = Keep(DuplicateObject<UIKRigDefinition>(TargetOriginal, GetTransientPackage()));
	Source->SetFlags(RF_Transient); Target->SetFlags(RF_Transient);
	if (!UIKRigController::GetController(Source)->SetSkeletalMesh(SourceMesh)
		|| !UIKRigController::GetController(Target)->SetSkeletalMesh(TargetMesh)) return nullptr;
	UIKRetargeter* Retargeter = Keep(NewObject<UIKRetargeter>(GetTransientPackage(), NAME_None, RF_Transient));
	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
	Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
	Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
	Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
	Controller->AutoMapChains(EAutoMapChainType::Exact, true);
	FRetargetGlobalSettings Settings = Controller->GetGlobalSettings();
	Settings.bEnableRoot = true; Settings.bEnableFK = true; Settings.bEnableIK = false;
	Controller->SetGlobalSettings(Settings);
	Controller->AutoAlignAllBones(ERetargetSourceOrTarget::Target);
	UIKRetargetProcessor* Processor = Keep(NewObject<UIKRetargetProcessor>(GetTransientPackage()));
	FRetargetProfile Profile;
	Retargeter->FillProfileWithAssetSettings(Profile);
	Processor->Initialize(SourceMesh, TargetMesh, Retargeter, Profile);
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_RETARGET initialized=%d fkPairs=%d sourceBones=%d targetBones=%d"),
		Processor->IsInitialized(), Processor->GetFKChainPairs().Num(),
		SourceMesh->GetSkeleton()->GetReferenceSkeleton().GetRawBoneNum(), TargetMesh->GetSkeleton()->GetReferenceSkeleton().GetRawBoneNum());
	return Processor->IsInitialized() && Processor->GetFKChainPairs().Num() >= 6 ? Retargeter : nullptr;
}

UAnimSequence* Bake(int32 Index, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UIKRetargeter* Retargeter)
{
	UAnimSequence* Source = Load<UAnimSequence>(Sources[Index]);
	if (!Source || Source->GetSkeleton() != SourceMesh->GetSkeleton()) return nullptr;
	const FString Package = AnimFolder / Names[Index];
	UAnimSequence* Sequence = Load<UAnimSequence>(Package);
	const bool bCreated = Sequence == nullptr;
	if (!Sequence)
	{
		FIKRetargetBatchOperationContext Context;
		Context.AssetsToRetarget.Add(Source);
		Context.SourceMesh = SourceMesh; Context.TargetMesh = TargetMesh; Context.IKRetargetAsset = Retargeter;
		Context.NameRule.FolderPath = AnimFolder; Context.NameRule.ReplaceFrom = Source->GetName(); Context.NameRule.ReplaceTo = Names[Index];
		Context.bUseSourcePath = false; Context.bOverwriteExistingFiles = false; Context.bIncludeReferencedAssets = false;
		Context.bExportOnlyAnimatedBones = false;
		Keep(NewObject<UIKRetargetBatchOperation>())->RunRetarget(Context);
		Sequence = Load<UAnimSequence>(Package);
	}
	const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
	if (!Model || Model->GetNumBoneTracks() == 0 || Sequence->GetSkeleton() != TargetMesh->GetSkeleton()
		|| Model->GetNumberOfKeys() != Source->GetDataModel()->GetNumberOfKeys()) return nullptr;
	if (!bCreated) return Sequence;
	Sequence->bEnableRootMotion = false; Sequence->bForceRootLock = true;
	Sequence->PostEditChange();
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_BAKED asset=%s tracks=%d keys=%d length=%.3f"),
		*Sequence->GetPathName(), Model->GetNumBoneTracks(), Model->GetNumberOfKeys(), Sequence->GetPlayLength());
	return Save(Sequence) ? Sequence : nullptr;
}

// 普攻/治疗/死亡各一段；范围技能依次连接起手、释放、收招，不用动画通知决定伤害。
UAnimMontage* Montage(int32 Index, USkeleton* Skeleton, const TArray<UAnimSequence*>& Sequences, bool bUpdateExisting = false)
{
	const FString Package = AnimFolder / MontageNames[Index];
	UAnimMontage* Result = Load<UAnimMontage>(Package);
	if (Result && !bUpdateExisting) return Result;
	const bool bCreated = Result == nullptr;
	if (!Result) Result = Keep(NewObject<UAnimMontage>(CreatePackage(*Package), MontageNames[Index], RF_Public | RF_Standalone));
	const FName SlotName = Index == 0 || Index == 1 || Index == 3 ? TEXT("UpperBody") : TEXT("DefaultSlot");
	if (!Skeleton || !Skeleton->ContainsSlotName(SlotName)) return nullptr;
	Result->Modify();
	Result->SetSkeleton(Skeleton);
	Result->SlotAnimTracks.Reset();
	Result->SlotAnimTracks.AddDefaulted_GetRef().SlotName = SlotName;
	Result->CompositeSections.Reset();
	float Time = 0.0f;
	for (int32 SegmentIndex = 0; SegmentIndex < Sequences.Num(); ++SegmentIndex)
	{
		UAnimSequence* Sequence = Sequences[SegmentIndex];
		FAnimSegment Segment;
		Segment.SetAnimReference(Sequence, true); Segment.StartPos = Time;
		// 保留素材本身的起手、循环和收招节奏，不把整段动作硬压到冷却或零点几秒。
		const float SegmentLength = Sequence->GetPlayLength();
		Segment.AnimPlayRate = 1.0f;
		Result->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
		FCompositeSection Section;
		Section.SectionName = Index == 3 ? (SegmentIndex == 0 ? TEXT("Windup") : SegmentIndex == 1 ? TEXT("Release") : TEXT("Recovery")) : TEXT("Default");
		Section.SetTime(Time);
		Result->CompositeSections.Add(Section);
		Time += SegmentLength;
	}
	for (int32 SectionIndex = 0; SectionIndex + 1 < Result->CompositeSections.Num(); ++SectionIndex)
		Result->CompositeSections[SectionIndex].NextSectionName = Result->CompositeSections[SectionIndex + 1].SectionName;
	Result->SetCompositeLength(Time);
	Result->bEnableAutoBlendOut = Index != 2;
	Result->BlendIn.SetBlendTime(0.15f); Result->BlendOut.SetBlendTime(0.2f);
	Result->PostEditChange();
	if (bCreated) FAssetRegistryModule::AssetCreated(Result);
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ANIMATION_MONTAGE asset=%s slot=%s segments=%d length=%.3f naturalRate=1"),
		*Result->GetPathName(), *SlotName.ToString(), Sequences.Num(), Time);
	return Save(Result) ? Result : nullptr;
}

bool HasCombatUpperBodyLayer(const UAnimGraphNode_LayeredBoneBlend* Layer)
{
	if (!Layer || Layer->Node.LayerSetup.Num() != 1 || Layer->Node.LayerSetup[0].BranchFilters.Num() != 1
		|| Layer->Node.LayerSetup[0].BranchFilters[0].BoneName != TEXT("spine_01")
		|| Layer->Node.LayerSetup[0].BranchFilters[0].BlendDepth != 3) return false;
	const UEdGraphPin* BaseInput = Layer->FindPin(TEXT("BasePose"), EGPD_Input);
	const UEdGraphPin* ActionInput = Layer->FindPin(TEXT("BlendPoses_0"), EGPD_Input);
	const UAnimGraphNode_UseCachedPose* Base = BaseInput && BaseInput->LinkedTo.Num() == 1
		? Cast<UAnimGraphNode_UseCachedPose>(BaseInput->LinkedTo[0]->GetOwningNode()) : nullptr;
	const UAnimGraphNode_Slot* Upper = ActionInput && ActionInput->LinkedTo.Num() == 1
		? Cast<UAnimGraphNode_Slot>(ActionInput->LinkedTo[0]->GetOwningNode()) : nullptr;
	const UEdGraphPin* UpperSource = Upper ? Upper->FindPin(TEXT("Source"), EGPD_Input) : nullptr;
	const UAnimGraphNode_UseCachedPose* Action = UpperSource && UpperSource->LinkedTo.Num() == 1
		? Cast<UAnimGraphNode_UseCachedPose>(UpperSource->LinkedTo[0]->GetOwningNode()) : nullptr;
	const UEdGraphPin* CachedSource = Base && Base->SaveCachedPoseNode.IsValid()
		? Base->SaveCachedPoseNode->FindPin(TEXT("Pose"), EGPD_Input) : nullptr;
	return Upper && Upper->Node.SlotName == TEXT("UpperBody") && Upper->Node.bAlwaysUpdateSourcePose
		&& Base && Action && Base->SaveCachedPoseNode.IsValid() && Base->SaveCachedPoseNode == Action->SaveCachedPoseNode
		&& CachedSource && CachedSource->LinkedTo.Num() == 1;
}

// 保留原有移动状态机，在最终输出前插入唯一 DefaultSlot，逐 Pin 确认真实连通。
bool InsertSlot(UAnimBlueprint* Blueprint)
{
	if (!Blueprint->TargetSkeleton || !Blueprint->TargetSkeleton->ContainsSlotName(TEXT("UpperBody"))
		|| Blueprint->TargetSkeleton->GetReferenceSkeleton().FindBoneIndex(TEXT("spine_01")) == INDEX_NONE) return false;
	TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
	UEdGraph* OutputGraph = nullptr;
	UAnimGraphNode_Root* Root = nullptr;
	TArray<UAnimGraphNode_Slot*> ActionSlots;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph->GetFName() == TEXT("AnimGraph"))
		{
			OutputGraph = Graph;
			Root = FBlueprintEditorUtils::GetAnimGraphRoot(Graph);
		}
		for (UEdGraphNode* Node : Graph->Nodes)
			if (UAnimGraphNode_Slot* Slot = Cast<UAnimGraphNode_Slot>(Node))
				if (Slot->Node.SlotName == TEXT("DefaultSlot")) ActionSlots.AddUnique(Slot);
	}
	UEdGraphPin* Input = Root ? Root->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
	if (!OutputGraph || !Input || Input->LinkedTo.Num() != 1) return false;
	UAnimGraphNode_Slot* FinalSlot = Cast<UAnimGraphNode_Slot>(Input->LinkedTo[0]->GetOwningNode());
	if (FinalSlot && FinalSlot->Node.SlotName != TEXT("DefaultSlot")) FinalSlot = nullptr;
	// 原模板可能已经在上游有同名 Slot。先核实这些节点能用 Source 姿态旁路，不能只看最终节点。
	for (UAnimGraphNode_Slot* Slot : ActionSlots)
	{
		if (Slot == FinalSlot) continue;
		const UEdGraphPin* Source = Slot->FindPin(TEXT("Source"), EGPD_Input);
		const UEdGraphPin* Pose = Slot->FindPin(TEXT("Pose"), EGPD_Output);
		if (!Source || !Pose || (!Pose->LinkedTo.IsEmpty() && Source->LinkedTo.Num() != 1)) return false;
	}
	if (!FinalSlot)
	{
		UEdGraphPin* Original = Input->LinkedTo[0];
		FGraphNodeCreator<UAnimGraphNode_Slot> Creator(*OutputGraph);
		FinalSlot = Creator.CreateNode(false);
		FinalSlot->Node.SlotName = TEXT("DefaultSlot"); FinalSlot->Node.bAlwaysUpdateSourcePose = true;
		FinalSlot->NodePosX = Root->NodePosX; FinalSlot->NodePosY = Root->NodePosY;
		Creator.Finalize(); Root->NodePosX += 300;
		OutputGraph->GetSchema()->BreakPinLinks(*Input, true);
		if (!OutputGraph->GetSchema()->TryCreateConnection(Original, FinalSlot->FindPin(TEXT("Source"), EGPD_Input))
			|| !OutputGraph->GetSchema()->TryCreateConnection(FinalSlot->FindPin(TEXT("Pose"), EGPD_Output), Input)) return false;
	}
	int32 Removed = 0;
	for (UAnimGraphNode_Slot* Slot : ActionSlots)
	{
		if (Slot == FinalSlot) continue;
		UEdGraph* Graph = Slot->GetGraph();
		UEdGraphPin* Source = Slot->FindPin(TEXT("Source"), EGPD_Input);
		UEdGraphPin* Upstream = Source->LinkedTo.Num() == 1 ? Source->LinkedTo[0] : nullptr;
		const TArray<UEdGraphPin*> Consumers = Slot->FindPin(TEXT("Pose"), EGPD_Output)->LinkedTo;
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_SLOT_BYPASS graph=%s node=%s consumers=%d"),
			*Graph->GetName(), *Slot->GetName(), Consumers.Num());
		FBlueprintEditorUtils::RemoveNode(Blueprint, Slot, true);
		for (UEdGraphPin* Consumer : Consumers)
			if (!Graph->GetSchema()->TryCreateConnection(Upstream, Consumer)) return false;
		++Removed;
	}
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_SLOT_CONNECTED blueprint=%s connected=1 removedDuplicates=%d finalSlots=1"),
		*Blueprint->GetPathName(), Removed);
	UEdGraphPin* FullBodyInput = FinalSlot->FindPin(TEXT("Source"), EGPD_Input);
	if (!FullBodyInput || FullBodyInput->LinkedTo.Num() != 1) return false;
	const FString LayerComment = TEXT("Combat: upper-body actions, locomotion legs");
	if (const UAnimGraphNode_LayeredBoneBlend* ExistingLayer = Cast<UAnimGraphNode_LayeredBoneBlend>(FullBodyInput->LinkedTo[0]->GetOwningNode());
		ExistingLayer && ExistingLayer->NodeComment == LayerComment)
	{
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ANIMATION_LAYER blueprint=%s upperBody=1 fullBody=1 reused=1"), *Blueprint->GetPathName());
		return HasCombatUpperBodyLayer(ExistingLayer);
	}
	// 使用骨架已注册的 UpperBody，不新增 Slot、修改或保存原 Skeleton。
	for (UEdGraph* Graph : Graphs)
		for (UEdGraphNode* Node : Graph->Nodes)
			if (const UAnimGraphNode_Slot* Slot = Cast<UAnimGraphNode_Slot>(Node); Slot && Slot->Node.SlotName == TEXT("UpperBody")) return false;
	UEdGraphPin* LocomotionOutput = FullBodyInput->LinkedTo[0];
	FGraphNodeCreator<UAnimGraphNode_SaveCachedPose> CacheCreator(*OutputGraph);
	UAnimGraphNode_SaveCachedPose* Cache = CacheCreator.CreateNode(false);
	Cache->CacheName = TEXT("CombatLocomotionPose");
	Cache->NodePosX = FinalSlot->NodePosX - 1100; Cache->NodePosY = FinalSlot->NodePosY - 250;
	CacheCreator.Finalize();
	UAnimGraphNode_UseCachedPose* Poses[2] = {};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FGraphNodeCreator<UAnimGraphNode_UseCachedPose> UseCreator(*OutputGraph);
		Poses[Index] = UseCreator.CreateNode(false);
		Poses[Index]->SaveCachedPoseNode = Cache;
		Poses[Index]->NodePosX = FinalSlot->NodePosX - 850; Poses[Index]->NodePosY = FinalSlot->NodePosY + Index * 180;
		UseCreator.Finalize();
	}
	FGraphNodeCreator<UAnimGraphNode_Slot> UpperCreator(*OutputGraph);
	UAnimGraphNode_Slot* Upper = UpperCreator.CreateNode(false);
	Upper->Node.SlotName = TEXT("UpperBody"); Upper->Node.bAlwaysUpdateSourcePose = true;
	Upper->NodePosX = FinalSlot->NodePosX - 600; Upper->NodePosY = FinalSlot->NodePosY + 180;
	UpperCreator.Finalize();
	FGraphNodeCreator<UAnimGraphNode_LayeredBoneBlend> LayerCreator(*OutputGraph);
	UAnimGraphNode_LayeredBoneBlend* Layer = LayerCreator.CreateNode(false);
	Layer->NodeComment = LayerComment;
	Layer->Node.bMeshSpaceRotationBlend = true;
	FBranchFilter& Filter = Layer->Node.LayerSetup[0].BranchFilters.AddDefaulted_GetRef();
	Filter.BoneName = TEXT("spine_01"); Filter.BlendDepth = 3;
	Layer->NodePosX = FinalSlot->NodePosX - 300; Layer->NodePosY = FinalSlot->NodePosY;
	LayerCreator.Finalize();
	// 移动姿态只计算一次；施法只覆盖脊柱以上，死亡仍由最后的全身 DefaultSlot 接管。
	OutputGraph->GetSchema()->BreakPinLinks(*FullBodyInput, true);
	const bool bLinked = OutputGraph->GetSchema()->TryCreateConnection(LocomotionOutput, Cache->FindPin(TEXT("Pose"), EGPD_Input))
		&& OutputGraph->GetSchema()->TryCreateConnection(Poses[0]->FindPin(TEXT("Pose"), EGPD_Output), Layer->FindPin(TEXT("BasePose"), EGPD_Input))
		&& OutputGraph->GetSchema()->TryCreateConnection(Poses[1]->FindPin(TEXT("Pose"), EGPD_Output), Upper->FindPin(TEXT("Source"), EGPD_Input))
		&& OutputGraph->GetSchema()->TryCreateConnection(Upper->FindPin(TEXT("Pose"), EGPD_Output), Layer->FindPin(TEXT("BlendPoses_0"), EGPD_Input))
		&& OutputGraph->GetSchema()->TryCreateConnection(Layer->FindPin(TEXT("Pose"), EGPD_Output), FullBodyInput);
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ANIMATION_LAYER blueprint=%s upperBody=1 fullBody=1 connected=%d reused=0"), *Blueprint->GetPathName(), bLinked);
	return bLinked && HasCombatUpperBodyLayer(Layer);
}

// 只设置已有受保护表现字段，不给运行时战斗增加工具专用接口。
bool SetAsset(UObject* Owner, FName Field, UObject* Asset, bool bRequired = true)
{
	FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Field);
	if (!Property) return !bRequired;
	if (Asset && !Asset->IsA(Property->PropertyClass)) return false;
	Property->SetObjectPropertyValue_InContainer(Owner, Asset);
	return true;
}

UBlueprint* CharacterBlueprint(const TCHAR* Name, UClass* Parent, USkeletalMesh* Mesh, UClass* AnimClass,
	UAnimMontage* const* Montages, UmultiplayerCombatConfig* Config, const ACharacter* PlayerTemplate, bool bEnemy)
{
	const FString Package = Folder / Name;
	UBlueprint* Blueprint = Load<UBlueprint>(Package);
	if (Blueprint) return Blueprint;
	Blueprint = Keep(FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Package), Name,
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild")));
	if (!Blueprint || !Compile(Blueprint)) return nullptr;
	ACharacter* Character = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
	if (!Character) return nullptr;
	Character->GetMesh()->SetSkeletalMesh(Mesh);
	Character->GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Character->GetMesh()->SetAnimInstanceClass(AnimClass);
	FTransform Transform = PlayerTemplate->GetMesh()->GetRelativeTransform();
	FVector Offset = Transform.GetLocation();
	Offset.Z += PlayerTemplate->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - Character->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	Transform.SetLocation(Offset); Character->GetMesh()->SetRelativeTransform(Transform);
	if (bEnemy)
	{
		TArray<UStaticMeshComponent*> Components; Character->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
			if (Component->GetFName() == TEXT("EnemyVisual")) { Component->SetVisibility(false); Component->SetHiddenInGame(true); }
	}
	if (!SetAsset(Character, TEXT("AttackMontage"), Montages[0]) || !SetAsset(Character, TEXT("HealMontage"), Montages[1])
		|| !SetAsset(Character, TEXT("DeathMontage"), Montages[2]) || !SetAsset(Character, TEXT("AreaMontage"), Montages[3], false)
		|| !SetAsset(Character, TEXT("HitMontage"), Montages[4], false) || !SetAsset(Character, TEXT("CombatConfig"), Config)) return nullptr;
	if (!Compile(Blueprint)) return nullptr;
	FAssetRegistryModule::AssetCreated(Blueprint);
	return Save(Blueprint) ? Blueprint : nullptr;
}

// 表现引用集中到数据资产；所选 Niagara 已通过递归依赖检查，跳过原包缺材质的效果。
UmultiplayerCombatConfig* CreateConfig()
{
	UmultiplayerCombatConfig* Config = Load<UmultiplayerCombatConfig>(ConfigPackage);
	const bool bCreated = Config == nullptr;
	if (!Config) Config = Keep(NewObject<UmultiplayerCombatConfig>(CreatePackage(*ConfigPackage), TEXT("DA_CombatConfig"), RF_Public | RF_Standalone));
	const FString Water = TEXT("/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/");
	const FString Poison = TEXT("/Game/Pack_Effects_2/Poison_Magic/VFX_Niagara/");
	const FString Audio = Folder / TEXT("Audio");
	auto SetFeedback = [&](FmultiplayerCombatFeedback& Feedback, const FString& Effect, const TCHAR* Sound)
	{
		if (!Effect.IsEmpty()) Feedback.Effect = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(ObjectPath(Effect)));
		if (Sound) Feedback.Sound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(ObjectPath(Audio / Sound)));
	};
	Config->ProjectileEffect = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(ObjectPath(Water + TEXT("NS_Water_Magic_Projectile4"))));
	SetFeedback(Config->AttackFeedback, Water + TEXT("NS_Water_Magic_Splash2"), TEXT("SW_Cast_Placeholder"));
	SetFeedback(Config->ImpactFeedback, Water + TEXT("NS_Water_Magic_Splash2"), TEXT("SW_Hit_Placeholder"));
	SetFeedback(Config->HealFeedback, Water + TEXT("NS_Water_Magic_Aura"), TEXT("SW_Heal_Placeholder"));
	SetFeedback(Config->DefenseFeedback, Water + TEXT("NS_Water_Magic_Shield"), TEXT("SW_Shield_Placeholder"));
	// 起手范围只用地面圆环；毒矛的追踪目标不是范围贴花。
	Config->CastingFeedback.Effect.Reset(); Config->CastingFeedback.CascadeEffect.Reset();
	Config->AreaMarkerMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(ObjectPath(Folder / TEXT("Materials/M_CombatAreaMarker"))));
	SetFeedback(Config->AreaFeedback, Poison + TEXT("NS_Posion_Magic_Area1"), TEXT("SW_Area_Placeholder"));
	SetFeedback(Config->StunFeedback, Poison + TEXT("NS_Posion_Magic_Debuff"), nullptr);
	SetFeedback(Config->FailureFeedback, TEXT(""), TEXT("SW_Denied_Placeholder"));
	if (bCreated) FAssetRegistryModule::AssetCreated(Config);
	return Save(Config) ? Config : nullptr;
}

// GameplayCue 只读取角色配置播放表现；蓝图资产只注册标签，不添加伤害或目标判断。
bool CreateCues()
{
	UClass* CueBase = LoadClass<UObject>(nullptr, TEXT("/Script/multiplayer.multiplayerCombatCue"));
	if (!CueBase) return false;
	for (const TCHAR* Name : { TEXT("Attack"), TEXT("Impact"), TEXT("Heal"), TEXT("Defense"), TEXT("Casting"), TEXT("Area"), TEXT("Stun"), TEXT("Failure") })
	{
		const FString AssetName = FString(TEXT("GCN_")) + Name;
		const FString Package = Folder / TEXT("Cues") / AssetName;
		UBlueprint* Blueprint = Load<UBlueprint>(Package);
		if (Blueprint) continue;
		Blueprint = Keep(FKismetEditorUtilities::CreateBlueprint(CueBase, CreatePackage(*Package), FName(*AssetName),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild")));
		if (!Blueprint || !Compile(Blueprint)) return false;
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		if (FCString::Strcmp(Name, TEXT("Heal")) == 0)
			CastChecked<AmultiplayerCombatCue>(CDO)->bUniqueInstancePerInstigator = true;
		FStructProperty* Field = FindFProperty<FStructProperty>(CDO->GetClass(), TEXT("GameplayCueTag"));
		if (!Field || Field->Struct != FGameplayTag::StaticStruct()) return false;
		FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*(FString(TEXT("GameplayCue.Combat.")) + Name)));
		*Field->ContainerPtrToValuePtr<FGameplayTag>(CDO) = Tag;
		if (FNameProperty* SearchName = FindFProperty<FNameProperty>(CDO->GetClass(), TEXT("GameplayCueName")))
			SearchName->SetPropertyValue_InContainer(CDO, Tag.GetTagName());
		if (!Compile(Blueprint)) return false;
		FAssetRegistryModule::AssetCreated(Blueprint);
		if (!Save(Blueprint)) return false;
	}
	return true;
}

// 布局只生成普通 UMG 控件；订阅、计时与解绑保留在同一个原生 Widget 父类。
bool CreateUI(bool bUpdateOnly = false)
{
	UClass* WidgetBase = LoadClass<UObject>(nullptr, TEXT("/Script/multiplayer.multiplayerCombatWidget"));
	if (!WidgetBase) return false;
	const FString WidgetPath = Folder / TEXT("UI/WBP_CombatSkills");
	UWidgetBlueprint* Widget = Load<UWidgetBlueprint>(WidgetPath);
	if (bUpdateOnly && (!Widget || Widget->ParentClass != WidgetBase)) return false;
	const bool bNewWidget = !Widget;
	if (bNewWidget || bUpdateOnly)
	{
		if (bNewWidget)
			Widget = Keep(Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(WidgetBase, CreatePackage(*WidgetPath),
				TEXT("WBP_CombatSkills"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild"))));
		if (!Widget || !Widget->WidgetTree) return false;
		Widget->Modify();
		// 显式更新只替换此表现蓝图的控件树，不触碰事件图、HUD 类或关卡。
		if (bUpdateOnly)
		{
			Widget->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Widget->WidgetTree = NewObject<UWidgetTree>(Widget, TEXT("WidgetTree"), RF_Transactional);
		}
		UWidgetTree* Tree = Widget->WidgetTree;
		UCanvasPanel* Canvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CombatCanvas"));
		Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		Tree->RootWidget = Canvas;
		// 项目沿用 UE 默认 1080p DPI 曲线；按两倍设计单位布局，540p 时落到清晰的实际像素尺寸。
		constexpr float DesignScale = 2.f;
		const auto Text = [Tree](const TCHAR* Name, const TCHAR* Value, int32 Size, FLinearColor Color, bool bVariable = false)
		{
			UTextBlock* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
			Label->bIsVariable = bVariable; Label->SetText(FText::FromString(Value));
			FSlateFontInfo Font = Label->GetFont(); Font.Size = Size * int32(DesignScale); Label->SetFont(Font);
			Label->SetColorAndOpacity(FSlateColor(Color));
			Label->SetShadowOffset(FVector2D(0, 1)); Label->SetShadowColorAndOpacity(FLinearColor(0, 0, 0, .65f));
			return Label;
		};
		const auto Panel = [Tree, Canvas](const TCHAR* Name, FVector2D Anchor, FVector2D Position, FVector2D Size, FVector2D Alignment)
		{
			UBorder* Border = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(Name));
			Border->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 5.f));
			Border->SetBrushColor(FLinearColor(.018f, .024f, .035f, .82f)); Border->SetPadding(FMargin(10 * DesignScale, 7 * DesignScale));
			UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Border);
			Slot->SetAnchors(FAnchors(Anchor.X, Anchor.Y)); Slot->SetAlignment(Alignment);
			Slot->SetPosition(Position * DesignScale); Slot->SetSize(Size * DesignScale);
			return Border;
		};
		const auto Bar = [Tree](UVerticalBox* Lines, const TCHAR* Name, FLinearColor Color, float Percent, float Height, FMargin Padding)
		{
			UProgressBar* Value = Tree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), FName(Name));
			Value->bIsVariable = true; Value->SetFillColorAndOpacity(Color); Value->SetPercent(Percent);
			FProgressBarStyle Style;
			Style.BackgroundImage = FSlateRoundedBoxBrush(FLinearColor(.08f, .1f, .13f, .95f), 2.f);
			Style.FillImage = FSlateRoundedBoxBrush(FLinearColor::White, 2.f);
			Style.MarqueeImage = Style.FillImage; Value->SetWidgetStyle(Style);
			USizeBox* Size = Tree->ConstructWidget<USizeBox>(); Size->SetHeightOverride(Height * DesignScale); Size->SetContent(Value);
			Lines->AddChildToVerticalBox(Size)->SetPadding(FMargin(Padding.Left * DesignScale, Padding.Top * DesignScale,
				Padding.Right * DesignScale, Padding.Bottom * DesignScale));
		};
		const FLinearColor White(.91f, .94f, .98f), Muted(.56f, .63f, .72f), Green(.32f, .8f, .59f),
			Blue(.33f, .65f, .94f), Gold(.96f, .75f, .38f), Red(.92f, .38f, .4f);
		// 生命区与技能区独立锚定；默认 DPI 下 900x540 的实际尺寸约为 216x88 与 360x78，不再铺满底边。
		UBorder* Stats = Panel(TEXT("StatsPanel"), FVector2D(0, 1), FVector2D(24, -24), FVector2D(216, 88), FVector2D(0, 1));
		UVerticalBox* StatsLines = Tree->ConstructWidget<UVerticalBox>(); Stats->SetContent(StatsLines);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UHorizontalBox* Line = Tree->ConstructWidget<UHorizontalBox>(); StatsLines->AddChildToVerticalBox(Line);
			UHorizontalBoxSlot* Caption = Line->AddChildToHorizontalBox(Text(Index == 0 ? TEXT("HealthLabel") : TEXT("EnergyLabel"),
				Index == 0 ? TEXT("生命") : TEXT("能量"), 12, Muted));
			Caption->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); Caption->SetVerticalAlignment(VAlign_Center);
			Line->AddChildToHorizontalBox(Text(Index == 0 ? TEXT("HealthText") : TEXT("EnergyText"), TEXT("-- / --"), 14, White, true));
			Bar(StatsLines, Index == 0 ? TEXT("HealthBar") : TEXT("EnergyBar"), Index == 0 ? Red : Blue, 1, 6, FMargin(0, 3, 0, Index == 0 ? 7 : 0));
		}
		// 空闲时由原生 Widget 折叠此区；状态文字与短提示单独显示，不挤在施法进度条里。
		UBorder* Casting = Panel(TEXT("CastingPanel"), FVector2D(.5, 1), FVector2D(0, -118), FVector2D(320, 52), FVector2D(.5, 1));
		Casting->bIsVariable = true; Casting->SetVisibility(ESlateVisibility::Collapsed);
		UVerticalBox* CastLines = Tree->ConstructWidget<UVerticalBox>(); Casting->SetContent(CastLines);
		UHorizontalBox* CastHeading = Tree->ConstructWidget<UHorizontalBox>(); CastLines->AddChildToVerticalBox(CastHeading);
		UHorizontalBoxSlot* CastName = CastHeading->AddChildToHorizontalBox(Text(TEXT("CastText"), TEXT(""), 14, Gold, true));
		CastName->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		CastHeading->AddChildToHorizontalBox(Text(TEXT("CancelHint"), TEXT("右键取消"), 11, Muted))->SetVerticalAlignment(VAlign_Center);
		Bar(CastLines, TEXT("CastBar"), Gold, 0, 5, FMargin(0, 6, 0, 0));
		UTextBlock* State = Text(TEXT("StateText"), TEXT(""), 15, Gold, true);
		State->SetJustification(ETextJustify::Center); State->SetVisibility(ESlateVisibility::Collapsed);
		UCanvasPanelSlot* StateSlot = Canvas->AddChildToCanvas(State);
		StateSlot->SetAnchors(FAnchors(.5f, 1)); StateSlot->SetAlignment(FVector2D(.5f, 1));
		StateSlot->SetPosition(FVector2D(0, -178) * DesignScale); StateSlot->SetSize(FVector2D(360, 24) * DesignScale);
		UBorder* Feedback = Panel(TEXT("FeedbackPanel"), FVector2D(.5, 1), FVector2D(0, -212), FVector2D(400, 32), FVector2D(.5, 1));
		Feedback->bIsVariable = true; Feedback->SetPadding(FMargin(8 * DesignScale, 4 * DesignScale)); Feedback->SetVisibility(ESlateVisibility::Collapsed);
		UTextBlock* Message = Text(TEXT("FeedbackText"), TEXT(""), 13, FLinearColor(1, .69f, .55f), true);
		Message->SetJustification(ETextJustify::Center); Message->SetAutoWrapText(true); Feedback->SetContent(Message);
		UBorder* Skills = Panel(TEXT("SkillsPanel"), FVector2D(.5, 1), FVector2D(0, -24), FVector2D(360, 78), FVector2D(.5, 1));
		Skills->SetPadding(FMargin(0)); Skills->SetBrushColor(FLinearColor::Transparent);
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(); Skills->SetContent(Row);
		const TCHAR* Cooldowns[] = { TEXT("AttackCooldownText"), TEXT("DefenseCooldownText"), TEXT("HealCooldownText"), TEXT("AreaCooldownText") };
		const TCHAR* CooldownBars[] = { TEXT("AttackCooldownBar"), TEXT("DefenseCooldownBar"), TEXT("HealCooldownBar"), TEXT("AreaCooldownBar") };
		const TCHAR* Slots[] = { TEXT("AttackSlot"), TEXT("DefenseSlot"), TEXT("HealSlot"), TEXT("AreaSlot") };
		const TCHAR* Keys[] = { TEXT("LMB"), TEXT("Q"), TEXT("E"), TEXT("R") };
		const TCHAR* Labels[] = { TEXT("攻击"), TEXT("防御"), TEXT("治疗"), TEXT("范围") };
		const FLinearColor Colors[] = { Blue, FLinearColor(.6f, .55f, 1), Green, Gold };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			UBorder* Tile = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(Slots[Index]));
			Tile->bIsVariable = true; Tile->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 5.f));
			Tile->SetBrushColor(FLinearColor(.025f, .035f, .05f, .9f)); Tile->SetPadding(FMargin(8 * DesignScale, 7 * DesignScale));
			UHorizontalBoxSlot* Slot = Row->AddChildToHorizontalBox(Tile); Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); Slot->SetPadding(FMargin(3 * DesignScale, 0));
			UVerticalBox* TileLines = Tree->ConstructWidget<UVerticalBox>(); Tile->SetContent(TileLines);
			UHorizontalBox* Title = Tree->ConstructWidget<UHorizontalBox>(); TileLines->AddChildToVerticalBox(Title);
			UHorizontalBoxSlot* Name = Title->AddChildToHorizontalBox(Text(*(FString(TEXT("SkillLabel")) + FString::FromInt(Index)), Labels[Index], 13, Colors[Index]));
			Name->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			Title->AddChildToHorizontalBox(Text(*(FString(TEXT("SkillKey")) + FString::FromInt(Index)), Keys[Index], 10, Muted))->SetVerticalAlignment(VAlign_Center);
			UTextBlock* Counter = Text(Cooldowns[Index], TEXT(""), 21, White, true); Counter->SetJustification(ETextJustify::Center);
			USizeBox* CounterSpace = Tree->ConstructWidget<USizeBox>(); CounterSpace->SetHeightOverride(31 * DesignScale); CounterSpace->SetContent(Counter);
			TileLines->AddChildToVerticalBox(CounterSpace)->SetPadding(FMargin(0, 2 * DesignScale, 0, 3 * DesignScale));
			Bar(TileLines, CooldownBars[Index], Colors[Index], 0, 3, FMargin(0));
		}
		if (bNewWidget) FAssetRegistryModule::AssetCreated(Widget);
		if (!Compile(Widget) || !Save(Widget)) return false;
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_HUD_LAYOUT_UPDATED asset=%s designStats=432x176 designSkills=720x156 feedback=800x64 updateOnly=%d"), *WidgetPath, bUpdateOnly);
	}
	if (bUpdateOnly) return true;
	UClass* HUDBase = LoadClass<UObject>(nullptr, TEXT("/Script/multiplayer.multiplayerCombatHUD"));
	if (!HUDBase) return false;
	const FString HUDPath = Folder / TEXT("UI/BP_CombatHUD");
	UBlueprint* HUD = Load<UBlueprint>(HUDPath);
	if (!HUD)
	{
		HUD = Keep(FKismetEditorUtilities::CreateBlueprint(HUDBase, CreatePackage(*HUDPath), TEXT("BP_CombatHUD"),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild")));
		if (!HUD || !Compile(HUD) || !SetAsset(HUD->GeneratedClass->GetDefaultObject(), TEXT("CombatWidgetClass"), Widget->GeneratedClass)) return false;
		FAssetRegistryModule::AssetCreated(HUD);
		if (!Compile(HUD) || !Save(HUD)) return false;
	}
	return true;
}

// 只给原 HUD 追加素材表现子控件；保留原父类、原控件、事件图、HUDClass 和准星。
bool ConnectARPGHUD()
{
	const FString OriginalPath = TEXT("/Game/Combat/UI/WBP_CombatHUD");
	const FString VisualPath = Folder / TEXT("UI/WBP_ARPGCombatVisuals");
	UWidgetBlueprint* Original = Load<UWidgetBlueprint>(OriginalPath);
	UCanvasPanel* OriginalCanvas = Original && Original->WidgetTree ? Cast<UCanvasPanel>(Original->WidgetTree->RootWidget) : nullptr;
	UClass* Base = LoadClass<UObject>(nullptr, TEXT("/Script/multiplayer.multiplayerCombatWidget"));
	if (!OriginalCanvas || !Base) return false;
	const FString Icons = TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/UI/Icons/");
	const FString Textures = TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/UI/CombatUI/Texture/");
	UTexture2D* SkillIcons[] = { Load<UTexture2D>(Icons + TEXT("T_FireballIcon")), Load<UTexture2D>(Icons + TEXT("Ability_Hoarfrost")),
		Load<UTexture2D>(Icons + TEXT("T_HealAbilityIcon")), Load<UTexture2D>(Icons + TEXT("T_FireblastIcon")),
		Load<UTexture2D>(Icons + TEXT("Ability_Portal_Blast")) };
	UTexture2D* Empty = Load<UTexture2D>(Textures + TEXT("T_EmptyBar"));
	UTexture2D* Health = Load<UTexture2D>(Textures + TEXT("T_HealthBar"));
	UTexture2D* Energy = Load<UTexture2D>(Textures + TEXT("T_ManaBar"));
	UTexture2D* Frame = Load<UTexture2D>(Textures + TEXT("T_StatBarframe"));
	if (!Empty || !Health || !Energy || !Frame) return false;
	for (UTexture2D* Icon : SkillIcons) if (!Icon) return false;
	UWidgetBlueprint* Visual = Load<UWidgetBlueprint>(VisualPath);
	const bool bNew = !Visual;
	if (!Visual)
		Visual = Keep(Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(Base, CreatePackage(*VisualPath),
			TEXT("WBP_ARPGCombatVisuals"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild"))));
	if (!Visual || Visual->ParentClass != Base || !Visual->WidgetTree) return false;
	// 重复执行仅重建这个专用表现子控件，不重建用户原 HUD。
	Visual->Modify();
	Visual->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Visual->WidgetTree = NewObject<UWidgetTree>(Visual, TEXT("WidgetTree"), RF_Transactional);
	UWidgetTree* Tree = Visual->WidgetTree;
	UCanvasPanel* Canvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ARPGCanvas"));
	Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	Tree->RootWidget = Canvas;
	constexpr float Scale = 2.f;
	const FLinearColor White(.94f, .95f, .98f), Muted(.66f, .7f, .76f), Gold(.95f, .78f, .45f);
	const auto Text = [Tree](const TCHAR* Name, const TCHAR* Value, int32 Size, FLinearColor Color, bool bVariable = false)
	{
		UTextBlock* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
		Label->bIsVariable = bVariable; Label->SetText(FText::FromString(Value));
		FSlateFontInfo Font = Label->GetFont(); Font.Size = Size * int32(Scale);
		Font.OutlineSettings = FFontOutlineSettings(2, FLinearColor::Black); Label->SetFont(Font);
		Label->SetColorAndOpacity(Color); Label->SetShadowOffset(FVector2D(1, 1));
		Label->SetShadowColorAndOpacity(FLinearColor(0, 0, 0, .9f));
		return Label;
	};
	const auto Place = [Canvas](UWidget* Child, FVector2D Anchor, FVector2D Position, FVector2D Size, FVector2D Alignment)
	{
		UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Child);
		Slot->SetAnchors(FAnchors(Anchor.X, Anchor.Y)); Slot->SetAlignment(Alignment);
		Slot->SetPosition(Position * Scale); Slot->SetSize(Size * Scale);
	};
	const auto Panel = [Tree, Place](const TCHAR* Name, FVector2D Anchor, FVector2D Position, FVector2D Size, FVector2D Alignment)
	{
		UBorder* Border = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(Name));
		Border->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 5.f));
		Border->SetBrushColor(FLinearColor(.018f, .024f, .035f, .85f)); Border->SetPadding(FMargin(8 * Scale));
		Place(Border, Anchor, Position, Size, Alignment);
		return Border;
	};
	const auto Bar = [Tree, Empty](const TCHAR* Name, UTexture2D* Fill, FLinearColor Color)
	{
		UProgressBar* Value = Tree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), FName(Name));
		Value->bIsVariable = true; Value->SetFillColorAndOpacity(Color); Value->SetPercent(1);
		FProgressBarStyle Style;
		Style.BackgroundImage.SetResourceObject(Empty); Style.BackgroundImage.ImageSize = FVector2D(400, 24);
		Style.FillImage.SetResourceObject(Fill); Style.FillImage.ImageSize = FVector2D(400, 24);
		Style.MarqueeImage = Style.FillImage; Value->SetWidgetStyle(Style);
		return Value;
	};
	// 生命/能量保留左下的独立容器，技能另设底部中央位置。
	UVerticalBox* Vitals = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ARPGVitalsStack"));
	Place(Vitals, FVector2D(0, 1), FVector2D(24, -24), FVector2D(216, 84), FVector2D(0, 1));
	FSlateBrush Transparent; Transparent.DrawAs = ESlateBrushDrawType::NoDrawType;
	UBorder* Stats = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ARPGStatsPanel"));
	Stats->SetBrush(Transparent); Stats->SetPadding(FMargin(0));
	UVerticalBox* StatsLines = Tree->ConstructWidget<UVerticalBox>(); Stats->SetContent(StatsLines);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UHorizontalBox* Heading = Tree->ConstructWidget<UHorizontalBox>(); StatsLines->AddChildToVerticalBox(Heading);
		Heading->AddChildToHorizontalBox(Text(Index == 0 ? TEXT("HealthLabel") : TEXT("EnergyLabel"),
			Index == 0 ? TEXT("生命") : TEXT("能量"), 11, Muted))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		Heading->AddChildToHorizontalBox(Text(Index == 0 ? TEXT("HealthText") : TEXT("EnergyText"), TEXT("-- / --"), 12, White, true));
		UOverlay* Layer = Tree->ConstructWidget<UOverlay>();
		UOverlaySlot* BarSlot = Layer->AddChildToOverlay(Bar(Index == 0 ? TEXT("HealthBar") : TEXT("EnergyBar"), Index == 0 ? Health : Energy, FLinearColor::White));
		BarSlot->SetPadding(FMargin(4, 3)); BarSlot->SetHorizontalAlignment(HAlign_Fill); BarSlot->SetVerticalAlignment(VAlign_Fill);
		UImage* Outline = Tree->ConstructWidget<UImage>(); Outline->SetBrushFromTexture(Frame);
		UOverlaySlot* FrameSlot = Layer->AddChildToOverlay(Outline);
		FrameSlot->SetHorizontalAlignment(HAlign_Fill); FrameSlot->SetVerticalAlignment(VAlign_Fill);
		USizeBox* Size = Tree->ConstructWidget<USizeBox>(); Size->SetHeightOverride(16 * Scale); Size->SetContent(Layer);
		StatsLines->AddChildToVerticalBox(Size)->SetPadding(FMargin(0, 2 * Scale, 0, Index == 0 ? 7 * Scale : 0));
	}
	UBorder* Skills = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ARPGSkillsPanel"));
	Skills->SetPadding(FMargin(0)); Skills->SetBrush(Transparent);
	// 技能独立放在下方中央；生命/能量留在左下，不再把五个技能挤进生命条的宽度。
	Place(Skills, FVector2D(.5f, 1), FVector2D(0, -24), FVector2D(400, 80), FVector2D(.5f, 1));
	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(); Skills->SetContent(Row);
	const TCHAR* Slots[] = { TEXT("AttackSlot"), TEXT("DefenseSlot"), TEXT("HealSlot"), TEXT("AreaSlot"), TEXT("DashSlot") };
	const TCHAR* Labels[] = { TEXT("攻击"), TEXT("防御"), TEXT("治疗"), TEXT("范围"), TEXT("冲刺") };
	const TCHAR* Keys[] = { TEXT("LMB"), TEXT("Q"), TEXT("E"), TEXT("R"), TEXT("Shift") };
	const TCHAR* Counters[] = { TEXT("AttackCooldownText"), TEXT("DefenseCooldownText"), TEXT("HealCooldownText"), TEXT("AreaCooldownText"), TEXT("DashCooldownText") };
	const TCHAR* Bars[] = { TEXT("AttackCooldownBar"), TEXT("DefenseCooldownBar"), TEXT("HealCooldownBar"), TEXT("AreaCooldownBar"), TEXT("DashCooldownBar") };
	const TCHAR* IconNames[] = { TEXT("AttackIcon"), TEXT("DefenseIcon"), TEXT("HealIcon"), TEXT("AreaIcon"), TEXT("DashIcon") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Slots); ++Index)
	{
		UBorder* Tile = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(Slots[Index]));
		// 父类仍可更新技能状态，但 NoDraw 背景不会被运行时 BrushColor 重新染出灰块。
		Tile->bIsVariable = true; Tile->SetBrush(Transparent); Tile->SetPadding(FMargin(2 * Scale, 0));
		UHorizontalBoxSlot* TileSlot = Row->AddChildToHorizontalBox(Tile);
		TileSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); TileSlot->SetPadding(FMargin(0, 0, Index < UE_ARRAY_COUNT(Slots) - 1 ? 10 * Scale : 0, 0));
		UVerticalBox* Lines = Tree->ConstructWidget<UVerticalBox>(); Tile->SetContent(Lines);
		UHorizontalBox* Heading = Tree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(Heading);
		Heading->AddChildToHorizontalBox(Text(*(FString(TEXT("SkillLabel")) + FString::FromInt(Index)), Labels[Index], 10, White))
			->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		Heading->AddChildToHorizontalBox(Text(*(FString(TEXT("SkillKey")) + FString::FromInt(Index)), Keys[Index], 10, Gold));
		UOverlay* IconLayer = Tree->ConstructWidget<UOverlay>();
		UImage* Icon = Tree->ConstructWidget<UImage>(UImage::StaticClass(), FName(IconNames[Index]));
		Icon->bIsVariable = true; Icon->SetBrushFromTexture(SkillIcons[Index]);
		UOverlaySlot* IconSlot = IconLayer->AddChildToOverlay(Icon);
		IconSlot->SetHorizontalAlignment(HAlign_Fill); IconSlot->SetVerticalAlignment(VAlign_Fill);
		UTextBlock* Counter = Text(Counters[Index], TEXT(""), 18, White, true); Counter->SetJustification(ETextJustify::Center);
		UOverlaySlot* CounterSlot = IconLayer->AddChildToOverlay(Counter);
		CounterSlot->SetHorizontalAlignment(HAlign_Fill); CounterSlot->SetVerticalAlignment(VAlign_Center);
		USizeBox* IconSize = Tree->ConstructWidget<USizeBox>();
		IconSize->SetWidthOverride(44 * Scale); IconSize->SetHeightOverride(44 * Scale); IconSize->SetContent(IconLayer);
		UVerticalBoxSlot* IconBoxSlot = Lines->AddChildToVerticalBox(IconSize);
		IconBoxSlot->SetHorizontalAlignment(HAlign_Center); IconBoxSlot->SetPadding(FMargin(0, 2 * Scale));
		USizeBox* TimerSize = Tree->ConstructWidget<USizeBox>(); TimerSize->SetHeightOverride(2 * Scale);
		TimerSize->SetContent(Bar(Bars[Index], Energy, Gold)); Lines->AddChildToVerticalBox(TimerSize);
	}
	USizeBox* StatsSize = Tree->ConstructWidget<USizeBox>(); StatsSize->SetHeightOverride(84 * Scale); StatsSize->SetContent(Stats);
	Vitals->AddChildToVerticalBox(StatsSize);
	UBorder* Casting = Panel(TEXT("CastingPanel"), FVector2D(.5, 1), FVector2D(0, -142), FVector2D(300, 44), FVector2D(.5, 1));
	Casting->bIsVariable = true; Casting->SetVisibility(ESlateVisibility::Collapsed);
	UVerticalBox* CastLines = Tree->ConstructWidget<UVerticalBox>(); Casting->SetContent(CastLines);
	UTextBlock* CastLabel = Text(TEXT("CastText"), TEXT(""), 12, Gold, true); CastLabel->SetJustification(ETextJustify::Center);
	CastLines->AddChildToVerticalBox(CastLabel);
	USizeBox* CastSize = Tree->ConstructWidget<USizeBox>(); CastSize->SetHeightOverride(5 * Scale);
	CastSize->SetContent(Bar(TEXT("CastBar"), Energy, Gold)); CastLines->AddChildToVerticalBox(CastSize)->SetPadding(FMargin(0, 4 * Scale));
	UTextBlock* State = Text(TEXT("StateText"), TEXT(""), 13, Gold, true); State->SetJustification(ETextJustify::Center);
	Place(State, FVector2D(.5, 1), FVector2D(0, -194), FVector2D(350, 24), FVector2D(.5, 1));
	UBorder* Feedback = Panel(TEXT("FeedbackPanel"), FVector2D(.5, 1), FVector2D(0, -226), FVector2D(400, 32), FVector2D(.5, 1));
	Feedback->bIsVariable = true; Feedback->SetVisibility(ESlateVisibility::Collapsed); Feedback->SetPadding(FMargin(6 * Scale));
	UTextBlock* Message = Text(TEXT("FeedbackText"), TEXT(""), 12, White, true); Message->SetJustification(ETextJustify::Center);
	Feedback->SetContent(Message);
	if (bNew) FAssetRegistryModule::AssetCreated(Visual);
	if (!Compile(Visual) || !Save(Visual)) return false;
	// 旧 HUD 仅新增一个全屏子控件；不删除或重命名任何原控件，也不改原绑定函数。
	UUserWidget* Child = Cast<UUserWidget>(Original->WidgetTree->FindWidget(TEXT("ARPGCombatVisuals")));
	if (Child && Child->GetClass() != Visual->GeneratedClass) return false;
	if (!Child)
	{
		Original->Modify();
		Child = Original->WidgetTree->ConstructWidget<UUserWidget>(Visual->GeneratedClass.Get(), TEXT("ARPGCombatVisuals"));
		Child->SetVisibility(ESlateVisibility::HitTestInvisible);
		UCanvasPanelSlot* Slot = OriginalCanvas->AddChildToCanvas(Child);
		Slot->SetAnchors(FAnchors(0, 0, 1, 1)); Slot->SetOffsets(FMargin(0)); Slot->SetZOrder(-1);
		if (!Compile(Original) || !Save(Original)) return false;
	}
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ARPG_HUD_CONNECTED original=%s child=%s originalGraphsPreserved=1 HUDClassChanged=0"), *OriginalPath, *VisualPath);
	return true;
}

// 只刷新专用表现副本与小兵受击/受控反馈，不重建地图、角色或原 HUD。
bool RefreshCombatFeedbackAndHUD()
{
	auto* Original = Load<UWidgetBlueprint>(TEXT("/Game/Combat/UI/WBP_CombatHUD"));
	if (!Original || !Original->WidgetTree || !Original->WidgetTree->FindWidget(TEXT("ARPGCombatVisuals"))) return false;
	if (!ConnectARPGHUD()) return false;
	auto* Source = Load<UWidgetBlueprint>(Folder / TEXT("UI/WBP_ARPGCombatVisuals"));
	auto* Escape = Load<UWidgetBlueprint>(TEXT("/Game/Combat/Escape/WBP_EscapeCombatVisuals"));
	auto* PlayerRules = Load<UmultiplayerCombatConfig>(ConfigPackage);
	auto* MinionRules = Load<UmultiplayerCombatConfig>(EnemyConfigPackage);
	auto* HealCue = Load<UBlueprint>(Folder / TEXT("Cues/GCN_Heal"));
	if (!Source || !Source->WidgetTree || !Escape || !Escape->WidgetTree || !PlayerRules || !MinionRules
		|| !HealCue || !HealCue->GeneratedClass) return false;
	// 两位施法者同时治疗同一目标时，各自保留自己的异步表现上下文。
	HealCue->Modify();
	CastChecked<AmultiplayerCombatCue>(HealCue->GeneratedClass->GetDefaultObject())->bUniqueInstancePerInstigator = true;
	if (!Compile(HealCue) || !Save(HealCue)) return false;
	Escape->Modify();
	Escape->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Escape->WidgetTree = DuplicateObject<UWidgetTree>(Source->WidgetTree, Escape, TEXT("WidgetTree"));
	// 撤离状态保留原来的上方显示区域，技能布局与基础关卡共用同一份模板。
	if (auto* Status = Cast<UTextBlock>(Escape->WidgetTree->FindWidget(TEXT("StateText"))))
	{
		Status->SetAutoWrapText(true);
		if (auto* Slot = Cast<UCanvasPanelSlot>(Status->Slot))
		{
			Slot->SetAnchors(FAnchors(.5f, 0)); Slot->SetAlignment(FVector2D(.5f, 0));
			Slot->SetPosition(FVector2D(0, 24)); Slot->SetSize(FVector2D(1050, 120));
		}
	}
	if (!Compile(Escape) || !Save(Escape)) return false;
	MinionRules->Modify();
	// 小兵仍只有自己的近战；被打中或被控制的表现不能因为没有玩家支援技能而清空。
	MinionRules->ImpactFeedback.Effect = PlayerRules->ImpactFeedback.Effect;
	MinionRules->ImpactFeedback.CascadeEffect = PlayerRules->ImpactFeedback.CascadeEffect;
	MinionRules->StunFeedback = PlayerRules->StunFeedback;
	if (!Save(MinionRules)) return false;
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_FEEDBACK_HUD_REFRESHED slots=5 vitals=left skills=center originalHUD=preserved enemyAttack=melee enemyImpactAndStun=connected"));
	return true;
}

// 复制源包的两层血条表现树，不复制属性系统、绑定图或平滑计时器。
bool ConnectARPGEnemyHealth()
{
	UWidgetBlueprint* SourceOuter = Load<UWidgetBlueprint>(TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/UI/Widget/WB_MagicEnemyHealth"));
	UWidgetBlueprint* SourceBar = Load<UWidgetBlueprint>(TEXT("/Game/AdvancedARPGCombat/AdvancedCombatFramework/UI/Widgets/WB_AttributeProgressBar"));
	UBlueprint* Enemy = Load<UBlueprint>(Folder / TEXT("BP_CombatEnemy"));
	if (!SourceOuter || !SourceOuter->WidgetTree || !SourceBar || !SourceBar->WidgetTree || !Enemy || !Enemy->GeneratedClass) return false;
	UWidget* SourceHealth = SourceOuter->WidgetTree->FindWidget(TEXT("Health"));
	if (!SourceHealth) return false;
	const auto CopyLayout = [](UWidgetBlueprint* Source, const TCHAR* Name)
	{
		const FString Package = Folder / TEXT("UI") / Name;
		UWidgetBlueprint* Output = Load<UWidgetBlueprint>(Package);
		if (Output && Output->ParentClass != UmultiplayerEnemyHealthWidget::StaticClass()) return static_cast<UWidgetBlueprint*>(nullptr);
		if (!Output)
		{
			Output = Keep(Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UmultiplayerEnemyHealthWidget::StaticClass(),
				CreatePackage(*Package), FName(Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
				UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild"))));
			if (!Output) return Output;
			FAssetRegistryModule::AssetCreated(Output);
		}
		Output->Modify();
		if (Output->WidgetTree) Output->WidgetTree->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Output->WidgetTree = DuplicateObject<UWidgetTree>(Source->WidgetTree, Output, TEXT("WidgetTree"));
		// UMG 属性委托属于源业务图；副本只保留控件、槽位、尺寸、颜色和纹理。
		Output->WidgetTree->ForEachWidget([](UWidget* Widget)
		{
			for (TFieldIterator<FDelegateProperty> Field(Widget->GetClass()); Field; ++Field)
				Field->ContainerPtrToValuePtr<FScriptDelegate>(Widget)->Unbind();
		});
		return Output;
	};
	UWidgetBlueprint* Bar = CopyLayout(SourceBar, TEXT("WBP_ARPGEnemyAttributeBar"));
	if (!Bar || !Bar->WidgetTree->RootWidget) return false;
	UProgressBar* Primary = Cast<UProgressBar>(Bar->WidgetTree->FindWidget(TEXT("AttributeBar")));
	UProgressBar* Trail = Cast<UProgressBar>(Bar->WidgetTree->FindWidget(TEXT("LerpAttributeBar")));
	if (!Primary || !Trail) return false;
	Primary->Rename(TEXT("HealthBar"), Bar->WidgetTree, REN_DontCreateRedirectors | REN_NonTransactional);
	Primary->bIsVariable = true; Trail->bIsVariable = true;
	// 外层源实例的样式由其 PreConstruct 图应用；这里直接拷贝样式值，不运行源初始化逻辑。
	for (const auto& Entry : { TPair<const TCHAR*, UProgressBar*>(TEXT("AttributeBarStyle"), Primary),
		TPair<const TCHAR*, UProgressBar*>(TEXT("LerpBarStyle"), Trail) })
	{
		const FStructProperty* Style = FindFProperty<FStructProperty>(SourceHealth->GetClass(), Entry.Key);
		if (!Style || Style->Struct->GetFName() != TEXT("ProgressBarStyle")) return false;
		Entry.Value->SetWidgetStyle(*Style->ContainerPtrToValuePtr<FProgressBarStyle>(SourceHealth));
	}
	UWidget* SourceRoot = Bar->WidgetTree->RootWidget;
	UOverlay* Numbers = Bar->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("EnemyHealthReadout"));
	Bar->WidgetTree->RootWidget = Numbers;
	UOverlaySlot* VisualSlot = Numbers->AddChildToOverlay(SourceRoot);
	VisualSlot->SetHorizontalAlignment(HAlign_Fill); VisualSlot->SetVerticalAlignment(VAlign_Fill);
	UTextBlock* Label = Bar->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HealthText"));
	Label->bIsVariable = true; Label->SetText(FText::FromString(TEXT("100 / 100"))); Label->SetJustification(ETextJustify::Center);
	FSlateFontInfo Font = Label->GetFont(); Font.Size = 12; Font.OutlineSettings = FFontOutlineSettings(1, FLinearColor::Black); Label->SetFont(Font);
	UOverlaySlot* TextSlot = Numbers->AddChildToOverlay(Label);
	TextSlot->SetHorizontalAlignment(HAlign_Fill); TextSlot->SetVerticalAlignment(VAlign_Center);
	if (!Compile(Bar) || !Save(Bar)) return false;
	UWidgetBlueprint* Outer = CopyLayout(SourceOuter, TEXT("WBP_ARPGEnemyHealth"));
	if (!Outer) return false;
	UWidget* OldHealth = Outer->WidgetTree->FindWidget(TEXT("Health"));
	UPanelWidget* Parent = OldHealth ? OldHealth->GetParent() : nullptr;
	if (!Parent) return false;
	OldHealth->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	UUserWidget* NewHealth = Outer->WidgetTree->ConstructWidget<UUserWidget>(Bar->GeneratedClass.Get(), TEXT("Health"));
	NewHealth->bIsVariable = true;
	if (!Parent->ReplaceChild(OldHealth, NewHealth)) return false;
	OldHealth->Slot = nullptr;
	if (!Compile(Outer) || !Save(Outer)) return false;
	Enemy->Modify();
	auto* Defaults = Cast<AmultiplayerCombatEnemy>(Enemy->GeneratedClass->GetDefaultObject());
	UWidgetComponent* Component = Defaults ? Cast<UWidgetComponent>(Defaults->GetDefaultSubobjectByName(TEXT("HealthBarWidget"))) : nullptr;
	if (!Component) return false;
	Component->SetWidgetClass(Outer->GeneratedClass.Get());
	if (auto* WorldHealth = Cast<UmultiplayerEnemyHealthWidgetComponent>(Component)) WorldHealth->ConfigureWorldDefaults();
	else return false;
	if (!Compile(Enemy) || !Save(Enemy)) return false;
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ARPG_ENEMY_HEALTH_CONNECTED sourceLayout=2 nativeWidgets=2 sourceBusinessGraphs=0 healthPolling=0"));
	return true;
}

// 仅迁移现有敌人 BP 的展示组件；不重新复制源 UI，不更换布局或业务图。
bool UpdateEnemyWorldHealth()
{
	UBlueprint* Enemy = Load<UBlueprint>(Folder / TEXT("BP_CombatEnemy"));
	if (!Enemy || !Enemy->GeneratedClass) return false;
	Enemy->Modify();
	auto* Defaults = Cast<AmultiplayerCombatEnemy>(Enemy->GeneratedClass->GetDefaultObject());
	auto* Legacy = Defaults ? Cast<UWidgetComponent>(Defaults->GetDefaultSubobjectByName(TEXT("HealthBarWidget"))) : nullptr;
	auto* Component = Cast<UmultiplayerEnemyHealthWidgetComponent>(Legacy);
	auto* ExistingWidget = Load<UWidgetBlueprint>(Folder / TEXT("UI/WBP_ARPGEnemyHealth"));
	UE_LOG(LogCombatAssetBuild, Display, TEXT("WORLD_HEALTH_COMPONENT_AUDIT bp=%s componentClass=%s nativeClass=%s"),
		*GetNameSafe(Enemy), *GetNameSafe(Legacy ? Legacy->GetClass() : nullptr),
		*GetNameSafe(GetMutableDefault<AmultiplayerCombatEnemy>()->GetDefaultSubobjectByName(TEXT("HealthBarWidget"))->GetClass()));
	if (!Component && Legacy && Legacy->GetClass() == UWidgetComponent::StaticClass())
	{
		// 旧BP序列化了同名基类子对象，单改C++构造类型不会升级它。按编辑器换组件类的方式
		// 复制共同属性并替换CDO引用，只迁移这个继承组件，不重建敌人或任何UI布局。
		const FName ComponentName = Legacy->GetFName();
		const EObjectFlags Flags = Legacy->GetFlags();
		Legacy->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Component = NewObject<UmultiplayerEnemyHealthWidgetComponent>(Defaults, ComponentName, Flags);
		UEngine::CopyPropertiesForUnrelatedObjects(Legacy, Component);
		TMap<UObject*, UObject*> Replacements;
		Replacements.Add(Legacy, Component);
		FArchiveReplaceObjectRef<UObject> ReplaceReferences(Defaults, Replacements);
		Defaults->RemoveOwnedComponent(Legacy);
		Defaults->AddOwnedComponent(Component);
		FindFProperty<FObjectPropertyBase>(Defaults->GetClass(), TEXT("HealthBarWidget"))->SetObjectPropertyValue_InContainer(Defaults, Component);
		Legacy->MarkAsGarbage();
	}
	if (!Component || !ExistingWidget || !ExistingWidget->GeneratedClass)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("World health migration requires the native component and existing widget: component=%s widget=%s"),
			*GetNameSafe(Defaults ? Defaults->GetDefaultSubobjectByName(TEXT("HealthBarWidget")) : nullptr), *GetNameSafe(ExistingWidget));
		return false;
	}
	// 原生组件类型更新后旧BP覆盖可能失去WidgetClass；明确重连已有布局，不复制或重建源UI。
	Component->SetWidgetClass(ExistingWidget->GeneratedClass.Get());
	Component->ConfigureWorldDefaults();
	if (!Compile(Enemy)) return false;
	Defaults = Cast<AmultiplayerCombatEnemy>(Enemy->GeneratedClass->GetDefaultObject());
	Component = Defaults ? Cast<UmultiplayerEnemyHealthWidgetComponent>(Defaults->GetDefaultSubobjectByName(TEXT("HealthBarWidget"))) : nullptr;
	if (!Component || Component->GetWidgetClass() != ExistingWidget->GeneratedClass || !Save(Enemy)) return false;
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ENEMY_WORLD_HEALTH_UPDATED asset=%s worldSpace=1 manualRedraw=1 layout=preserved depthTest=1"), *Enemy->GetPathName());
	return true;
}

bool Create()
{
	USkeletalMesh* SourceMesh = Load<USkeletalMesh>(TEXT("/Game/Mage/Demo/Mannequins/Meshes/SKM_Manny_Simple"));
	USkeletalMesh* TargetMesh = Load<USkeletalMesh>(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple"));
	UBlueprint* Player = Load<UBlueprint>(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	UAnimBlueprint* Manny = Load<UAnimBlueprint>(TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny"));
	UAnimBlueprint* Quinn = Load<UAnimBlueprint>(TEXT("/Game/Characters/Mannequins/Animations/ABP_Quinn"));
	if (!SourceMesh || !TargetMesh || !Player || !Player->GeneratedClass || !Manny || !Quinn
		|| TargetMesh->GetSkeleton() != Manny->TargetSkeleton || !TargetMesh->GetSkeleton()->ContainsSlotName(TEXT("DefaultSlot"))) return false;
	UIKRetargeter* Retargeter = PrepareRetargeter(SourceMesh, TargetMesh);
	if (!Retargeter) return false;
	UAnimSequence* Sequences[UE_ARRAY_COUNT(Sources)] = {};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Sources); ++Index)
	{
		Sequences[Index] = Bake(Index, SourceMesh, TargetMesh, Retargeter);
		if (!Sequences[Index]) return false;
	}
	UAnimMontage* Montages[] = {
		Montage(0, TargetMesh->GetSkeleton(), { Sequences[0] }), Montage(1, TargetMesh->GetSkeleton(), { Sequences[1] }),
		Montage(2, TargetMesh->GetSkeleton(), { Sequences[2] }), Montage(3, TargetMesh->GetSkeleton(), { Sequences[3], Sequences[4], Sequences[5] }),
		Montage(4, TargetMesh->GetSkeleton(), { Sequences[6] }) };
	for (UAnimMontage* Asset : Montages) if (!Asset) return false;
	IAssetTools& Assets = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	// Quinn 的父节点覆盖按 GUID 查找；复制父图时保留 GUID，避免冷加载删掉全部覆盖。
	FBlueprintDuplicationScopeFlags DuplicateScope(FBlueprintDuplicationScopeFlags::TheSameNodeGuid);
	UAnimBlueprint* Parent = Load<UAnimBlueprint>(AnimFolder / TEXT("ABP_CombatLocomotion"));
	if (!Parent) Parent = Keep(Cast<UAnimBlueprint>(Assets.DuplicateAsset(TEXT("ABP_CombatLocomotion"), AnimFolder, Manny)));
	if (!Parent || !InsertSlot(Parent) || !Compile(Parent) || !Save(Parent)) return false;
	UAnimBlueprint* Child = Load<UAnimBlueprint>(AnimFolder / TEXT("ABP_CombatQuinn"));
	if (!Child) Child = Keep(Cast<UAnimBlueprint>(Assets.DuplicateAsset(TEXT("ABP_CombatQuinn"), AnimFolder, Quinn)));
	if (!Child) return false;
	Child->ParentClass = Parent->GeneratedClass;
	Child->ParentAssetOverrides = Quinn->ParentAssetOverrides;
	FBlueprintEditorUtils::RefreshAllNodes(Child);
	if (!Compile(Child) || !Save(Child)) return false;
	const ACharacter* Template = Cast<ACharacter>(Player->GeneratedClass->GetDefaultObject());
	if (!Template) return false;
	UmultiplayerCombatConfig* Config = CreateConfig();
	if (!Config || !CreateCues() || !CreateUI()) return false;
	UBlueprint* NewPlayer = CharacterBlueprint(TEXT("BP_CombatPlayer"), Player->GeneratedClass, TargetMesh, Child->GeneratedClass, Montages, Config, Template, false);
	UBlueprint* NewEnemy = CharacterBlueprint(TEXT("BP_CombatEnemy"), AmultiplayerCombatEnemy::StaticClass(), TargetMesh, Child->GeneratedClass, Montages, Config, Template, true);
	FAssetCompilingManager::Get().FinishAllCompilation();
	return NewPlayer && NewEnemy;
}

// 只更新已接入的战斗蒙太奇和两份 Combat 动画蓝图，不重新导入素材、保存骨架或加载关卡。
bool UpdateAnimations()
{
	USkeletalMesh* Mesh = Load<USkeletalMesh>(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple"));
	UAnimBlueprint* Parent = Load<UAnimBlueprint>(AnimFolder / TEXT("ABP_CombatLocomotion"));
	UAnimBlueprint* Child = Load<UAnimBlueprint>(AnimFolder / TEXT("ABP_CombatQuinn"));
	UmultiplayerCombatConfig* Config = Load<UmultiplayerCombatConfig>(ConfigPackage);
	UAnimSequence* Sequences[7] = {};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Sequences); ++Index)
	{
		Sequences[Index] = Load<UAnimSequence>(AnimFolder / Names[Index]);
		if (!Sequences[Index] || !Mesh || Sequences[Index]->GetSkeleton() != Mesh->GetSkeleton()) return false;
	}
	if (!Parent || !Child || !Config || Parent->TargetSkeleton != Mesh->GetSkeleton() || Child->ParentClass != Parent->GeneratedClass) return false;
	for (const TCHAR* Name : MontageNames)
		if (!Load<UAnimMontage>(AnimFolder / Name)) return false;
	UAnimMontage* Montages[] = {
		Montage(0, Mesh->GetSkeleton(), { Sequences[0] }, true), Montage(1, Mesh->GetSkeleton(), { Sequences[1] }, true),
		Montage(2, Mesh->GetSkeleton(), { Sequences[2] }, true), Montage(3, Mesh->GetSkeleton(), { Sequences[3], Sequences[4], Sequences[5] }, true),
		Montage(4, Mesh->GetSkeleton(), { Sequences[6] }, true) };
	for (UAnimMontage* Asset : Montages) if (!Asset) return false;
	if (!InsertSlot(Parent) || !Compile(Parent) || Parent->Status == BS_UpToDateWithWarnings || !Save(Parent)) return false;
	FBlueprintEditorUtils::RefreshAllNodes(Child);
	if (!Compile(Child) || Child->Status == BS_UpToDateWithWarnings || !Save(Child)) return false;
	const float Windup = Montages[3]->GetSectionLength(Montages[3]->GetSectionIndex(TEXT("Windup")));
	Config->Modify();
	Config->Area.Windup = Windup;
	Config->Area.Recovery = Montages[3]->GetPlayLength() - Windup;
	// 只对齐范围技能时序，保留用户配置的伤害、消耗、冷却和其他技能参数。
	if (!Save(Config)) return false;
	FAssetCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ANIMATION_TIMING attack=%.6f areaWindup=%.6f areaRecovery=%.6f visualPreview=NOT_RUN"),
		Montages[0]->GetPlayLength(), Config->Area.Windup, Config->Area.Recovery);
	return true;
}

// 只接入素材表现：保留本项目骨架、移动状态机和 GAS，不挂源包自己的战斗框架。
bool ConnectARPGAnimations()
{
	const FString ARPGFolder = Folder / TEXT("ARPG");
	const FString SourceFolder = TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/Animations");
	USkeletalMesh* SourceMesh = Load<USkeletalMesh>(TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Meshes/SKM_CombatMannequin_Simple"));
	USkeletalMesh* TargetMesh = Load<USkeletalMesh>(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple"));
	UIKRigDefinition* OriginalRig = Load<UIKRigDefinition>(TEXT("/Game/Characters/Mannequins/Rigs/IK_Mannequin"));
	UAnimBlueprint* Template = Load<UAnimBlueprint>(AnimFolder / TEXT("ABP_CombatLocomotion"));
	UmultiplayerCombatConfig* Config = Load<UmultiplayerCombatConfig>(ConfigPackage);
	if (!SourceMesh || !TargetMesh || !OriginalRig || !Template || !Config) return false;

	// 临时 Rig 使用同一套 UE5 骨链，但真正把 ARPG 源骨轨烘焙到当前项目骨架。
	UIKRigDefinition* SourceRig = Keep(DuplicateObject<UIKRigDefinition>(OriginalRig, GetTransientPackage()));
	UIKRigDefinition* TargetRig = Keep(DuplicateObject<UIKRigDefinition>(OriginalRig, GetTransientPackage()));
	SourceRig->SetFlags(RF_Transient); TargetRig->SetFlags(RF_Transient);
	if (!UIKRigController::GetController(SourceRig)->SetSkeletalMesh(SourceMesh)
		|| !UIKRigController::GetController(TargetRig)->SetSkeletalMesh(TargetMesh)) return false;
	UIKRetargeter* Retargeter = Keep(NewObject<UIKRetargeter>(GetTransientPackage(), NAME_None, RF_Transient));
	UIKRetargeterController* RetargetController = UIKRetargeterController::GetController(Retargeter);
	RetargetController->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
	RetargetController->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
	RetargetController->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
	RetargetController->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
	RetargetController->AutoMapChains(EAutoMapChainType::Exact, true);
	FRetargetGlobalSettings Settings = RetargetController->GetGlobalSettings();
	Settings.bEnableRoot = true; Settings.bEnableFK = true; Settings.bEnableIK = false;
	RetargetController->SetGlobalSettings(Settings);
	RetargetController->AutoAlignAllBones(ERetargetSourceOrTarget::Target);
	UIKRetargetProcessor* Processor = Keep(NewObject<UIKRetargetProcessor>(GetTransientPackage()));
	FRetargetProfile Profile; Retargeter->FillProfileWithAssetSettings(Profile);
	Processor->Initialize(SourceMesh, TargetMesh, Retargeter, Profile);
	if (!Processor->IsInitialized() || Processor->GetFKChainPairs().Num() < 6) return false;

	struct FAnimationSource { const TCHAR* Name; const TCHAR* Path; };
	const FAnimationSource AnimationSources[] = {
		{ TEXT("Idle"), TEXT("Locomotion/Idle") }, { TEXT("JogFwd"), TEXT("Locomotion/Jog_Fwd") },
		{ TEXT("JogBwd"), TEXT("Locomotion/Jog_Bwd") }, { TEXT("JogLeft"), TEXT("Locomotion/Jog_Left") },
		{ TEXT("JogRight"), TEXT("Locomotion/Jog_Right") }, { TEXT("Sprint"), TEXT("Locomotion/TravelMode_Sprint") },
		{ TEXT("JumpStart"), TEXT("Jump/Travel_Mode_Jump_Start") }, { TEXT("FallLoop"), TEXT("Jump/Travel_Mode_Jump_Fall_Loop") },
		{ TEXT("Attack"), TEXT("FireMagicProjectile_A_Short") }, { TEXT("Area"), TEXT("Skills/Flameblast_Anim") },
		{ TEXT("Death"), TEXT("Death/Death_Fwd") }, { TEXT("Hit"), TEXT("ExampleMagicEnemy/HitReact_Front_Ranged_Anim") },
		// 原治疗 Montage 长 9.8 秒且尾段自循环；短治疗先使用非循环施法手势，不把长动画硬加速。
		{ TEXT("Heal"), TEXT("FireMagicProjectile_A_Medium") }
	};
	TMap<FString, UAnimSequence*> Sequences;
	TMap<FString, UAnimSequence*> SourceNameToRetargeted;
	for (const FAnimationSource& Entry : AnimationSources)
	{
		UAnimSequence* Source = Load<UAnimSequence>(SourceFolder / Entry.Path);
		if (!Source || Source->GetSkeleton() != SourceMesh->GetSkeleton()) return false;
		const FString Name = FString(TEXT("AS_ARPG_")) + Entry.Name;
		UAnimSequence* Sequence = Load<UAnimSequence>(ARPGFolder / Name);
		if (!Sequence)
		{
			FIKRetargetBatchOperationContext Context;
			Context.AssetsToRetarget.Add(Source);
			Context.SourceMesh = SourceMesh; Context.TargetMesh = TargetMesh; Context.IKRetargetAsset = Retargeter;
			Context.NameRule.FolderPath = ARPGFolder;
			Context.NameRule.ReplaceFrom = Source->GetName(); Context.NameRule.ReplaceTo = Name;
			Context.bUseSourcePath = false; Context.bOverwriteExistingFiles = false;
			Context.bIncludeReferencedAssets = false; Context.bExportOnlyAnimatedBones = false;
			Keep(NewObject<UIKRetargetBatchOperation>())->RunRetarget(Context);
			Sequence = Load<UAnimSequence>(ARPGFolder / Name);
		}
		const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
		if (!Model || Model->GetNumBoneTracks() == 0 || Sequence->GetSkeleton() != TargetMesh->GetSkeleton()
			|| Model->GetNumberOfKeys() != Source->GetDataModel()->GetNumberOfKeys()) return false;
		Sequence->Modify();
		// 业务结算和移动约束由 GAS 负责，不携带原包的技能、旋转或输入通知。
		Sequence->RemoveNotifies();
		Sequence->bEnableRootMotion = false; Sequence->bForceRootLock = true;
		Sequence->PostEditChange();
		if (!Save(Sequence)) return false;
		Sequences.Add(Entry.Name, Sequence); SourceNameToRetargeted.Add(Source->GetName(), Sequence);
		UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ANIMATION_BAKED asset=%s seconds=%.6f tracks=%d"),
			*Sequence->GetPathName(), Sequence->GetPlayLength(), Model->GetNumBoneTracks());
	}

	IAssetTools& Assets = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UBlendSpace* SourceBlend = Load<UBlendSpace>(SourceFolder / TEXT("Locomotion/BS_MagicLocomotion"));
	if (!SourceBlend) return false;
	// 样本动画已逐个重定向；这里只复制方向布局，分别适配玩家 500 和敌人 220 的实际移动速度。
	const auto MakeLocomotion = [&](const TCHAR* Name, float RunSpeed) -> UBlendSpace*
	{
		UBlendSpace* Result = Load<UBlendSpace>(ARPGFolder / Name);
		if (!Result) Result = Keep(Cast<UBlendSpace>(Assets.DuplicateAsset(Name, ARPGFolder, SourceBlend)));
		if (!Result || Result->GetNumberOfBlendSamples() != SourceBlend->GetNumberOfBlendSamples()) return nullptr;
		Result->Modify(); Result->SetSkeleton(TargetMesh->GetSkeleton());
		const FStructProperty* Parameters = FindFProperty<FStructProperty>(Result->GetClass(), TEXT("BlendParameters"));
		if (!Parameters || Parameters->ArrayDim < 2) return nullptr;
		FBlendParameter* SpeedAxis = Parameters->ContainerPtrToValuePtr<FBlendParameter>(Result, 1);
		SpeedAxis->Min = 0; SpeedAxis->Max = 800.f * RunSpeed / 600.f; SpeedAxis->bSnapToGrid = false;
		for (int32 Index = 0; Index < SourceBlend->GetNumberOfBlendSamples(); ++Index)
		{
			const FBlendSample& Sample = SourceBlend->GetBlendSample(Index);
			UAnimSequence* const* Animation = Sample.Animation ? SourceNameToRetargeted.Find(Sample.Animation->GetName()) : nullptr;
			FVector Position = Sample.SampleValue; Position.Y *= RunSpeed / 600.f;
			if (!Animation || !Result->ReplaceSampleAnimation(Index, *Animation) || !Result->EditSampleValue(Index, Position)) return nullptr;
		}
		Result->TargetWeightInterpolationSpeedPerSec = 8.f;
		Result->ValidateSampleData(); Result->ResampleData(); Result->PostEditChange();
		return Save(Result) ? Result : nullptr;
	};
	UBlendSpace* PlayerLocomotion = MakeLocomotion(TEXT("BS_ARPG_PlayerLocomotion"), 500.f);
	UBlendSpace* EnemyLocomotion = MakeLocomotion(TEXT("BS_ARPG_EnemyLocomotion"), 220.f);
	if (!PlayerLocomotion || !EnemyLocomotion) return false;

	// 从本项目模板复制状态机和动画层，只替换素材与两个采样输入；原模板和原蓝图不保存。
	const auto MakeAnimBlueprint = [&](const TCHAR* Name, UBlendSpace* Locomotion) -> UAnimBlueprint*
	{
		UAnimBlueprint* Result = Load<UAnimBlueprint>(ARPGFolder / Name);
		if (!Result) Result = Keep(Cast<UAnimBlueprint>(Assets.DuplicateAsset(Name, ARPGFolder, Template)));
		if (!Result) return nullptr;
		Result->Modify(); Result->ParentClass = UmultiplayerCombatAnimInstance::StaticClass();
		Result->ParentAssetOverrides.Reset();
		FBlueprintEditorUtils::RefreshAllNodes(Result);
		TArray<UEdGraph*> Graphs; Result->GetAllGraphs(Graphs);
		int32 IdleCount = 0, JumpCount = 0, FallCount = 0, BlendCount = 0;
		int32 MoveConditionCount = 0, FallingConditionCount = 0, RemovedBasicFootIK = 0;
		for (UEdGraph* Graph : Graphs)
		{
			TArray<UEdGraphNode*> Nodes;
			for (UEdGraphNode* Node : Graph->Nodes) Nodes.Add(Node);
			for (UEdGraphNode* Node : Nodes)
			{
				// ARPG 动作没有模板 Foot IK 所需的辅助骨轨；该 Rig 会把活动脚骨重新拉回静止 IK 目标。
				// 只在本项目动画副本中旁路这一份具体 Rig，保留原状态机、缓存、动作层以及源模板。
				if (Node->GetClass()->GetFName() == TEXT("AnimGraphNode_ControlRig"))
				{
					const FStructProperty* Data = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
					const FObjectPropertyBase* Field = Data ? FindFProperty<FObjectPropertyBase>(Data->Struct, TEXT("ControlRigClass")) : nullptr;
					const UObject* Rig = Field ? Field->GetObjectPropertyValue_InContainer(Data->ContainerPtrToValuePtr<void>(Node)) : nullptr;
					if (!Rig && Data)
					{
						Field = FindFProperty<FObjectPropertyBase>(Data->Struct, TEXT("DefaultControlRigClass"));
						Rig = Field ? Field->GetObjectPropertyValue_InContainer(Data->ContainerPtrToValuePtr<void>(Node)) : nullptr;
					}
					if (Rig && Rig->GetPathName() == TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK_C"))
					{
						UEdGraphPin* Source = Node->FindPin(TEXT("Source"), EGPD_Input);
						UEdGraphPin* Pose = Node->FindPin(TEXT("Pose"), EGPD_Output);
						if (!Source || !Pose || (!Pose->LinkedTo.IsEmpty() && Source->LinkedTo.Num() != 1)) return nullptr;
						UEdGraphPin* Upstream = Source->LinkedTo.Num() == 1 ? Source->LinkedTo[0] : nullptr;
						const TArray<UEdGraphPin*> Consumers = Pose->LinkedTo;
						FBlueprintEditorUtils::RemoveNode(Result, Node, true);
						for (UEdGraphPin* Consumer : Consumers)
							if (!Graph->GetSchema()->TryCreateConnection(Upstream, Consumer)) return nullptr;
						++RemovedBasicFootIK;
						continue;
					}
				}
				// 模板 ShouldMove 还要求输入加速度，AI/远端角色会因此停留在 Idle；迁移条件改读实际运动。
				// 只改读取端，保留原事件图和原变量；旧 BlueprintUpdate 写入不会覆盖这两个原生字段。
				if (UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
				{
					const FName OldName = Getter->GetVarName();
					FName NewName = OldName;
					if (OldName == TEXT("ShouldMove")) NewName = TEXT("CombatShouldMove");
					else if (OldName == TEXT("IsFalling")) NewName = TEXT("CombatIsFalling");
					else if (OldName == TEXT("GroundSpeed")) NewName = TEXT("CombatMoveSpeed");
					if (NewName != OldName)
					{
						UEdGraphPin* Output = Getter->FindPin(OldName, EGPD_Output);
						if (!Output) return nullptr;
						const TArray<UEdGraphPin*> Consumers = Output->LinkedTo;
						Graph->GetSchema()->BreakPinLinks(*Output, true);
						// SetSelfMember(Name) 会保留旧蓝图变量 GUID，重建时反而被重定向回旧变量；显式绑定原生字段以清除旧 GUID。
						FProperty* NativeProperty = FindFProperty<FProperty>(UmultiplayerCombatAnimInstance::StaticClass(), NewName);
						if (!NativeProperty) return nullptr;
						Getter->SetFromProperty(NativeProperty, true, UmultiplayerCombatAnimInstance::StaticClass());
						Getter->ReconstructNode();
						UEdGraphPin* NewOutput = Getter->FindPin(NewName, EGPD_Output);
						if (!NewOutput)
						{
							UE_LOG(LogCombatAssetBuild, Error, TEXT("ARPG_ANIMATION_VARIABLE_BIND_FAILED asset=%s variable=%s"), *Result->GetPathName(), *NewName.ToString());
							return nullptr;
						}
						for (UEdGraphPin* Consumer : Consumers)
							if (!Graph->GetSchema()->TryCreateConnection(NewOutput, Consumer)) return nullptr;
					}
					if (NewName == TEXT("CombatShouldMove")) ++MoveConditionCount;
					if (NewName == TEXT("CombatIsFalling")) ++FallingConditionCount;
				}
				if (UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Node))
				{
					const UAnimationAsset* Animation = Player->GetAnimationAsset();
					if (!Animation) continue;
					const FString AssetName = Animation->GetName();
					if (AssetName.Contains(TEXT("Idle"))) { Player->SetAnimationAsset(Sequences[TEXT("Idle")]); ++IdleCount; }
					else if (AssetName.Contains(TEXT("Jump"))) { Player->SetAnimationAsset(Sequences[TEXT("JumpStart")]); ++JumpCount; }
					else if (AssetName.Contains(TEXT("Fall"))) { Player->SetAnimationAsset(Sequences[TEXT("FallLoop")]); ++FallCount; }
					// 保留目标骨架的普通 Land；原包的落地是 Additive，不能当作全身姿态。
				}
				UAnimGraphNode_BlendSpacePlayer* Blend = Cast<UAnimGraphNode_BlendSpacePlayer>(Node);
				if (!Blend) continue;
				Blend->SetAnimationAsset(Locomotion);
				for (FOptionalPinFromProperty& Pin : Blend->ShowPinForProperties)
					if (Pin.PropertyName == TEXT("X") || Pin.PropertyName == TEXT("Y")) Pin.bShowPin = true;
				Blend->ReconstructNode();
				const FName Fields[] = { TEXT("CombatMoveDirection"), TEXT("CombatMoveSpeed") };
				const FName Pins[] = { TEXT("X"), TEXT("Y") };
				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					UEdGraphPin* Input = Blend->FindPin(Pins[Axis], EGPD_Input);
					if (!Input) return nullptr;
					Graph->GetSchema()->BreakPinLinks(*Input, true);
					UK2Node_VariableGet* Getter = nullptr;
					for (UEdGraphNode* Candidate : Graph->Nodes)
						if (UK2Node_VariableGet* Existing = Cast<UK2Node_VariableGet>(Candidate); Existing && Existing->GetVarName() == Fields[Axis])
							{ Getter = Existing; break; }
					if (!Getter)
					{
						FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
						Getter = Creator.CreateNode(false); Getter->VariableReference.SetSelfMember(Fields[Axis]);
						Getter->NodePosX = Blend->NodePosX - 260; Getter->NodePosY = Blend->NodePosY + Axis * 90;
						Creator.Finalize();
					}
					if (!Graph->GetSchema()->TryCreateConnection(Getter->FindPin(Fields[Axis], EGPD_Output), Input)) return nullptr;
				}
				++BlendCount;
			}
		}
		if (IdleCount == 0 || JumpCount == 0 || FallCount == 0 || BlendCount == 0
			|| MoveConditionCount == 0 || FallingConditionCount == 0 || !InsertSlot(Result)
			|| !Compile(Result) || !Save(Result)) return nullptr;
		UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ANIMATION_GRAPH asset=%s idle=%d jump=%d fall=%d blend=%d moveConditions=%d fallingConditions=%d removedBasicFootIK=%d direction=X speed=Y"),
			*Result->GetPathName(), IdleCount, JumpCount, FallCount, BlendCount, MoveConditionCount, FallingConditionCount, RemovedBasicFootIK);
		return Result;
	};
	UAnimBlueprint* PlayerAnimation = MakeAnimBlueprint(TEXT("ABP_ARPGLocomotion"), PlayerLocomotion);
	UAnimBlueprint* EnemyAnimation = MakeAnimBlueprint(TEXT("ABP_ARPGEnemyLocomotion"), EnemyLocomotion);
	if (!PlayerAnimation || !EnemyAnimation) return false;

	const auto MakeMontage = [&](const TCHAR* Name, UAnimSequence* Sequence, FName Slot, float ReleaseTime = 0) -> UAnimMontage*
	{
		UAnimMontage* Result = Load<UAnimMontage>(ARPGFolder / Name);
		const bool bCreated = Result == nullptr;
		if (!Result) Result = Keep(NewObject<UAnimMontage>(CreatePackage(*(ARPGFolder / Name)), Name, RF_Public | RF_Standalone));
		if (!TargetMesh->GetSkeleton()->ContainsSlotName(Slot)) return nullptr;
		Result->Modify(); Result->SetSkeleton(TargetMesh->GetSkeleton()); Result->RemoveNotifies();
		Result->SlotAnimTracks.Reset(); Result->SlotAnimTracks.AddDefaulted_GetRef().SlotName = Slot;
		FAnimSegment Segment; Segment.SetAnimReference(Sequence, true); Segment.StartPos = 0; Segment.AnimPlayRate = 1.f;
		Result->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
		Result->CompositeSections.Reset();
		FCompositeSection& First = Result->CompositeSections.AddDefaulted_GetRef();
		First.SectionName = ReleaseTime > 0 ? TEXT("Windup") : TEXT("Default"); First.SetTime(0);
		if (ReleaseTime > 0)
		{
			First.NextSectionName = TEXT("Release");
			FCompositeSection& Release = Result->CompositeSections.AddDefaulted_GetRef();
			Release.SectionName = TEXT("Release"); Release.SetTime(ReleaseTime);
		}
		Result->SetCompositeLength(Sequence->GetPlayLength());
		Result->bEnableAutoBlendOut = Sequence != Sequences[TEXT("Death")];
		Result->BlendIn.SetBlendTime(.1f); Result->BlendOut.SetBlendTime(.15f); Result->PostEditChange();
		if (bCreated) FAssetRegistryModule::AssetCreated(Result);
		return Save(Result) ? Result : nullptr;
	};
	constexpr float AttackRelease = .159123f, AreaRelease = .321208f;
	UAnimMontage* Montages[] = {
		MakeMontage(TEXT("AM_ARPG_Attack"), Sequences[TEXT("Attack")], TEXT("UpperBody")),
		MakeMontage(TEXT("AM_ARPG_Heal"), Sequences[TEXT("Heal")], TEXT("UpperBody")),
		MakeMontage(TEXT("AM_ARPG_Death"), Sequences[TEXT("Death")], TEXT("DefaultSlot")),
		MakeMontage(TEXT("AM_ARPG_Area"), Sequences[TEXT("Area")], TEXT("UpperBody"), AreaRelease),
		MakeMontage(TEXT("AM_ARPG_Hit"), Sequences[TEXT("Hit")], TEXT("UpperBody")) };
	for (UAnimMontage* Asset : Montages) if (!Asset) return false;
	Config->Modify();
	Config->Attack.Windup = AttackRelease; Config->Attack.Recovery = Sequences[TEXT("Attack")]->GetPlayLength() - AttackRelease;
	Config->Area.Windup = AreaRelease; Config->Area.Recovery = Sequences[TEXT("Area")]->GetPlayLength() - AreaRelease;
	if (!Save(Config)) return false;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UBlueprint* Blueprint = Load<UBlueprint>(Folder / (Index == 0 ? TEXT("BP_CombatPlayer") : TEXT("BP_CombatEnemy")));
		ACharacter* Character = Blueprint && Blueprint->GeneratedClass ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
		if (!Character || !Character->GetMesh()->GetSkeletalMeshAsset()) return false;
		const UClass* CurrentAnimation = Character->GetMesh()->GetAnimClass();
		// 独立原骨架敌人已完成连接时保留它；重新生成玩家/旧版图不能把敌人悄悄切回 Quinn。
		if (Index == 1 && CurrentAnimation && CurrentAnimation->GetPathName().StartsWith(Folder / TEXT("ARPG/EnemySource/")))
		{
			UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ANIMATION_BINDING_PRESERVED enemySource=%s"), *CurrentAnimation->GetPathName());
			continue;
		}
		if (Character->GetMesh()->GetSkeletalMeshAsset()->GetSkeleton() != TargetMesh->GetSkeleton()) return false;
		Blueprint->Modify(); Character->Modify(); Character->GetMesh()->Modify();
		if (Index == 0)
		{
			// 玩家朝向跟随镜头，横移/后退由四向动画表示；不修改 Co-op 基础角色或敌人的 AI 朝向。
			// 更新转向规则：镜头独立观察，身体随移动或接纳的攻击转身；不再使用上面的跟镜头方案。
			Character->bUseControllerRotationYaw = false;
			Character->GetCharacterMovement()->bOrientRotationToMovement = true;
		}
		Character->GetMesh()->SetAnimInstanceClass(Index == 0 ? PlayerAnimation->GeneratedClass : EnemyAnimation->GeneratedClass);
		if (!SetAsset(Character, TEXT("AttackMontage"), Montages[0]) || !SetAsset(Character, TEXT("HealMontage"), Montages[1])
			|| !SetAsset(Character, TEXT("DeathMontage"), Montages[2]) || !SetAsset(Character, TEXT("AreaMontage"), Montages[3])
			|| !SetAsset(Character, TEXT("HitMontage"), Montages[4]) || !Compile(Blueprint) || !Save(Blueprint)) return false;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ANIMATIONS_CONNECTED sequences=%d montages=5 locomotionBlueprints=2 attackRelease=%.6f areaRelease=%.6f HUD=unchanged skeleton=unchanged visualPreview=NOT_RUN"),
		Sequences.Num(), AttackRelease, AreaRelease);
	return true;
}

// 敌人直接使用素材包骨架和动作；动画图由本项目构建，不接入素材包的技能、AI 或属性框架。
bool ConnectARPGEnemySource(bool bAuditOnly = false)
{
	const FString Output = Folder / TEXT("ARPG/EnemySource");
	const FString SourceFolder = TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/Animations");
	USkeletalMesh* Mesh = Load<USkeletalMesh>(TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Meshes/SKM_CombatMannequin"));
	UBlendSpace* SourceBlend = Load<UBlendSpace>(SourceFolder / TEXT("1Handed/Locomotion/BS_1HandedWeaponLocomotion"));
	UAnimMontage* SourceAttack = Load<UAnimMontage>(SourceFolder / TEXT("AI/ExampleSwordMob/SwordMob_MeleeClose01_Montage"));
	UBlueprint* Enemy = Load<UBlueprint>(Folder / TEXT("BP_CombatEnemy"));
	if (!Mesh || !SourceBlend || !SourceAttack || SourceAttack->SlotAnimTracks.IsEmpty()
		|| SourceAttack->SlotAnimTracks[0].AnimTrack.AnimSegments.IsEmpty()
		|| !Enemy || !Enemy->GeneratedClass || !Mesh->GetSkeleton()->ContainsSlotName(TEXT("DefaultSlot"))) return false;
	UAnimSequence* SwordAttack = Keep(Cast<UAnimSequence>(SourceAttack->SlotAnimTracks[0].AnimTrack.AnimSegments[0].GetAnimReference()));
	if (!SwordAttack) return false;
	const FStructProperty* SourceParameters = FindFProperty<FStructProperty>(SourceBlend->GetClass(), TEXT("BlendParameters"));
	if (!SourceParameters || SourceParameters->ArrayDim < 2) return false;
	if (bAuditOnly)
	{
		// 先读原始出手窗口和速度轴，再离线接线；审计模式不创建、编译或保存任何资产。
		for (const FSlotAnimationTrack& Track : SourceAttack->SlotAnimTracks)
			for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
				UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_SEGMENT slot=%s animation=%s start=%.6f animStart=%.6f animEnd=%.6f rate=%.6f loops=%d length=%.6f"),
					*Track.SlotName.ToString(), *GetPathNameSafe(Segment.GetAnimReference()), Segment.StartPos, Segment.AnimStartTime, Segment.AnimEndTime,
					Segment.AnimPlayRate, Segment.LoopingCount, SourceAttack->GetPlayLength());
		for (const UAnimSequenceBase* Source : { static_cast<UAnimSequenceBase*>(SourceAttack), static_cast<UAnimSequenceBase*>(SwordAttack) })
			for (const FAnimNotifyEvent& Notify : Source->Notifies)
				UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_NOTIFY animation=%s name=%s notify=%s state=%s start=%.6f end=%.6f"),
					*Source->GetPathName(), *Notify.NotifyName.ToString(), *GetPathNameSafe(Notify.Notify.Get()),
					*GetPathNameSafe(Notify.NotifyStateClass.Get()), Notify.GetTriggerTime(), Notify.GetEndTriggerTime());
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const FBlendParameter* Parameter = SourceParameters->ContainerPtrToValuePtr<FBlendParameter>(SourceBlend, Axis);
			UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_AXIS axis=%d name=%s min=%.6f max=%.6f"),
				Axis, *Parameter->DisplayName, Parameter->Min, Parameter->Max);
		}
		for (const FBlendSample& Sample : SourceBlend->GetBlendSamples())
			UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_SAMPLE animation=%s value=%s rate=%.6f"),
				*GetPathNameSafe(Sample.Animation), *Sample.SampleValue.ToString(), Sample.RateScale);
		const auto* Character = Cast<AmultiplayerCombatEnemy>(Enemy->GeneratedClass->GetDefaultObject());
		UBehaviorTree* Tree = Character ? Character->GetCombatBehaviorTree() : nullptr;
		if (!Tree || !Tree->RootNode) return false;
		TArray<UBTCompositeNode*> Pending{ Tree->RootNode };
		while (!Pending.IsEmpty())
		{
			const UBTCompositeNode* Node = Pending.Pop(EAllowShrinking::No);
			for (const FBTCompositeChild& Child : Node->Children)
			{
				if (Child.ChildComposite) Pending.Add(Child.ChildComposite);
				if (const auto* Move = Cast<UBTTask_MoveTo>(Child.ChildTask))
					UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_BT_MOVE task=%s acceptableRadius=%s includesAgentRadius=%s includesGoalRadius=%s description=%s"),
						*Move->GetPathName(), *Move->AcceptableRadius.ToString(), *Move->bReachTestIncludesAgentRadius.ToString(),
						*Move->bReachTestIncludesGoalRadius.ToString(), *Move->GetStaticDescription());
			}
		}
		return true;
	}
	if (SourceAttack->SlotAnimTracks.Num() != 1 || SourceAttack->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() != 1) return false;
	// 原 SwordMob 的挥刀轨迹窗口实测为 0.342609～0.528883 秒；只借用时间，不执行原包通知逻辑。
	const FAnimNotifyEvent* AttackTrace = SourceAttack->Notifies.FindByPredicate([](const FAnimNotifyEvent& Notify)
	{
		return Notify.NotifyName == TEXT("AttackTraceNS_C");
	});
	if (!AttackTrace || AttackTrace->GetTriggerTime() <= 0 || AttackTrace->GetTriggerTime() >= SwordAttack->GetPlayLength()) return false;
	const float AttackRelease = AttackTrace->GetTriggerTime();
	IAssetTools& Assets = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	const TPair<const TCHAR*, const TCHAR*> EnemySequences[] = {
		{ TEXT("Idle"), TEXT("1Handed/Locomotion/1Handed_Idle_Anim") },
		{ TEXT("JogFwd"), TEXT("1Handed/Locomotion/Jog_Fwd_Anim") },
		{ TEXT("JogBwd"), TEXT("1Handed/Locomotion/Jog_Bwd_Anim") },
		{ TEXT("JogLeft"), TEXT("1Handed/Locomotion/Jog_Left_Anim") },
		{ TEXT("JogRight"), TEXT("1Handed/Locomotion/Jog_Right_Anim") },
		{ TEXT("Fall"), TEXT("1Handed/Jump/GreystoneJump_Fall") },
		{ TEXT("Attack"), nullptr },
		{ TEXT("Death"), TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/Animations/Death/Death_Fwd") },
		{ TEXT("Hit"), TEXT("1Handed/HitReactions/HitReact_Front_Anim") }
	};
	TMap<FString, UAnimSequence*> Sequences, BySourceName;
	for (const auto& Entry : EnemySequences)
	{
		UAnimSequence* Source = Entry.Value
			? Load<UAnimSequence>(FString(Entry.Value).StartsWith(TEXT("/Game/")) ? FString(Entry.Value) : SourceFolder / Entry.Value)
			: SwordAttack;
		if (!Source || Source->GetSkeleton() != Mesh->GetSkeleton()) return false;
		// 近战副本与旧法师副本分开命名，避免复用旧资产时保留了错误的骨轨。
		const FString Name = FString(TEXT("AS_Enemy_Melee_")) + Entry.Key;
		UAnimSequence* Sequence = Load<UAnimSequence>(Output / Name);
		if (!Sequence) Sequence = Keep(Cast<UAnimSequence>(Assets.DuplicateAsset(Name, Output, Source)));
		if (!Sequence || Sequence->GetSkeleton() != Mesh->GetSkeleton()) return false;
		Sequence->Modify(); Sequence->RemoveNotifies();
		// 只修改本项目副本，CMC 继续控制位移；原包的骨轨、骨架、通知和 Root Motion 配置不动。
		Sequence->bEnableRootMotion = false; Sequence->bForceRootLock = true; Sequence->PostEditChange();
		if (!Save(Sequence)) return false;
		Sequences.Add(Entry.Key, Sequence); BySourceName.Add(Source->GetName(), Sequence);
	}
	UBlendSpace* Locomotion = Load<UBlendSpace>(Output / TEXT("BS_EnemyMeleeLocomotion"));
	if (!Locomotion) Locomotion = Keep(Cast<UBlendSpace>(Assets.DuplicateAsset(TEXT("BS_EnemyMeleeLocomotion"), Output, SourceBlend)));
	if (!Locomotion || Locomotion->GetSkeleton() != Mesh->GetSkeleton()
		|| Locomotion->GetNumberOfBlendSamples() != SourceBlend->GetNumberOfBlendSamples()) return false;
	Locomotion->Modify();
	const FStructProperty* Parameters = FindFProperty<FStructProperty>(Locomotion->GetClass(), TEXT("BlendParameters"));
	if (!Parameters || Parameters->ArrayDim < 2) return false;
	FBlendParameter* SpeedAxis = Parameters->ContainerPtrToValuePtr<FBlendParameter>(Locomotion, 1);
	// 原四向 Jog 样本位于 600，映射到小兵 CMC 的 220 速度；方向轴与原 14 个样本布局保持一致。
	const FBlendParameter* SourceSpeedAxis = SourceParameters->ContainerPtrToValuePtr<FBlendParameter>(SourceBlend, 1);
	SpeedAxis->Min = 0; SpeedAxis->Max = SourceSpeedAxis->Max * 220.f / 600.f; SpeedAxis->bSnapToGrid = false;
	for (int32 Index = 0; Index < SourceBlend->GetNumberOfBlendSamples(); ++Index)
	{
		const FBlendSample& Sample = SourceBlend->GetBlendSample(Index);
		UAnimSequence* const* Animation = Sample.Animation ? BySourceName.Find(Sample.Animation->GetName()) : nullptr;
		FVector Position = Sample.SampleValue; Position.Y *= 220.f / 600.f;
		if (!Animation || !Locomotion->ReplaceSampleAnimation(Index, *Animation) || !Locomotion->EditSampleValue(Index, Position)) return false;
	}
	Locomotion->TargetWeightInterpolationSpeedPerSec = 8.f;
	Locomotion->ValidateSampleData(); Locomotion->ResampleData(); Locomotion->PostEditChange();
	if (!Save(Locomotion)) return false;

	const FString AnimationPath = Output / TEXT("ABP_EnemySourceLocomotion");
	UAnimBlueprint* Animation = Load<UAnimBlueprint>(AnimationPath);
	if (!Animation)
	{
		UAnimBlueprintFactory* Factory = Keep(NewObject<UAnimBlueprintFactory>());
		Factory->ParentClass = UmultiplayerCombatAnimInstance::StaticClass();
		Factory->TargetSkeleton = Mesh->GetSkeleton(); Factory->PreviewSkeletalMesh = Mesh;
		Animation = Keep(Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(),
			CreatePackage(*AnimationPath), TEXT("ABP_EnemySourceLocomotion"), RF_Public | RF_Standalone, nullptr, GWarn)));
		if (Animation) FAssetRegistryModule::AssetCreated(Animation);
	}
	if (!Animation || Animation->TargetSkeleton != Mesh->GetSkeleton()) return false;
	Animation->Modify(); Animation->ParentClass = UmultiplayerCombatAnimInstance::StaticClass();
	Animation->SetPreviewMesh(Mesh);
	TArray<UEdGraph*> Graphs; Animation->GetAllGraphs(Graphs);
	UEdGraph* Graph = nullptr;
	for (UEdGraph* Candidate : Graphs) if (Candidate->GetFName() == TEXT("AnimGraph")) { Graph = Candidate; break; }
	UAnimGraphNode_Root* Root = Graph ? FBlueprintEditorUtils::GetAnimGraphRoot(Graph) : nullptr;
	if (!Root) return false;
	TArray<UEdGraphNode*> OldNodes;
	for (UEdGraphNode* Node : Graph->Nodes) if (Node != Root) OldNodes.Add(Node);
	for (UEdGraphNode* Node : OldNodes) FBlueprintEditorUtils::RemoveNode(Animation, Node, true);
	Root->NodePosX = 900; Root->NodePosY = 0;
	FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> MoveCreator(*Graph);
	UAnimGraphNode_BlendSpacePlayer* Move = MoveCreator.CreateNode(false);
	Move->Node.SetBlendSpace(Locomotion); Move->NodePosX = 0; Move->NodePosY = 150; MoveCreator.Finalize();
	for (FOptionalPinFromProperty& Pin : Move->ShowPinForProperties)
		if (Pin.PropertyName == TEXT("X") || Pin.PropertyName == TEXT("Y")) Pin.bShowPin = true;
	Move->ReconstructNode();
	FGraphNodeCreator<UAnimGraphNode_SequencePlayer> FallCreator(*Graph);
	UAnimGraphNode_SequencePlayer* Fall = FallCreator.CreateNode(false);
	Fall->Node.SetSequence(Sequences[TEXT("Fall")]); Fall->NodePosX = 0; Fall->NodePosY = -150; FallCreator.Finalize();
	FGraphNodeCreator<UAnimGraphNode_BlendListByBool> FallingCreator(*Graph);
	UAnimGraphNode_BlendListByBool* Falling = FallingCreator.CreateNode(false);
	Falling->NodePosX = 350; Falling->NodePosY = 0; FallingCreator.Finalize();
	FGraphNodeCreator<UAnimGraphNode_Slot> SlotCreator(*Graph);
	UAnimGraphNode_Slot* Slot = SlotCreator.CreateNode(false);
	Slot->Node.SlotName = TEXT("DefaultSlot"); Slot->Node.bAlwaysUpdateSourcePose = true;
	Slot->NodePosX = 650; Slot->NodePosY = 0; SlotCreator.Finalize();
	const FName Fields[] = { TEXT("CombatMoveDirection"), TEXT("CombatMoveSpeed"), TEXT("CombatIsFalling") };
	UEdGraphPin* Inputs[] = { Move->FindPin(TEXT("X"), EGPD_Input), Move->FindPin(TEXT("Y"), EGPD_Input),
		Falling->FindPin(TEXT("bActiveValue"), EGPD_Input) };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Getter = Creator.CreateNode(false); Getter->VariableReference.SetSelfMember(Fields[Index]);
		Getter->NodePosX = Index < 2 ? -280 : 80; Getter->NodePosY = Index < 2 ? 150 + Index * 90 : -300;
		Creator.Finalize();
		if (!Inputs[Index] || !Graph->GetSchema()->TryCreateConnection(Getter->FindPin(Fields[Index], EGPD_Output), Inputs[Index])) return false;
	}
	if (!Graph->GetSchema()->TryCreateConnection(Fall->FindPin(TEXT("Pose"), EGPD_Output), Falling->FindPin(TEXT("BlendPose_0"), EGPD_Input))
		|| !Graph->GetSchema()->TryCreateConnection(Move->FindPin(TEXT("Pose"), EGPD_Output), Falling->FindPin(TEXT("BlendPose_1"), EGPD_Input))
		|| !Graph->GetSchema()->TryCreateConnection(Falling->FindPin(TEXT("Pose"), EGPD_Output), Slot->FindPin(TEXT("Source"), EGPD_Input))
		|| !Graph->GetSchema()->TryCreateConnection(Slot->FindPin(TEXT("Pose"), EGPD_Output), Root->FindPin(TEXT("Result"), EGPD_Input))
		|| !Compile(Animation) || !Save(Animation)) return false;

	const TCHAR* MontageFields[] = { TEXT("AttackMontage"), TEXT("DeathMontage"), TEXT("HitMontage") };
	const TCHAR* Actions[] = { TEXT("Attack"), TEXT("Death"), TEXT("Hit") };
	UAnimMontage* Montages[3] = {};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Montages); ++Index)
	{
		const FString Name = FString(TEXT("AM_Enemy_")) + Actions[Index];
		UAnimMontage* Montage = Load<UAnimMontage>(Output / Name);
		const bool bCreated = Montage == nullptr;
		if (!Montage) Montage = Keep(NewObject<UAnimMontage>(CreatePackage(*(Output / Name)), *Name, RF_Public | RF_Standalone));
		Montage->Modify(); Montage->SetSkeleton(Mesh->GetSkeleton()); Montage->RemoveNotifies();
		Montage->SlotAnimTracks.Reset(); Montage->SlotAnimTracks.AddDefaulted_GetRef().SlotName = TEXT("DefaultSlot");
		UAnimSequence* Sequence = Sequences[Actions[Index]];
		FAnimSegment Segment; Segment.SetAnimReference(Sequence, true); Segment.StartPos = 0; Segment.AnimPlayRate = 1.f;
		Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
		Montage->CompositeSections.Reset();
		FCompositeSection& First = Montage->CompositeSections.AddDefaulted_GetRef();
		First.SectionName = TEXT("Default"); First.SetTime(0);
		Montage->SetCompositeLength(Sequence->GetPlayLength()); Montage->bEnableAutoBlendOut = Index != 1;
		Montage->BlendIn.SetBlendTime(.1f); Montage->BlendOut.SetBlendTime(.15f); Montage->PostEditChange();
		if (bCreated) FAssetRegistryModule::AssetCreated(Montage);
		if (!Save(Montage)) return false;
		Montages[Index] = Montage;
	}
	// 小兵只有自己的近战规则，不共享玩家的弹体、治疗和范围施法配置。
	UmultiplayerCombatConfig* Config = Load<UmultiplayerCombatConfig>(EnemyConfigPackage);
	const bool bConfigCreated = Config == nullptr;
	if (!Config) Config = Keep(NewObject<UmultiplayerCombatConfig>(CreatePackage(*EnemyConfigPackage), TEXT("DA_EnemyCombatConfig"), RF_Public | RF_Standalone));
	Config->Modify();
	Config->Attack = FmultiplayerCombatSkillRule(0, 1.8f, 15, 160, 0, AttackRelease,
		FMath::Max(.05f, Montages[0]->GetPlayLength() - AttackRelease));
	Config->ProjectileEffect.Reset(); Config->ProjectileCascadeEffect.Reset(); Config->AreaMarkerMaterial.Reset();
	Config->AttackFeedback = {}; Config->ImpactFeedback = {}; Config->CastingFeedback = {};
	Config->HealFeedback = {}; Config->DefenseFeedback = {}; Config->AreaFeedback = {};
	Config->StunFeedback = {}; Config->FailureFeedback = {};
	Config->AttackFeedback.Sound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(ObjectPath(
		TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/DemoAssets/SoundFX/A_Sword_Swing_Cue"))));
	Config->ImpactFeedback.Sound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(ObjectPath(
		TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/DemoAssets/SoundFX/A_Guardian_ImpactL_Cue"))));
	const auto* SharedFeedback = Load<UmultiplayerCombatConfig>(ConfigPackage);
	if (!SharedFeedback) return false;
	Config->ImpactFeedback.Effect = SharedFeedback->ImpactFeedback.Effect;
	Config->ImpactFeedback.CascadeEffect = SharedFeedback->ImpactFeedback.CascadeEffect;
	Config->StunFeedback = SharedFeedback->StunFeedback;
	if (bConfigCreated) FAssetRegistryModule::AssetCreated(Config);
	if (!Keep(Config->AttackFeedback.Sound.LoadSynchronous()) || !Keep(Config->ImpactFeedback.Sound.LoadSynchronous()) || !Save(Config)) return false;
	ACharacter* Character = Cast<ACharacter>(Enemy->GeneratedClass->GetDefaultObject());
	if (!Character) return false;
	Enemy->Modify(); Character->Modify(); Character->GetMesh()->Modify();
	Character->GetMesh()->SetSkeletalMesh(Mesh);
	Character->GetMesh()->SetRelativeLocation(FVector(0, 0, -Character->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight()));
	Character->GetMesh()->SetRelativeRotation(FRotator(0, -90, 0)); Character->GetMesh()->SetRelativeScale3D(FVector::OneVector);
	Character->GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Character->GetMesh()->SetAnimInstanceClass(Animation->GeneratedClass);
	// 原生基类没有骨骼网格；只在这个已接好素材的蓝图上保存手部挂点，避免空网格实例报插槽警告。
	UStaticMeshComponent* Weapon = Cast<UStaticMeshComponent>(Character->GetDefaultSubobjectByName(TEXT("EnemyWeapon")));
	if (!Weapon) return false;
	UStaticMesh* Polearm = Load<UStaticMesh>(TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/DemoAssets/Meshes/Weapons/Paragon/FengMao_Weapon"));
	const FName GripSocket(TEXT("FengMaoWeapon_Active"));
	if (!Polearm || !Character->GetMesh()->DoesSocketExist(GripSocket)
		|| Character->GetMesh()->GetSocketBoneName(GripSocket) != TEXT("hand_r")) return false;
	// 复用源素材的握持偏移；武器组件本身不再叠加一份手工位置或旋转，也不参与伤害碰撞。
	Weapon->Modify(); Weapon->SetupAttachment(Character->GetMesh(), GripSocket);
	Weapon->SetRelativeTransform(FTransform::Identity);
	Weapon->SetStaticMesh(Polearm);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Weapon->SetGenerateOverlapEvents(false);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Montages); ++Index)
		if (!SetAsset(Character, MontageFields[Index], Montages[Index])) return false;
	FFloatProperty* Distance = FindFProperty<FFloatProperty>(Character->GetClass(), TEXT("AttackDistance"));
	if (!Distance || !SetAsset(Character, TEXT("HealMontage"), nullptr) || !SetAsset(Character, TEXT("AreaMontage"), nullptr)
		|| !SetAsset(Character, TEXT("CombatConfig"), Config)
		|| !SetAsset(Character, TEXT("AttackAbilityClass"), UmultiplayerEnemyMeleeAbility::StaticClass())) return false;
	Distance->SetPropertyValue_InContainer(Character, Config->Attack.Range);
	if (!Compile(Enemy) || !Save(Enemy)) return false;
	FAssetCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCombatAssetBuild, Display, TEXT("ARPG_ENEMY_SOURCE_CONNECTED mesh=%s skeleton=%s animation=%s sequences=%d montages=3 attackSource=%s release=%.6f duration=%.6f ability=EnemyMelee gameplay=ownBT_GAS_CMC visualPreview=NOT_RUN"),
		*Mesh->GetPathName(), *Mesh->GetSkeleton()->GetPathName(), *Animation->GetPathName(), Sequences.Num(),
		*SourceAttack->GetPathName(), AttackRelease, Montages[0]->GetPlayLength());
	return true;
}

// 只更新两个固定 AI 包；复用运行时节点，编辑器图由树反建，不重建角色、HUD 或地图。
bool UpdateCombatAI(bool bValidateOnly)
{
	const FString BlackboardPackage = TEXT("/Game/Combat/AI/BB_CombatEnemy"), TreePackage = TEXT("/Game/Combat/AI/BT_CombatEnemy");
	UBlackboardData* Blackboard = Load<UBlackboardData>(BlackboardPackage);
	UBehaviorTree* Tree = Load<UBehaviorTree>(TreePackage);
	if (!Blackboard || !Tree) return false;
	const FName Keys[] = { CombatAIKeys::TargetActor, CombatAIKeys::CanAttack, CombatAIKeys::LastKnownLocation,
		CombatAIKeys::SearchUntil, CombatAIKeys::HomeLocation, CombatAIKeys::PatrolLocation, CombatAIKeys::CanMove };
	UClass* KeyTypes[] = { UBlackboardKeyType_Object::StaticClass(), UBlackboardKeyType_Bool::StaticClass(),
		UBlackboardKeyType_Vector::StaticClass(), UBlackboardKeyType_Float::StaticClass(),
		UBlackboardKeyType_Vector::StaticClass(), UBlackboardKeyType_Vector::StaticClass(), UBlackboardKeyType_Bool::StaticClass() };
	const FName Conditions[] = { CombatAIKeys::CanAttack, CombatAIKeys::TargetActor, CombatAIKeys::LastKnownLocation };
	const FName MoveKeys[] = { CombatAIKeys::TargetActor, CombatAIKeys::LastKnownLocation, CombatAIKeys::PatrolLocation };
	if (!bValidateOnly)
	{
		Blackboard->Modify(); Tree->Modify(); Blackboard->Keys.Reset();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Keys); ++Index)
		{
			FBlackboardEntry& Entry = Blackboard->Keys.AddDefaulted_GetRef();
			Entry.EntryName = Keys[Index]; Entry.bInstanceSynced = false;
			Entry.KeyType = NewObject<UBlackboardKeyType>(Blackboard, KeyTypes[Index], NAME_None, RF_Transactional);
			if (auto* Object = Cast<UBlackboardKeyType_Object>(Entry.KeyType)) Object->BaseClass = AmultiplayerCombatCharacter::StaticClass();
		}
		Blackboard->PostEditChange();
		Tree->BlackboardAsset = Blackboard; Tree->RootDecorators.Reset(); Tree->RootDecoratorOps.Reset();
		bool bConfigured = true;
		auto SetKey = [&](UObject* Node, FName Key)
		{
			const FStructProperty* Property = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("BlackboardKey"));
			if (!Property || Property->Struct != FBlackboardKeySelector::StaticStruct()) { bConfigured = false; return; }
			auto* Selector = Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
			Selector->SelectedKeyName = Key; Selector->ResolveSelectedKey(*Blackboard);
			bConfigured &= Selector->GetSelectedKeyID() != FBlackboard::InvalidKey;
		};
		auto SetByte = [&](UObject* Node, FName Name, uint8 Value)
		{
			FByteProperty* Property = FindFProperty<FByteProperty>(Node->GetClass(), Name);
			if (Property) Property->SetPropertyValue_InContainer(Node, Value); else bConfigured = false;
		};
		auto NewWait = [&](UBTCompositeNode* Parent, float Time, float Deviation)
		{
			auto* Wait = NewObject<UBTTask_Wait>(Tree, NAME_None, RF_Transactional);
			Wait->WaitTime = FValueOrBBKey_Float(Time); Wait->RandomDeviation = FValueOrBBKey_Float(Deviation);
			Parent->Children.AddDefaulted_GetRef().ChildTask = Wait;
		};
		auto* Root = NewObject<UBTComposite_Selector>(Tree, NAME_None, RF_Transactional);
		Root->NodeName = TEXT("Attack / Chase / Search / Patrol"); Tree->RootNode = Root;
		Root->Services.Add(NewObject<UmultiplayerBTService_UpdateCombatTarget>(Tree, NAME_None, RF_Transactional));
		const TCHAR* Labels[] = { TEXT("Attack visible target"), TEXT("Chase visible target"), TEXT("Search last seen location"), TEXT("Patrol near home") };
		UBTComposite_Sequence* Branches[4] = {};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Branches[Index] = NewObject<UBTComposite_Sequence>(Tree, NAME_None, RF_Transactional);
			Branches[Index]->NodeName = Labels[Index];
			FBTCompositeChild& Branch = Root->Children.AddDefaulted_GetRef(); Branch.ChildComposite = Branches[Index];
			if (Index > 0)
			{
				// 禁锢只中止追击、搜索和巡逻；攻击分支仍可独立在射程内执行。
				auto* CanMove = NewObject<UBTDecorator_Blackboard>(Tree, NAME_None, RF_Transactional);
				SetKey(CanMove, CombatAIKeys::CanMove); SetByte(CanMove, TEXT("FlowAbortMode"), EBTFlowAbortMode::Both);
				SetByte(CanMove, TEXT("BasicOperation"), EBasicKeyOperation::Set); SetByte(CanMove, TEXT("OperationType"), EBasicKeyOperation::Set);
				CanMove->BuildDescription(); Branch.Decorators.Add(CanMove);
			}
			if (Index >= UE_ARRAY_COUNT(Conditions)) continue;
			auto* Condition = NewObject<UBTDecorator_Blackboard>(Tree, NAME_None, RF_Transactional);
			SetKey(Condition, Conditions[Index]); SetByte(Condition, TEXT("FlowAbortMode"), EBTFlowAbortMode::Both);
			SetByte(Condition, TEXT("BasicOperation"), EBasicKeyOperation::Set); SetByte(Condition, TEXT("OperationType"), EBasicKeyOperation::Set);
			Condition->BuildDescription(); Branch.Decorators.Add(Condition);
		}
		Branches[0]->Children.AddDefaulted_GetRef().ChildTask = NewObject<UmultiplayerBTTask_RequestCombatAttack>(Tree, NAME_None, RF_Transactional);
		NewWait(Branches[0], .25f, 0);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			UBTComposite_Sequence* Parent = Branches[Index + 1];
			if (Index == 2) Parent->Children.AddDefaulted_GetRef().ChildTask = NewObject<UmultiplayerBTTask_SelectPatrolLocation>(Tree, NAME_None, RF_Transactional);
			auto* Move = NewObject<UBTTask_MoveTo>(Tree, NAME_None, RF_Transactional); SetKey(Move, MoveKeys[Index]);
			Move->AcceptableRadius = FValueOrBBKey_Float(50.f); Move->bObserveBlackboardValue = true;
			Move->bTrackMovingGoal = FValueOrBBKey_Bool(Index == 0); Move->bAllowPartialPath = FValueOrBBKey_Bool(false);
			Move->bReachTestIncludesAgentRadius = FValueOrBBKey_Bool(false); Move->bReachTestIncludesGoalRadius = FValueOrBBKey_Bool(false);
			FBTCompositeChild& Child = Parent->Children.AddDefaulted_GetRef(); Child.ChildTask = Move;
			Child.Decorators.Add(NewObject<UBTDecorator_ForceSuccess>(Tree, NAME_None, RF_Transactional));
			NewWait(Parent, Index == 0 ? .25f : Index == 1 ? 1.f : 1.5f, Index == 0 ? 0.f : Index == 1 ? .25f : .5f);
		}
		NewWait(Root, 1.f, 0); // 导航选点失败也等待一秒，避免空场景每帧重试寻路。
		if (!bConfigured) return false;
		UClass* GraphClass = UBehaviorTreeGraph::StaticClass();
		Tree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, MakeUniqueObjectName(Tree, GraphClass, TEXT("Behavior Tree")),
			GraphClass, UEdGraphSchema_BehaviorTree::StaticClass());
		auto* Graph = CastChecked<UBehaviorTreeGraph>(Tree->BTGraph);
		// 反建期间锁住重编译；Root 尚未连线时不能让编辑器清空新树。
		Graph->LockUpdates(); Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		for (UEdGraphNode* Node : Graph->Nodes)
			if (auto* RootGraph = Cast<UBehaviorTreeGraphNode_Root>(Node)) { RootGraph->BlackboardAsset = Blackboard; RootGraph->UpdateBlackboard(); }
		Graph->OnCreated(); Graph->Initialize(); Graph->UpdateClassData(); Graph->UnlockUpdates();
		for (UObject* Asset : { static_cast<UObject*>(Blackboard), static_cast<UObject*>(Tree) })
		{
			const FString Package = Asset->GetPackage()->GetName();
			if (Package != BlackboardPackage && Package != TreePackage) return false;
			Asset->MarkPackageDirty(); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Asset->GetPackage(), Asset,
				*FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()), Args)) return false;
		}
	}
	// ValidateCombatAI 在新进程只读冷加载，检查实际序列化的键、分支、等待和可编辑图。
	// 编辑器会按版本补入持久 SelfActor 键；业务键逐项验证，不把该引擎键当成额外玩法状态。
	if (Tree->BlackboardAsset != Blackboard || Blackboard->Keys.Num() < UE_ARRAY_COUNT(Keys)
		|| Blackboard->Keys.Num() > UE_ARRAY_COUNT(Keys) + 1) return false;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Keys); ++Index)
	{
		const FBlackboardEntry* Entry = Blackboard->GetKey(Blackboard->GetKeyID(Keys[Index]));
		if (!Entry || Entry->bInstanceSynced || !Entry->KeyType || Entry->KeyType->GetClass() != KeyTypes[Index]) return false;
	}
	const auto* TargetType = Cast<UBlackboardKeyType_Object>(Blackboard->GetKey(Blackboard->GetKeyID(Keys[0]))->KeyType);
	const auto* Root = Cast<UBTComposite_Selector>(Tree->RootNode);
	if (!TargetType || TargetType->BaseClass != AmultiplayerCombatCharacter::StaticClass() || !Root || Root->Children.Num() != 5
		|| Root->Services.Num() != 1 || !Root->Services[0] || !Root->Services[0]->IsA<UmultiplayerBTService_UpdateCombatTarget>()) return false;
	const UBlackboardComponent* NoBlackboard = nullptr;
	auto CheckWait = [NoBlackboard](const UBTTaskNode* Node, float Time, float Deviation)
	{
		const auto* Wait = Cast<UBTTask_Wait>(Node);
		return Wait && Wait->WaitTime.GetKey().IsNone() && Wait->RandomDeviation.GetKey().IsNone()
			&& FMath::IsNearlyEqual(Wait->WaitTime.GetValue(NoBlackboard), Time) && FMath::IsNearlyEqual(Wait->RandomDeviation.GetValue(NoBlackboard), Deviation);
	};
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FBTCompositeChild& Child = Root->Children[Index]; const auto* Sequence = Cast<UBTComposite_Sequence>(Child.ChildComposite);
		if (!Sequence || Sequence->Children.Num() != (Index == 3 ? 3 : 2)) return false;
		if (Index < UE_ARRAY_COUNT(Conditions))
		{
			const int32 ConditionIndex = Index == 0 ? 0 : 1;
			const auto* Condition = Child.Decorators.Num() == ConditionIndex + 1 ? Cast<UBTDecorator_Blackboard>(Child.Decorators[ConditionIndex]) : nullptr;
			const auto* Operation = Condition ? FindFProperty<FByteProperty>(Condition->GetClass(), TEXT("OperationType")) : nullptr;
			if (!Condition || Condition->GetSelectedBlackboardKey() != Conditions[Index] || Condition->GetFlowAbortMode() != EBTFlowAbortMode::Both
				|| !Operation || Operation->GetPropertyValue_InContainer(Condition) != EBasicKeyOperation::Set) return false;
		}
		if (Index > 0)
		{
			const auto* CanMove = Child.Decorators.Num() > 0 ? Cast<UBTDecorator_Blackboard>(Child.Decorators[0]) : nullptr;
			if (!CanMove || CanMove->GetSelectedBlackboardKey() != CombatAIKeys::CanMove
				|| CanMove->GetFlowAbortMode() != EBTFlowAbortMode::Both) return false;
		}
		if (!CheckWait(Sequence->Children.Last().ChildTask, Index < 2 ? .25f : Index == 2 ? 1.f : 1.5f, Index < 2 ? 0.f : Index == 2 ? .25f : .5f)) return false;
		if (Index == 0) { if (!Sequence->Children[0].ChildTask || !Sequence->Children[0].ChildTask->IsA<UmultiplayerBTTask_RequestCombatAttack>()) return false; continue; }
		if (Index == 3 && (!Sequence->Children[0].ChildTask || !Sequence->Children[0].ChildTask->IsA<UmultiplayerBTTask_SelectPatrolLocation>())) return false;
		const FBTCompositeChild& MoveChild = Sequence->Children[Index == 3 ? 1 : 0]; const auto* Move = Cast<UBTTask_MoveTo>(MoveChild.ChildTask);
		if (!Move || Move->GetSelectedBlackboardKey() != MoveKeys[Index - 1] || !Move->bObserveBlackboardValue
			|| !FMath::IsNearlyEqual(Move->AcceptableRadius.GetValue(NoBlackboard), 50.f) || Move->bTrackMovingGoal.GetValue(NoBlackboard) != (Index == 1)
			|| Move->bAllowPartialPath.GetValue(NoBlackboard) || Move->bReachTestIncludesAgentRadius.GetValue(NoBlackboard) || Move->bReachTestIncludesGoalRadius.GetValue(NoBlackboard)
			|| MoveChild.Decorators.Num() != 1 || !MoveChild.Decorators[0] || !MoveChild.Decorators[0]->IsA<UBTDecorator_ForceSuccess>()) return false;
	}
	const auto* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
	if (!CheckWait(Root->Children[4].ChildTask, 1.f, 0) || !Graph || Graph->Nodes.Num() != 16) return false;
	int32 GraphRoots = 0, Services = 0, Decorators = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const auto* BTNode = Cast<UBehaviorTreeGraphNode>(Node); if (!BTNode) return false;
		if (const auto* GraphRoot = Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			++GraphRoots;
			if (GraphRoot->BlackboardAsset != Blackboard || !BTNode->GetOutputPin() || BTNode->GetOutputPin()->LinkedTo.Num() != 1) return false;
			const auto* Linked = Cast<UBehaviorTreeGraphNode>(BTNode->GetOutputPin()->LinkedTo[0]->GetOwningNode());
			if (!Linked || Linked->NodeInstance != Root) return false;
		}
		else if (!BTNode->NodeInstance || BTNode->NodeInstance->GetOuter() != Tree) return false;
		Services += BTNode->Services.Num(); Decorators += BTNode->Decorators.Num();
	}
	const bool bValid = GraphRoots == 1 && Services == 1 && Decorators == 9;
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_AI_ASSET mode=%s valid=%d keys=%d branches=5 graphNodes=%d decorators=%d mapsSaved=0"),
		bValidateOnly ? TEXT("Validate") : TEXT("Update"), bValid, Blackboard->Keys.Num(), Graph->Nodes.Num(), Decorators);
	return bValid;
}

// 新进程冷加载验证实际 Skeleton、最终图连线和 CDO，而不是只统计资产文件。
bool Validate()
{
	// 从实际角色 CDO 出发检查正在使用的资产，不再验证已退出运行链路的旧 Quinn 子蓝图。
	const TCHAR* MeshPaths[] = { TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple"),
		TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Meshes/SKM_CombatMannequin") };
	const TCHAR* AnimationPaths[] = { TEXT("ARPG/ABP_ARPGLocomotion"), TEXT("ARPG/EnemySource/ABP_EnemySourceLocomotion") };
	const TCHAR* MontageFields[] = { TEXT("AttackMontage"), TEXT("HealMontage"), TEXT("DeathMontage"), TEXT("AreaMontage"), TEXT("HitMontage") };
	ACharacter* Characters[2] = {};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UBlueprint* Blueprint = Load<UBlueprint>(Folder / (Index == 0 ? TEXT("BP_CombatPlayer") : TEXT("BP_CombatEnemy")));
		ACharacter* CDO = Blueprint && Blueprint->GeneratedClass ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
		USkeletalMesh* Mesh = Load<USkeletalMesh>(MeshPaths[Index]);
		UAnimBlueprint* Animation = Load<UAnimBlueprint>(Folder / AnimationPaths[Index]);
		if (!CDO || !Mesh || !Animation || Animation->ParentClass != UmultiplayerCombatAnimInstance::StaticClass()
			|| Animation->TargetSkeleton != Mesh->GetSkeleton() || CDO->GetMesh()->GetSkeletalMeshAsset() != Mesh
			|| CDO->GetMesh()->GetAnimClass() != Animation->GeneratedClass) return false;
		Characters[Index] = CDO;
		int32 FullSlots = 0, UpperSlots = 0; bool bOutputConnected = false, bLayerConnected = Index == 1, bLegAnimated = false;
		TSet<FName> NativeReads; TArray<UEdGraph*> Graphs; Animation->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Index == 0 && Node->GetClass()->GetFName() == TEXT("AnimGraphNode_ControlRig"))
				{
					const FStructProperty* Data = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
					const FObjectPropertyBase* Field = Data ? FindFProperty<FObjectPropertyBase>(Data->Struct, TEXT("ControlRigClass")) : nullptr;
					const UObject* Rig = Field ? Field->GetObjectPropertyValue_InContainer(Data->ContainerPtrToValuePtr<void>(Node)) : nullptr;
					if (!Rig && Data)
					{
						Field = FindFProperty<FObjectPropertyBase>(Data->Struct, TEXT("DefaultControlRigClass"));
						Rig = Field ? Field->GetObjectPropertyValue_InContainer(Data->ContainerPtrToValuePtr<void>(Node)) : nullptr;
					}
					if (Rig && Rig->GetPathName() == TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK_C")) return false;
				}
				if (const auto* Slot = Cast<UAnimGraphNode_Slot>(Node))
					{ FullSlots += Slot->Node.SlotName == TEXT("DefaultSlot"); UpperSlots += Slot->Node.SlotName == TEXT("UpperBody"); }
				if (const auto* Getter = Cast<UK2Node_VariableGet>(Node))
				{
					const FName Name = Getter->GetVarName();
					if (Name == TEXT("ShouldMove") || Name == TEXT("IsFalling") || Name == TEXT("GroundSpeed")) return false;
					NativeReads.Add(Name);
				}
				if (const auto* Player = Cast<UAnimGraphNode_SequencePlayer>(Node))
				{
					const auto* Sequence = Cast<UAnimSequence>(Player->GetAnimationAsset());
					if (!Sequence || Sequence->GetSkeleton() != Mesh->GetSkeleton() || !Sequence->GetDataModel()
						|| Sequence->GetDataModel()->GetNumBoneTracks() == 0) return false;
				}
				if (const auto* Blend = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
				{
					const UBlendSpace* Space = Cast<UBlendSpace>(Blend->GetAnimationAsset());
					if (!Space || Space->GetSkeleton() != Mesh->GetSkeleton() || Space->GetNumberOfBlendSamples() < 5) return false;
					for (const FBlendSample& Sample : Space->GetBlendSamples())
					{
						UAnimSequence* Sequence = Sample.Animation;
						if (!Sequence || Sequence->GetSkeleton() != Mesh->GetSkeleton() || !Sequence->GetDataModel()
							|| Sequence->GetDataModel()->GetNumBoneTracks() == 0) return false;
						const int32 Calf = Mesh->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(TEXT("calf_l"));
						if (Calf == INDEX_NONE) return false;
						FTransform First, Later; Sequence->GetBoneTransform(First, FSkeletonPoseBoneIndex(Calf), 0., true);
						Sequence->GetBoneTransform(Later, FSkeletonPoseBoneIndex(Calf), Sequence->GetPlayLength() * .37, true);
						bLegAnimated |= First.GetRotation().AngularDistance(Later.GetRotation()) > .01;
					}
				}
			}
			if (Graph->GetFName() != TEXT("AnimGraph")) continue;
			UAnimGraphNode_Root* Root = FBlueprintEditorUtils::GetAnimGraphRoot(Graph);
			const UEdGraphPin* Input = Root ? Root->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
			const auto* Slot = Input && Input->LinkedTo.Num() == 1 ? Cast<UAnimGraphNode_Slot>(Input->LinkedTo[0]->GetOwningNode()) : nullptr;
			const UEdGraphPin* Source = Slot ? Slot->FindPin(TEXT("Source"), EGPD_Input) : nullptr;
			bOutputConnected = Slot && Slot->Node.SlotName == TEXT("DefaultSlot") && Source && Source->LinkedTo.Num() == 1;
			if (Index == 0 && bOutputConnected) bLayerConnected = HasCombatUpperBodyLayer(Cast<UAnimGraphNode_LayeredBoneBlend>(Source->LinkedTo[0]->GetOwningNode()));
		}
		if (FullSlots != 1 || UpperSlots != (Index == 0 ? 1 : 0) || !bOutputConnected || !bLayerConnected || !bLegAnimated
			|| !NativeReads.Contains(TEXT("CombatMoveSpeed")) || !NativeReads.Contains(TEXT("CombatMoveDirection"))
			|| !NativeReads.Contains(TEXT("CombatIsFalling")) || (Index == 0 && !NativeReads.Contains(TEXT("CombatShouldMove")))) return false;
		for (int32 Action = 0; Action < UE_ARRAY_COUNT(MontageFields); ++Action)
		{
			const FObjectPropertyBase* Field = FindFProperty<FObjectPropertyBase>(CDO->GetClass(), MontageFields[Action]);
			const UAnimMontage* Montage = Field ? Cast<UAnimMontage>(Field->GetObjectPropertyValue_InContainer(CDO)) : nullptr;
			if (Index == 1 && (Action == 1 || Action == 3))
			{
				// 小兵不再携带玩家的治疗与范围施法动作，旧未引用资产不影响当前链路。
				if (!Field || Montage) return false;
				continue;
			}
			const FName ExpectedSlot = Index == 0 && Action != 2 ? TEXT("UpperBody") : TEXT("DefaultSlot");
			if (!Montage || Montage->GetSkeleton() != Mesh->GetSkeleton() || Montage->SlotAnimTracks.Num() != 1
				|| Montage->SlotAnimTracks[0].SlotName != ExpectedSlot || Montage->GetPlayLength() <= 0 || !Montage->Notifies.IsEmpty()) return false;
			for (const FAnimSegment& Segment : Montage->SlotAnimTracks[0].AnimTrack.AnimSegments)
			{
				const auto* Sequence = Cast<UAnimSequence>(Segment.GetAnimReference());
				if (!Sequence || Sequence->GetSkeleton() != Mesh->GetSkeleton() || !Sequence->GetDataModel()
					|| Sequence->GetDataModel()->GetNumBoneTracks() == 0 || !Sequence->Notifies.IsEmpty() || !FMath::IsNearlyEqual(Segment.AnimPlayRate, 1.f)) return false;
				if (Index == 1 && (!Sequence->GetName().StartsWith(TEXT("AS_Enemy_Melee_")) || Sequence->bEnableRootMotion)) return false;
			}
		}
		if (Index == 1)
		{
			const auto* Minion = Cast<AmultiplayerCombatEnemy>(CDO);
			const auto* Rules = Minion ? Minion->GetCombatConfigAsset() : nullptr;
			const UAnimMontage* Attack = Minion ? Minion->GetActionMontage(EMultiplayerCombatAnimation::Attack) : nullptr;
			const auto* Weapon = Minion ? Cast<UStaticMeshComponent>(CDO->GetDefaultSubobjectByName(TEXT("EnemyWeapon"))) : nullptr;
			const auto* Polearm = Load<UStaticMesh>(TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/DemoAssets/Meshes/Weapons/Paragon/FengMao_Weapon"));
			if (!Weapon || !Polearm || Weapon->GetStaticMesh() != Polearm
				|| Weapon->GetAttachParent() != Minion->GetMesh() || Weapon->GetAttachSocketName() != TEXT("FengMaoWeapon_Active")
				|| Minion->GetMesh()->GetSocketBoneName(Weapon->GetAttachSocketName()) != TEXT("hand_r")
				|| !Weapon->GetRelativeTransform().Equals(FTransform::Identity)
				|| Weapon->GetCollisionEnabled() != ECollisionEnabled::NoCollision || Weapon->GetGenerateOverlapEvents()) return false;
			if (!Minion || Minion->GetAttackAbilityClass() != UmultiplayerEnemyMeleeAbility::StaticClass()
				|| !Rules || !Attack || Rules->GetPackage()->GetName() != EnemyConfigPackage
				|| !FMath::IsNearlyEqual(Rules->Attack.Amount, 15.f) || !FMath::IsNearlyEqual(Rules->Attack.Cooldown, 1.8f)
				|| !FMath::IsNearlyEqual(Rules->Attack.Range, 160.f) || !FMath::IsNearlyEqual(Minion->GetAttackDistance(), Rules->Attack.Range)
				|| !FMath::IsNearlyEqual(Rules->Attack.Windup + Rules->Attack.Recovery, Attack->GetPlayLength(), .001f)
				|| Rules->Attack.Cooldown < Attack->GetPlayLength()
				|| !Rules->ProjectileEffect.IsNull() || !Rules->ProjectileCascadeEffect.IsNull() || !Rules->AreaMarkerMaterial.IsNull()
				|| !Rules->AttackFeedback.Effect.IsNull() || !Rules->AttackFeedback.CascadeEffect.IsNull()
				|| !Rules->CastingFeedback.Effect.IsNull() || !Rules->CastingFeedback.CascadeEffect.IsNull()
				|| !Keep(Rules->AttackFeedback.Sound.LoadSynchronous()) || !Keep(Rules->ImpactFeedback.Sound.LoadSynchronous())) return false;
			UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_MINION_VALIDATED config=%s ability=EnemyMelee damage=%.2f range=%.2f cooldown=%.2f windup=%.6f recovery=%.6f supportMontages=none projectile=none"),
				*Rules->GetPathName(), Rules->Attack.Amount, Rules->Attack.Range, Rules->Attack.Cooldown, Rules->Attack.Windup, Rules->Attack.Recovery);
		}
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_CHARACTER_VALIDATED asset=%s skeleton=%s nativeMotionReads=verified legBoneTracks=animated slots=connected"), *Blueprint->GetPathName(), *Mesh->GetSkeleton()->GetPathName());
	}
	UmultiplayerCombatConfig* Config = Load<UmultiplayerCombatConfig>(ConfigPackage);
	if (!Config || !Keep(Config->AreaMarkerMaterial.LoadSynchronous())) return false;
	FmultiplayerCombatFeedback Projectile; Projectile.Effect = Config->ProjectileEffect; Projectile.CascadeEffect = Config->ProjectileCascadeEffect;
	for (const FmultiplayerCombatFeedback* Feedback : { &Projectile, &Config->AttackFeedback, &Config->ImpactFeedback, &Config->HealFeedback,
		&Config->DefenseFeedback, &Config->AreaFeedback, &Config->StunFeedback, &Config->FailureFeedback })
	{
		if (!Feedback->Sound.IsNull() && !Keep(Feedback->Sound.LoadSynchronous())) return false;
		if (!Feedback->CascadeEffect.IsNull())
			{ const auto* System = Keep(Feedback->CascadeEffect.LoadSynchronous()); if (!System || System->Emitters.IsEmpty()) return false; }
		else if (!Feedback->Effect.IsNull())
		{
			UNiagaraSystem* System = Keep(Feedback->Effect.LoadSynchronous()); if (!System) return false;
			System->RequestCompile(false); System->WaitForCompilationComplete(false, false);
			if (!System->IsValid() || !System->IsReadyToRun()) return false;
		}
		else if (Feedback != &Config->FailureFeedback) return false;
	}
	for (const TCHAR* Name : { TEXT("Attack"), TEXT("Impact"), TEXT("Heal"), TEXT("Defense"), TEXT("Casting"), TEXT("Area"), TEXT("Stun"), TEXT("Failure") })
	{
		UBlueprint* Cue = Load<UBlueprint>(Folder / TEXT("Cues") / (FString(TEXT("GCN_")) + Name));
		UObject* CDO = Cue && Cue->GeneratedClass ? Cue->GeneratedClass->GetDefaultObject() : nullptr;
		const FNameProperty* Field = CDO ? FindFProperty<FNameProperty>(CDO->GetClass(), TEXT("GameplayCueName")) : nullptr;
		if (!Field || Field->GetPropertyValue_InContainer(CDO) != FName(*(FString(TEXT("GameplayCue.Combat.")) + Name))) return false;
	}
	// 原 HUD 和准星仍在；新增表现只是原控件树里的一个子控件，敌人血条也使用自己的 GAS 事件父类。
	UBlueprint* Mode = Load<UBlueprint>(TEXT("/Game/Combat/BP_CombatGameMode"));
	UBlueprint* HUD = Load<UBlueprint>(TEXT("/Game/Combat/UI/BP_CombatHUD"));
	const FObjectPropertyBase* HUDField = Mode && Mode->GeneratedClass ? FindFProperty<FObjectPropertyBase>(Mode->GeneratedClass, TEXT("HUDClass")) : nullptr;
	if (!HUD || !HUDField || HUDField->GetObjectPropertyValue_InContainer(Mode->GeneratedClass->GetDefaultObject()) != HUD->GeneratedClass) return false;
	UWidgetBlueprint* Original = Load<UWidgetBlueprint>(TEXT("/Game/Combat/UI/WBP_CombatHUD"));
	UWidgetBlueprint* Visuals = Load<UWidgetBlueprint>(Folder / TEXT("UI/WBP_ARPGCombatVisuals"));
	const UWidget* Child = Original && Original->WidgetTree ? Original->WidgetTree->FindWidget(TEXT("ARPGCombatVisuals")) : nullptr;
	if (!Visuals || Visuals->ParentClass != UmultiplayerCombatWidget::StaticClass() || !Child || Child->GetClass() != Visuals->GeneratedClass
		|| !Original->WidgetTree->FindWidget(TEXT("CombatCrosshair")) || Original->UbergraphPages.IsEmpty()) return false;
	UWidgetBlueprint* EnemyWidget = Load<UWidgetBlueprint>(Folder / TEXT("UI/WBP_ARPGEnemyHealth"));
	const auto* HealthComponent = Cast<UWidgetComponent>(Characters[1]->GetDefaultSubobjectByName(TEXT("HealthBarWidget")));
	if (!EnemyWidget || EnemyWidget->ParentClass != UmultiplayerEnemyHealthWidget::StaticClass() || !HealthComponent
		|| HealthComponent->GetWidgetClass().Get() != EnemyWidget->GeneratedClass.Get() || HealthComponent->GetWidgetSpace() != EWidgetSpace::World
		|| !HealthComponent->GetManuallyRedraw() || !HealthComponent->IsA<UmultiplayerEnemyHealthWidgetComponent>()) return false;
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_VALIDATE actualCDOs=2 skeletons=matched montageSlots=verified nativeMotion=verified effects=verified originalHUD=preserved enemyWidget=verified visualPreview=NOT_RUN"));
	return true;
}
}
#endif

int32 UCombatAssetBuildCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	const bool bCreate = FParse::Param(*Params, TEXT("Create"));
	const bool bValidate = FParse::Param(*Params, TEXT("Validate"));
	const bool bUpdateUI = FParse::Param(*Params, TEXT("UpdateUI"));
	const bool bUpdateAnimations = FParse::Param(*Params, TEXT("UpdateAnimations"));
	const bool bConnectARPG = FParse::Param(*Params, TEXT("ConnectARPGAnimations"));
	const bool bConnectARPGHUD = FParse::Param(*Params, TEXT("ConnectARPGHUD"));
	const bool bRefreshCombatFeedback = FParse::Param(*Params, TEXT("RefreshCombatFeedbackAndHUD"));
	const bool bConnectARPGEnemyHealth = FParse::Param(*Params, TEXT("ConnectARPGEnemyHealth"));
	const bool bUpdateEnemyWorldHealth = FParse::Param(*Params, TEXT("UpdateEnemyWorldHealth"));
	const bool bConnectARPGEnemySource = FParse::Param(*Params, TEXT("ConnectARPGEnemySource"));
	const bool bAuditARPGEnemySource = FParse::Param(*Params, TEXT("AuditARPGEnemySource"));
	const bool bFixCombatInput = FParse::Param(*Params, TEXT("FixCombatInput"));
	const bool bUpdateCombatAI = FParse::Param(*Params, TEXT("UpdateCombatAI"));
	const bool bValidateCombatAI = FParse::Param(*Params, TEXT("ValidateCombatAI"));
	if (int32(bCreate) + int32(bValidate) + int32(bUpdateUI) + int32(bUpdateAnimations) + int32(bConnectARPG) + int32(bConnectARPGHUD) + int32(bRefreshCombatFeedback) + int32(bConnectARPGEnemyHealth) + int32(bUpdateEnemyWorldHealth) + int32(bConnectARPGEnemySource) + int32(bAuditARPGEnemySource) + int32(bFixCombatInput) + int32(bUpdateCombatAI) + int32(bValidateCombatAI) != 1) return 1;
	const bool bSuccess = bUpdateCombatAI || bValidateCombatAI ? CombatAssets::UpdateCombatAI(bValidateCombatAI)
		: bAuditARPGEnemySource ? CombatAssets::ConnectARPGEnemySource(true) : bFixCombatInput ? CombatAssets::FixCombatInput() : bConnectARPGEnemySource ? CombatAssets::ConnectARPGEnemySource() : bConnectARPG ? CombatAssets::ConnectARPGAnimations()
		: bUpdateEnemyWorldHealth ? CombatAssets::UpdateEnemyWorldHealth() : bRefreshCombatFeedback ? CombatAssets::RefreshCombatFeedbackAndHUD() : bConnectARPGHUD ? CombatAssets::ConnectARPGHUD() : bConnectARPGEnemyHealth ? CombatAssets::ConnectARPGEnemyHealth() : bUpdateAnimations ? CombatAssets::UpdateAnimations()
		: bUpdateUI ? CombatAssets::CreateUI(true) : bCreate ? CombatAssets::Create() : CombatAssets::Validate();
	// 离线操作结束即释放临时强引用，不让工具对象滞留到编辑器模块销毁阶段。
	CombatAssets::References.Reset();
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_ASSET_BUILD mode=%s success=%d"),
		bUpdateCombatAI ? TEXT("UpdateCombatAI") : bValidateCombatAI ? TEXT("ValidateCombatAI") : bAuditARPGEnemySource ? TEXT("AuditARPGEnemySource") : bFixCombatInput ? TEXT("FixCombatInput") : bConnectARPGEnemySource ? TEXT("ConnectARPGEnemySource") : bConnectARPG ? TEXT("ConnectARPGAnimations") : bUpdateEnemyWorldHealth ? TEXT("UpdateEnemyWorldHealth") : bRefreshCombatFeedback ? TEXT("RefreshCombatFeedbackAndHUD") : bConnectARPGHUD ? TEXT("ConnectARPGHUD") : bConnectARPGEnemyHealth ? TEXT("ConnectARPGEnemyHealth") : bUpdateAnimations ? TEXT("UpdateAnimations") : bUpdateUI ? TEXT("UpdateUI") : bCreate ? TEXT("Create") : TEXT("Validate"), bSuccess);
	return bSuccess ? 0 : 1;
#else
	return 1;
#endif
}
