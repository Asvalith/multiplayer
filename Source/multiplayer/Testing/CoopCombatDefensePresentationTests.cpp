// Copyright Epic Games, Inc. All Rights Reserved.

// 回归 Q 的持续防御状态、动作到期与正式蓝图／HUD 接线，不用替代界面掩盖缺失配置。
// 状态与动画均来自保存的角色资产；本组不是实机视觉或网络传输验收。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/multiplayerGameMode.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/InputKeyDelegateBinding.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
#include "Animation/AnimBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_VariableGet.h"
#include "WidgetBlueprint.h"
#endif

namespace
{
	// 使用保存的正式角色与动画图，只关闭场景移动；不模拟联网确认、不修改资产。
	struct FDefensePresentationWorld
	{
		UWorld* World = nullptr;
		AmultiplayerCombatCharacter* Player = nullptr;
		UmultiplayerCombatAnimInstance* Animation = nullptr;
		ULocalPlayer* InputPlayer = nullptr;
		uint64 PreviousFrame = GFrameCounter;

		// 按需走正式本地接纳与输入映射；其余姿态测试仍只加载保存的玩家及动画图。
		explicit FDefensePresentationWorld(bool bInput = false)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			if (bInput)
			{
				auto* Instance = NewObject<UGameInstance>(GEngine);
				World->SetGameInstance(Instance);
				Context.OwningGameInstance = Instance;
				World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
				World->SetGameMode(FURL());
			}
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
			if (!PlayerClass) return;
			if (bInput)
			{
				auto* Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
				if (!Mode) return;
				Mode->DefaultPawnClass = PlayerClass;
				auto* Start = World->SpawnActor<APlayerStart>(FVector(0, 0, 110), FRotator::ZeroRotator);
				auto* PC = World->SpawnActor<AmultiplayerCoopPlayerController>();
				if (!Start || !PC) return;
				InputPlayer = NewObject<ULocalPlayer>(GEngine);
				World->GetGameInstance()->AddLocalPlayer(InputPlayer, FPlatformUserId::CreateFromInternalId(0));
				PC->SetPlayer(InputPlayer);
				PC->StartSpot = Start;
				Mode->RestartPlayer(PC);
				if (auto* Input = InputPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
				{
					FModifyContextOptions Options;
					Options.bForceImmediately = true;
					Input->RequestRebuildControlMappings(Options);
				}
				Player = Cast<AmultiplayerCombatCharacter>(PC->GetPawn());
			}
			else Player = World->SpawnActor<AmultiplayerCombatCharacter>(PlayerClass, FVector(0, 0, 110), FRotator::ZeroRotator);
			if (!Player) return;
			Player->GetCharacterMovement()->DisableMovement();
			Player->GetCharacterMovement()->SetComponentTickEnabled(false);
			Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Player->GetMesh()->bEnableUpdateRateOptimizations = false;
			Animation = Cast<UmultiplayerCombatAnimInstance>(Player->GetMesh()->GetAnimInstance());
			Advance(.3f);
		}

		// 结束表现对象及世界，恢复帧号，不让动作和定时器跨测试残留。
		~FDefensePresentationWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			if (InputPlayer) InputPlayer->PlayerRemoved();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 推进正式世界与动画更新，让持续效果自然到期而非手动删标签代替计时。
		void Advance(float Seconds)
		{
			for (int32 Frame = 0; Frame < FMath::CeilToInt(Seconds / .02f); ++Frame)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, .02f);
			}
		}

		// 立即从真实角色读取动画状态，断言标签变化后的姿态选择。
		void ReadState()
		{
			Animation->NativeUpdateAnimation(.02f);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDefensePoseLifecycleTest, "Multiplayer.Combat.DefensePoseLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实 Q 的持续效果控制姿态；Ability 结束、控制中断和重新接纳不能留下旧姿态或停止新动作。
bool FCoopCombatDefensePoseLifecycleTest::RunTest(const FString& Parameters)
{
	FDefensePresentationWorld Fixture;
	if (!TestNotNull(TEXT("Formal player spawned for defense pose"), Fixture.Player)
		|| !TestNotNull(TEXT("Formal player uses its native-backed animation"), Fixture.Animation)) return false;
	auto* Player = Fixture.Player;
	auto* ASC = Player->GetAbilitySystemComponent();
	TestFalse(TEXT("Idle has no defensive pose"), Fixture.Animation->CombatIsDefending);
	Player->Defend();
	Fixture.ReadState();
	TestTrue(TEXT("Real Q grants its persistent defense effect"), Player->IsDefending());
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerDefendAbility::StaticClass());
	TestTrue(TEXT("Instant Q has already ended while its effect remains"), Spec && !Spec->IsActive());
	TestTrue(TEXT("Defense pose follows the lasting effect, not ability activation"), Fixture.Animation->CombatIsDefending);
	Player->ApplyStun(.2f);
	Fixture.ReadState();
	TestTrue(TEXT("The defensive effect remains while stunned"), Player->IsDefending());
	TestFalse(TEXT("Stun suppresses the active guard pose"), Fixture.Animation->CombatIsDefending);
	Fixture.Advance(.3f);
	TestTrue(TEXT("Remaining defense resumes after control expires"), Fixture.Animation->CombatIsDefending);
	Player->SetCombatSessionReady(false);
	Fixture.ReadState();
	TestFalse(TEXT("Unadmitted pawn does not raise its guard"), Fixture.Animation->CombatIsDefending);
	Player->SetCombatSessionReady(true);
	Fixture.ReadState();
	TestTrue(TEXT("Readmission recovers the still-active guard"), Fixture.Animation->CombatIsDefending);
	Player->Attack();
	UAnimMontage* Attack = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	if (!TestTrue(TEXT("An overlapping real attack takes its normal montage slot"),
		Attack && Fixture.Animation->Montage_IsPlaying(Attack))) return false;
	ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(CombatTags::State_Defending));
	Fixture.ReadState();
	TestFalse(TEXT("Removing the owning effect clears its guard pose"), Fixture.Animation->CombatIsDefending);
	TestTrue(TEXT("Guard removal cannot stop an already-started attack montage"), Fixture.Animation->Montage_IsPlaying(Attack));
	Fixture.Advance(1.5f);
	ASC->AddLooseGameplayTag(CombatTags::State_Defending);
	ASC->AddLooseGameplayTag(CombatTags::State_Defending);
	Fixture.ReadState();
	TestTrue(TEXT("Two tag contributions still select one guard pose"), Fixture.Animation->CombatIsDefending);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Defending);
	Fixture.ReadState();
	TestTrue(TEXT("Removing one contribution retains the remaining guard"), Fixture.Animation->CombatIsDefending);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Defending);
	Fixture.ReadState();
	TestFalse(TEXT("Last tag removal also covers rejected-prediction tag cleanup"), Fixture.Animation->CombatIsDefending);
	// 只验证标签清理后的展示状态，不把本地加减标签冒充真实服务器预测拒绝。
	ASC->AddLooseGameplayTag(CombatTags::State_Defending);
	Player->GetCoopComponent()->RestoreDowned(2.f);
	Fixture.ReadState();
	TestFalse(TEXT("Downed state suppresses guard even if a tag arrives late"), Fixture.Animation->CombatIsDefending);
	Player->GetCoopComponent()->ClearLifeState();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	Fixture.ReadState();
	TestFalse(TEXT("Death suppresses a late surviving defensive tag"), Fixture.Animation->CombatIsDefending);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Defending);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDefenseRightClickCancellationTest, "Multiplayer.Combat.DefenseRightClickCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实 Q 松开确认后 GA 已结束；右键仍须撤下持续格挡，而不是只取消一个已不活动的 Ability。
// 此处验证正式 PlayerInput 与动画／效果边界；单世界 key0 不冒充远端预测拒绝或旧包隔离。
bool FCoopCombatDefenseRightClickCancellationTest::RunTest(const FString& Parameters)
{
	FDefensePresentationWorld Fixture(true);
	if (!TestNotNull(TEXT("Formal input player spawned for right-click guard cancellation"), Fixture.Player)
		|| !TestNotNull(TEXT("Input player uses the saved defense animation graph"), Fixture.Animation)) return false;
	auto* Player = Fixture.Player;
	auto* PC = Cast<APlayerController>(Player->GetController());
	if (!TestTrue(TEXT("Real local input and production pawn bindings are installed"),
		PC && Cast<UEnhancedPlayerInput>(PC->PlayerInput) && Player->InputComponent)
		|| !TestTrue(TEXT("Normal admission permits skills"), Player->IsCombatReady())) return false;
	const auto Input = [&](const FKey& Key, EInputEvent Event)
	{
		PC->InputKey(FInputKeyParams(Key, Event, Event == IE_Pressed ? 1. : 0.));
		Fixture.Advance(.02f);
	};
	// 已有护盾也不能被“结束防御”误删；它不是持续格挡效果的退款账本。
	Player->GrantShield(25.f);
	const float EnergyBefore = Player->GetEnergy();
	Input(EKeys::Q, IE_Pressed);
	TestTrue(TEXT("Actual Q press enters defense preview without granting defense"),
		Player->IsTargetingSkill() && !Player->IsDefending());
	Input(EKeys::Q, IE_Released);
	// 输入和 Mesh 可能在同一世界 Tick 的不同顺序执行；与本文件其余姿态用例一样，
	// 在状态断言前从正式 NativeUpdateAnimation 同步采样，不把上一帧快照当作当前结果。
	Fixture.ReadState();
	const auto* Spec = Player->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerDefendAbility::StaticClass());
	if (!TestTrue(TEXT("Q release commits a lasting guard after its ability has ended"),
		Player->IsDefending() && Player->IsParrying() && Spec && !Spec->IsActive())) return false;
	TestEqual(TEXT("The confirmed guard has paid its configured cost once"), Player->GetEnergy(),
		EnergyBefore - Player->GetSkillRule(EMultiplayerCombatSkill::Defend).Cost);
	TestTrue(TEXT("The saved animation has entered its guard pose"), Fixture.Animation->CombatIsDefending);
	Input(EKeys::LeftMouseButton, IE_Pressed);
	Input(EKeys::LeftMouseButton, IE_Released);
	UAnimMontage* Attack = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	if (!TestTrue(TEXT("A later normal attack plays while the guard effect still lasts"),
		Player->IsDefending() && Attack && Fixture.Animation->Montage_IsPlaying(Attack))) return false;
	const float EnergyCommitted = Player->GetEnergy();
	const float Cooldown = Player->GetSkillCooldownRemaining(EMultiplayerCombatSkill::Defend);
	const float Shield = Player->GetShield();
	TestTrue(TEXT("The preservation check has a real shield and committed cooldown"), Shield > 0 && Cooldown > 0);
	Input(EKeys::RightMouseButton, IE_Pressed);
	Input(EKeys::RightMouseButton, IE_Released);
	Fixture.ReadState();
	TestFalse(TEXT("Actual right-click ends the lasting defense even though Q ability already ended"), Player->IsDefending());
	TestFalse(TEXT("Right-click also ends the associated short parry window"), Player->IsParrying());
	TestFalse(TEXT("Guard state immediately leaves the saved animation graph"), Fixture.Animation->CombatIsDefending);
	TestTrue(TEXT("Ending guard does not stop the newer attack montage"), Fixture.Animation->Montage_IsPlaying(Attack));
	TestEqual(TEXT("Ending guard does not refund committed energy"), Player->GetEnergy(), EnergyCommitted);
	TestEqual(TEXT("Ending guard preserves the independent existing shield"), Player->GetShield(), Shield);
	TestTrue(TEXT("Ending guard preserves its original cooldown deadline"),
		FMath::IsNearlyEqual(Player->GetSkillCooldownRemaining(EMultiplayerCombatSkill::Defend), Cooldown - .04f, .01f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDefensePoseExpiryTest, "Multiplayer.Combat.DefensePoseExpiry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 防御 GE 自然到期后，角色标签和动画防御姿态必须一起退出。
bool FCoopCombatDefensePoseExpiryTest::RunTest(const FString& Parameters)
{
	FDefensePresentationWorld Fixture;
	if (!TestNotNull(TEXT("Formal player is available for timed guard"), Fixture.Player)
		|| !TestNotNull(TEXT("Timed guard uses the actual animation instance"), Fixture.Animation)) return false;
	Fixture.Player->Defend();
	Fixture.Advance(.1f);
	TestTrue(TEXT("Timed real defense selects its pose"), Fixture.Animation->CombatIsDefending);
	const float Remaining = Fixture.Player->GetDefenseRemaining();
	Fixture.Advance(Remaining + .15f);
	TestFalse(TEXT("Real effect expiration removes its tag"), Fixture.Player->IsDefending());
	TestFalse(TEXT("Expired effect returns the animation to its base pose"), Fixture.Animation->CombatIsDefending);
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatFormalPresentationWiringTest, "Multiplayer.Combat.FormalPresentationWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 直接检查磁盘正式 GameMode/角色的 CDO，绝不在测试中换 HUD 或替换正式类来绕过缺失接线。
bool FCoopCombatFormalPresentationWiringTest::RunTest(const FString& Parameters)
{
	UClass* ModeClass = LoadClass<AGameModeBase>(nullptr,
		TEXT("/Game/Combat/BP_CombatGameMode.BP_CombatGameMode_C"));
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	UClass* OriginalHUDClass = LoadClass<AHUD>(nullptr,
		TEXT("/Game/Combat/UI/BP_CombatHUD.BP_CombatHUD_C"));
	if (!TestNotNull(TEXT("Saved formal GameMode loads unchanged"), ModeClass)
		|| !TestNotNull(TEXT("Saved formal player class loads unchanged"), PlayerClass)
		|| !TestNotNull(TEXT("Existing original HUD blueprint class still exists"), OriginalHUDClass)) return false;
	const auto* Mode = ModeClass->GetDefaultObject<AGameModeBase>();
	UClass* HUDClass = Mode->HUDClass.Get();
	TestTrue(TEXT("Formal GameMode retains its actual player blueprint"), Mode->DefaultPawnClass.Get() == PlayerClass);
	TestTrue(TEXT("Formal GameMode retains the original HUD blueprint path"), HUDClass == OriginalHUDClass);
	if (!TestTrue(TEXT("Formal HUD now inherits native targeting-range drawing"),
		HUDClass && HUDClass->IsChildOf(AmultiplayerCombatHUD::StaticClass()))) return false;
	const auto* WidgetField = FindFProperty<FObjectPropertyBase>(HUDClass, TEXT("CombatWidgetClass"));
	if (!TestNotNull(TEXT("Native widget selection is reflected on the formal HUD"), WidgetField)) return false;
	TestNull(TEXT("Original HUD creates its own widget without a duplicate native widget"),
		WidgetField->GetObjectPropertyValue_InContainer(HUDClass->GetDefaultObject()));
	const auto* HUDBlueprint = Cast<UBlueprint>(HUDClass->ClassGeneratedBy);
	TestTrue(TEXT("Original HUD retains its existing event graphs"), HUDBlueprint && !HUDBlueprint->UbergraphPages.IsEmpty());
	const auto* Widget = LoadObject<UWidgetBlueprint>(nullptr,
		TEXT("/Game/Combat/UI/WBP_CombatHUD.WBP_CombatHUD"));
	TestTrue(TEXT("Original widget and crosshair are retained"), Widget && Widget->WidgetTree
		&& Widget->WidgetTree->FindWidget(TEXT("CombatCrosshair"))
		&& Widget->WidgetTree->FindWidget(TEXT("ARPGCombatVisuals")) && !Widget->UbergraphPages.IsEmpty());
	int32 Overrides = 0;
	for (UClass* Class = PlayerClass; Class; Class = Class->GetSuperClass())
	{
		const auto* Generated = Cast<UBlueprintGeneratedClass>(Class);
		if (!Generated) continue;
		for (const auto& Object : Generated->DynamicBindingObjects)
			if (const auto* Binding = Cast<UInputKeyDelegateBinding>(Object))
				for (const auto& Entry : Binding->InputKeyDelegateBindings)
				{
					const FKey Key = Entry.InputChord.Key;
					const bool bSkillKey = Key == EKeys::Q || Key == EKeys::E || Key == EKeys::R
						|| Key == EKeys::LeftShift || Key == EKeys::RightShift
						|| Key == EKeys::LeftMouseButton || Key == EKeys::RightMouseButton;
					if (!bSkillKey) continue;
					++Overrides;
					AddError(FString::Printf(TEXT("Formal input still overrides native targeting: class=%s key=%s event=%d function=%s"),
						*Class->GetPathName(), *Key.ToString(), int32(Entry.InputKeyEvent), *Entry.FunctionNameToBind.ToString()));
				}
	}
	TestEqual(TEXT("No formal blueprint skill-key binding bypasses press-preview/release-confirm"), Overrides, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDefensePoseLayerTest, "Multiplayer.Combat.DefensePoseLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 检查保存动画图的真实连线；单有 bool 或未接出的 SequencePlayer 不能算防御动作已接好。
bool FCoopCombatDefensePoseLayerTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Combat/Presentation/ARPG/ABP_ARPGLocomotion.ABP_ARPGLocomotion"));
	UAnimSequence* Guard = LoadObject<UAnimSequence>(nullptr,
		TEXT("/Game/Combat/Presentation/Animations/AS_Defend.AS_Defend"));
	if (!TestNotNull(TEXT("Formal player animation blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("Retargeted blocking loop exists"), Guard)) return false;
	int32 ConnectedGuards = 0;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const auto* Upper = Cast<UAnimGraphNode_Slot>(Node);
			if (!Upper || Upper->Node.SlotName != TEXT("UpperBody")) continue;
			const UEdGraphPin* Source = Upper->FindPin(TEXT("Source"), EGPD_Input);
			const auto* Blend = Source && Source->LinkedTo.Num() == 1
				? Cast<UAnimGraphNode_BlendListByBool>(Source->LinkedTo[0]->GetOwningNode()) : nullptr;
			if (!Blend) continue;
			const UEdGraphPin* Active = Blend->FindPin(TEXT("bActiveValue"), EGPD_Input);
			const auto* Getter = Active && Active->LinkedTo.Num() == 1
				? Cast<UK2Node_VariableGet>(Active->LinkedTo[0]->GetOwningNode()) : nullptr;
			const UEdGraphPin* Pose = Blend->FindPin(TEXT("BlendPose_0"), EGPD_Input);
			const auto* Sequence = Pose && Pose->LinkedTo.Num() == 1
				? Cast<UAnimGraphNode_SequencePlayer>(Pose->LinkedTo[0]->GetOwningNode()) : nullptr;
			ConnectedGuards += Getter && Getter->GetVarName() == TEXT("CombatIsDefending")
				&& Sequence && Sequence->GetAnimationAsset() == Guard;
		}
	TestEqual(TEXT("Guard loop is selected by the real state and feeds the upper-body action source exactly once"), ConnectedGuards, 1);
	TestTrue(TEXT("Guard and the formal locomotion use the same retargeted skeleton"), Guard->GetSkeleton() == Blueprint->TargetSkeleton);
	TestTrue(TEXT("Guard does not move the actor through root motion"), !Guard->bEnableRootMotion);
	return !HasAnyErrors();
}
#endif

#endif
