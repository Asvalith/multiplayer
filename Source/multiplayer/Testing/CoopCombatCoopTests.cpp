// Copyright Epic Games, Inc. All Rights Reserved.

// 回归倒地、救援独占、引导打断和终局规则，并检查正式倒地动作的生命周期。
// 单世界用例验证业务与 RepNotify 契约；实际传输另由双客户端场景验收。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatTags.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Components/ActorComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"

namespace
{
	// 玩家都有真实 PC/ASC；仅用原生外观隔离动画与 AI，救援由正式 GAS 能力和计时任务执行。
	struct FCombatCoopWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;

		// 按用例选择真实 GameMode 和端类型，建立可推进救援与动作计时的独立世界。
		FCombatCoopWorld(bool bWithGameMode = false,ENetMode NetMode=NM_Standalone)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(NetMode);
			if (bWithGameMode)
			{
				auto* Instance = NewObject<UGameInstance>(GEngine);
				World->SetGameInstance(Instance);
				Context.OwningGameInstance = Instance;
				World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
				World->SetGameMode(FURL());
			}
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			if (NetMode!=NM_Standalone) World->WorldType=EWorldType::PIE;
		}

		// 对称结束世界和对象生命周期，并恢复测试前帧号。
		~FCombatCoopWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 小步推进正式 GAS 任务、倒地计时和表现更新，不直接调用完成回调。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.01f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Elapsed += Step;
			}
		}

		// 生成受控角色并使用独立短时配置，避免修改共享资产或等待完整线上时长。
		AmultiplayerCombatCharacter* Character(const FVector& Location, bool bEnableDowned = false,UClass* Class=nullptr)
		{
			auto* Character = World->SpawnActor<AmultiplayerCombatCharacter>(Class ? Class : AmultiplayerCombatCharacter::StaticClass(),Location, FRotator::ZeroRotator);
			auto* Config = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), Character);
			Config->BleedoutDuration = 2.f;
			Config->RescueDuration = .4f;
			Config->RescueRange = 250.f;
			FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("CombatConfig"))->SetObjectPropertyValue_InContainer(Character, Config);
			Character->GetCoopComponent()->bEnableDowned = bEnableDowned;
			auto* Controller = World->SpawnActor<APlayerController>();
			Controller->SetPlayerState(World->SpawnActor<APlayerState>());
			Controller->Possess(Character);
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			return Character;
		}

		// 创建真实视线阻挡，验证救援过程会复核遮挡而不是只检查初始距离。
		AActor* Wall(const FVector& Location)
		{
			auto* Actor = World->SpawnActor<AActor>();
			auto* Box = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Box);
			Box->SetBoxExtent(FVector(10, 120, 150));
			Box->SetCollisionProfileName(TEXT("BlockAll"));
			Box->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}
	};

	// 经正式伤害 GE 造成致死结果，让倒地与死亡按属性通知链触发。
	void LethalDamage(AmultiplayerCombatCharacter* Character)
	{
		auto* ASC = Character->GetAbilitySystemComponent();
		auto Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
		Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.f);
		ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
	}

	// 同时观察救援组件和 GAS 标签，防止结束后只清理了其中一份状态。
	bool HasRescueState(AmultiplayerCombatCharacter* Character)
	{
		return Character->GetCoopComponent()->IsRescuing()
			|| Character->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Rescuing);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRescueCompletionTest, "Multiplayer.Combat.Coop.RescueCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 致死伤害走属性结算进入倒地；救援只恢复配置的生命比例，不洗掉旧消耗和冷却。
bool FCoopCombatRescueCompletionTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture;
	auto* Target = Fixture.Character(FVector(0, 0, 100), true);
	auto* Rescuer = Fixture.Character(FVector(-150, 0, 100));
	auto* TargetASC = Target->GetAbilitySystemComponent();
	TargetASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 70.f);
	Target->Heal();
	Target->Defend();
	const float EnergyBeforeDowned = Target->GetEnergy();
	const float HealCooldown = Target->GetHealCooldownRemaining();
	const float DefendCooldown = Target->GetDefenseCooldownRemaining();
	if (!TestTrue(TEXT("Target has real spent energy and existing support cooldowns"),
		EnergyBeforeDowned < Target->GetMaxEnergy() && HealCooldown > 0.f && DefendCooldown > 0.f)) return false;
	LethalDamage(Target);
	if (!TestTrue(TEXT("A controlled target with a ready partner enters downed state"), Target->GetCoopComponent()->IsDowned())) return false;
	TestEqual(TEXT("Downed state commits exactly one placeholder health"), Target->GetHealth(), 1.f);
	TestTrue(TEXT("Downed is not final death"), Target->IsAlive());
	TestFalse(TEXT("Downed player cannot participate in normal gameplay"), Target->IsCombatReady());
	TestFalse(TEXT("Downed does not grant the final-death tag"), TargetASC->HasMatchingGameplayTag(CombatTags::State_Dead));
	TestEqual(TEXT("Downed locks actual movement"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	TestEqual(TEXT("Downed never refills spent energy"), Target->GetEnergy(), EnergyBeforeDowned);
	Target->Attack();
	TestFalse(TEXT("Downed player cannot activate normal attacks"), TargetASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
	Rescuer->Rescue();
	if (!TestTrue(TEXT("The actual rescue ability reserves the downed player"), HasRescueState(Rescuer))) return false;
	TestEqual(TEXT("Rescue locks actual rescuer movement"), Rescuer->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	Fixture.Advance(.15f);
	TestTrue(TEXT("Real timer advances the rescue progress"), Rescuer->GetCoopComponent()->GetRescueProgress() > .25f);
	TestEqual(TEXT("Target is not healed during the unfinished channel"), Target->GetHealth(), 1.f);
	Fixture.Advance(.3f);
	TestFalse(TEXT("Completed rescue clears downed state"), Target->GetCoopComponent()->IsDowned());
	TestTrue(TEXT("Completed rescue restores the already admitted player"), Target->IsCombatReady());
	TestEqual(TEXT("Rescue restores thirty percent, not full health"), Target->GetHealth(), Target->GetMaxHealth() * .3f);
	TestEqual(TEXT("Rescue does not refill the target's energy"), Target->GetEnergy(), EnergyBeforeDowned);
	TestTrue(TEXT("Heal cooldown continues from its existing deadline"), FMath::IsNearlyEqual(Target->GetHealCooldownRemaining(), HealCooldown - .45f, .03f));
	TestTrue(TEXT("Defense cooldown continues from its existing deadline"), FMath::IsNearlyEqual(Target->GetDefenseCooldownRemaining(), DefendCooldown - .45f, .03f));
	TestFalse(TEXT("Completed rescue releases both reservation and owned state"), HasRescueState(Rescuer));
	TestEqual(TEXT("Completion restores target's previous movement mode"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	TestEqual(TEXT("Completion restores rescuer's previous movement mode"), Rescuer->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	TestFalse(TEXT("A late duplicate completion cannot heal again"), Rescuer->GetCoopComponent()->CompleteRescue());
	Fixture.Advance(2.1f);
	TestTrue(TEXT("Old bleedout timer cannot kill the successfully rescued player"), Target->IsCombatReady());
	TestEqual(TEXT("Old callbacks do not repeat rescue settlement"), Target->GetHealth(), Target->GetMaxHealth() * .3f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRescueReservationTest, "Multiplayer.Combat.Coop.RescueReservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 两名玩家争抢同一个倒地目标：首次接纳者独占，释放后第二名才能接手，不重复结算。
bool FCoopCombatRescueReservationTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture;
	auto* Target = Fixture.Character(FVector(0, 0, 100), true);
	auto* First = Fixture.Character(FVector(-150, 0, 100));
	auto* Second = Fixture.Character(FVector(0, -150, 100));
	LethalDamage(Target);
	First->Rescue();
	if (!TestTrue(TEXT("First real ability owns the reservation"), HasRescueState(First))) return false;
	Second->Rescue();
	TestFalse(TEXT("Second ability cannot reserve an occupied target"), HasRescueState(Second));
	TestFalse(TEXT("Occupied target fails the second rescuer's eligibility query"), Second->GetCoopComponent()->CanStartRescue());
	Fixture.Advance(.1f);
	First->CancelRescue();
	TestFalse(TEXT("Release input cancels the first ability and frees its reservation"), HasRescueState(First));
	Second->Rescue();
	if (!TestTrue(TEXT("Second player can immediately reserve after the first releases"), HasRescueState(Second))) return false;
	Fixture.Advance(.45f);
	TestFalse(TEXT("Second rescue finishes normally"), HasRescueState(Second));
	TestEqual(TEXT("Only one completed rescue settles health"), Target->GetHealth(), Target->GetMaxHealth() * .3f);
	First->Rescue();
	TestFalse(TEXT("A standing target cannot be reserved again"), HasRescueState(First));
	Fixture.Advance(.5f);
	TestEqual(TEXT("Cancelled first task cannot deliver a late second heal"), Target->GetHealth(), Target->GetMaxHealth() * .3f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRescueReachabilityTest, "Multiplayer.Combat.Coop.RescueReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 起手和持续引导都检查实际距离与 Visibility 射线；不是只在按下 F 时验证一次。
bool FCoopCombatRescueReachabilityTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture;
	auto* Target = Fixture.Character(FVector(0, 0, 100), true);
	auto* Rescuer = Fixture.Character(FVector(-400, 0, 100));
	LethalDamage(Target);
	Rescuer->Rescue();
	TestFalse(TEXT("Out-of-range rescue is rejected before a channel starts"), HasRescueState(Rescuer));
	Rescuer->SetActorLocation(FVector(-150, 0, 100));
	auto* Wall = Fixture.Wall(FVector(-75, 0, 100));
	Rescuer->Rescue();
	TestFalse(TEXT("Real collision wall blocks rescue start"), HasRescueState(Rescuer));
	Wall->Destroy();
	Rescuer->Rescue();
	if (!TestTrue(TEXT("Removing the actual wall permits rescue"), HasRescueState(Rescuer))) return false;
	Wall = Fixture.Wall(FVector(-75, 0, 100));
	Fixture.Advance(.12f);
	TestFalse(TEXT("A new wall interrupts the ongoing channel"), HasRescueState(Rescuer));
	TestTrue(TEXT("Interrupted target remains downed"), Target->GetCoopComponent()->IsDowned());
	Wall->Destroy();
	Rescuer->Rescue();
	if (!TestTrue(TEXT("Wall cancellation released the target for a new attempt"), HasRescueState(Rescuer))) return false;
	Target->SetActorLocation(FVector(500, 0, 100));
	Fixture.Advance(.12f);
	TestFalse(TEXT("Moving the target out of range also cancels the real channel"), HasRescueState(Rescuer));
	Fixture.Advance(.5f);
	TestEqual(TEXT("Old wall/range-cancelled tasks cannot heal later"), Target->GetHealth(), 1.f);
	TestEqual(TEXT("Cancelled reachability attempts restore rescuer movement"), Rescuer->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRescueInterruptCleanupTest, "Multiplayer.Combat.Coop.RescueInterruptCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 释放、眩晕、两端销毁与死亡都要取消任务、释放占用；后续任务到时不能继续复活原目标。
bool FCoopCombatRescueInterruptCleanupTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture;
	for (int32 Exit = 0; Exit < 6; ++Exit)
	{
		const float Row = Exit * 1000.f;
		auto* Target = Fixture.Character(FVector(0, Row, 100), true);
		auto* Rescuer = Fixture.Character(FVector(-150, Row, 100));
		if (Exit == 4) const_cast<UmultiplayerCombatConfig*>(Target->GetCombatConfigAsset())->BleedoutDuration = .2f;
		LethalDamage(Target);
		Rescuer->Rescue();
		if (!TestTrue(*FString::Printf(TEXT("Exit %d starts an actual reserved rescue"), Exit), HasRescueState(Rescuer))) return false;
		Fixture.Advance(.1f);
		if (Exit == 0) Rescuer->CancelRescue();
		else if (Exit == 1) Rescuer->ApplyStun(.2f);
		else if (Exit == 2)
		{
			LethalDamage(Rescuer);
			TestFalse(TEXT("Rescuer's lethal damage really commits final death"), Rescuer->IsAlive());
		}
		else if (Exit == 3) Rescuer->Destroy();
		else if (Exit == 4)
		{
			Fixture.Advance(.15f);
			TestFalse(TEXT("Target's real bleedout deadline commits final death"), Target->IsAlive());
		}
		else Target->Destroy();
		TestFalse(*FString::Printf(TEXT("Exit %d releases the component reservation and owned ability state"), Exit), HasRescueState(Rescuer));
		if (Exit < 4)
		{
			auto* Replacement = Fixture.Character(FVector(0, Row - 150, 100));
			Replacement->Rescue();
			TestTrue(*FString::Printf(TEXT("Exit %d frees the target for another actual rescuer"), Exit), HasRescueState(Replacement));
			Replacement->CancelRescue();
		}
		Fixture.Advance(.5f);
		TestFalse(*FString::Printf(TEXT("Exit %d cannot restart rescue from an old timer"), Exit), HasRescueState(Rescuer));
		if (Exit < 4) TestEqual(TEXT("Interrupted rescue never delivers delayed healing"), Target->GetHealth(), 1.f);
		if (Exit <= 1) TestEqual(TEXT("Living interrupted rescuer regains its prior movement mode"), Rescuer->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDownedRecoveryDeadlineTest, "Multiplayer.Combat.Coop.DownedRecoveryDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 快照仅恢复倒地剩余期限，不恢复占用关系或引导能力；离线耗时必须扣除，不能刷新失血时间。
bool FCoopCombatDownedRecoveryDeadlineTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture;
	auto* Target = Fixture.Character(FVector(0, 0, 100), true);
	auto* Rescuer = Fixture.Character(FVector(-150, 0, 100));
	LethalDamage(Target);
	Fixture.Advance(.2f);
	const float FirstRemaining = Target->GetCoopComponent()->GetDownedRemaining();
	TestFalse(TEXT("Entering downed a second time is rejected"), Target->GetCoopComponent()->TryEnterDowned());
	TestTrue(TEXT("Rejected duplicate cannot refresh the original bleedout deadline"),
		FMath::IsNearlyEqual(Target->GetCoopComponent()->GetDownedRemaining(), FirstRemaining, .001f));
	Rescuer->Rescue();
	if (!TestTrue(TEXT("Pre-disconnect player has a real active rescue"), HasRescueState(Rescuer))) return false;
	Fixture.Advance(.1f);
	const float RemainingAtDisconnect = Target->GetCoopComponent()->GetDownedRemaining();
	const FmultiplayerCombatResumeState TargetSnapshot = Target->CaptureResumeState();
	const FmultiplayerCombatResumeState RescuerSnapshot = Rescuer->CaptureResumeState();
	Target->Destroy();
	TestFalse(TEXT("Target destruction immediately cancels its rescuer"), HasRescueState(Rescuer));
	Rescuer->Destroy();

	auto* RestoredTarget = Fixture.Character(FVector(0, 0, 100), true);
	RestoredTarget->SetCombatSessionReady(false);
	RestoredTarget->RestoreResumeState(TargetSnapshot, .4f);
	RestoredTarget->SetCombatSessionReady(true);
	auto* RestoredRescuer = Fixture.Character(FVector(-150, 0, 100));
	RestoredRescuer->SetCombatSessionReady(false);
	RestoredRescuer->RestoreResumeState(RescuerSnapshot, .4f);
	RestoredRescuer->SetCombatSessionReady(true);
	TestTrue(TEXT("Reconnected downed state is restored, not respawned at full health"), RestoredTarget->GetCoopComponent()->IsDowned());
	TestEqual(TEXT("Reconnected downed player retains placeholder health"), RestoredTarget->GetHealth(), 1.f);
	TestFalse(TEXT("Readmission cannot bypass downed gameplay restrictions"), RestoredTarget->IsCombatReady());
	TestTrue(TEXT("Offline time is subtracted from remaining bleedout"),
		FMath::IsNearlyEqual(RestoredTarget->GetCoopComponent()->GetDownedRemaining(), RemainingAtDisconnect - .4f, .03f));
	TestFalse(TEXT("Reconnection never resumes the previous rescue task or reservation"), HasRescueState(RestoredRescuer));
	TestEqual(TEXT("Reconnected rescuer has no stale progress bar"), RestoredRescuer->GetCoopComponent()->GetRescueProgress(), 0.f);
	Fixture.Advance(.6f);
	TestTrue(TEXT("Old rescue completion deadline cannot finish on reconnected characters"), RestoredTarget->GetCoopComponent()->IsDowned());
	TestEqual(TEXT("No old channel heals the reconnected target"), RestoredTarget->GetHealth(), 1.f);
	Fixture.Advance(RestoredTarget->GetCoopComponent()->GetDownedRemaining() + .1f);
	TestFalse(TEXT("Remaining bleedout expires into final death"), RestoredTarget->IsAlive());
	TestFalse(TEXT("Final death removes downed state"), RestoredTarget->GetCoopComponent()->IsDowned());
	TestFalse(TEXT("Final death clears the actual downed tag"), RestoredTarget->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Downed));

	auto* Expired = Fixture.Character(FVector(2000, 0, 100), true);
	Expired->SetCombatSessionReady(false);
	Expired->RestoreResumeState(TargetSnapshot, 5.f);
	TestFalse(TEXT("Offline expiry commits death immediately instead of granting a fresh bleedout duration"), Expired->IsAlive());
	TestFalse(TEXT("Expired reconnect cannot retain a downed timer"), Expired->GetCoopComponent()->IsDowned());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTeamDefeatTest, "Multiplayer.Combat.Coop.TeamDefeatAndPersistentResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 使用真实 GameMode/State 与两名受控制玩家；结果通过复制快照恢复，不靠重新播放死亡事件。
bool FCoopCombatTeamDefeatTest::RunTest(const FString& Parameters)
{
	FCombatCoopWorld Fixture(true);
	auto* Mode = Fixture.World->GetAuthGameMode<AmultiplayerGameMode>();
	auto* State = Fixture.World->GetGameState<AmultiplayerCoopGameState>();
	if (!TestNotNull(TEXT("Team defeat uses the actual production GameMode"), Mode)
		|| !TestNotNull(TEXT("Production GameMode initializes its actual cooperative GameState"), State)) return false;
	FmultiplayerCoopObjectiveState Initial;
	Initial.RequiredKeys = 2;
	State->ApplyAuthoritativeState(Initial);
	TestTrue(TEXT("The live round accepts a genuine key commitment before defeat"), Mode->RegisterActivatedKey([] { return true; }));
	auto* First = Fixture.Character(FVector(0, 0, 100), true);
	auto* Last = Fixture.Character(FVector(-150, 0, 100), true);
	if (!TestTrue(TEXT("Both server pawns have real player controllers"), First->IsPlayerControlled() && Last->IsPlayerControlled())) return false;

	// 使用现有反射函数接收真实动态广播，避免新增探针类或改全局 ProcessEvent；每次重复操作都检查标记未再翻转。
	auto* LostMarker = NewObject<USceneComponent>(State);
	LostMarker->RegisterComponent();
	LostMarker->SetActive(false);
	FScriptDelegate LostNotification;
	LostNotification.BindUFunction(LostMarker, GET_FUNCTION_NAME_CHECKED(UActorComponent, ToggleActive));
	State->OnGameLost.Add(LostNotification);
	LethalDamage(First);
	TestTrue(TEXT("The first lethal hit enters actual downed state while a partner can act"), First->GetCoopComponent()->IsDowned());
	TestTrue(TEXT("The final standing partner is still combat ready"), Last->IsCombatReady());
	TestFalse(TEXT("One downed player is not a team defeat"), State->HasGameLost());
	Mode->NotifyCombatPlayerDeath();
	TestFalse(TEXT("Explicit team reevaluation still preserves the rescue opportunity"), State->HasGameLost());
	TestFalse(TEXT("Downing the first player emits no lost notification"), LostMarker->IsActive());
	LethalDamage(Last);
	TestFalse(TEXT("The last actionable player cannot down when nobody can rescue"), Last->IsAlive());
	TestFalse(TEXT("The last player's final death is not another downed state"), Last->GetCoopComponent()->IsDowned());
	TestTrue(TEXT("Actual final death notifies GameMode and commits team defeat"), State->HasGameLost());
	TestFalse(TEXT("Defeat cannot also be victory"), State->HasGameWon());
	TestTrue(TEXT("The initial lost transition emits its actual local broadcast"), LostMarker->IsActive());

	int32 KeyCommitments = 0;
	TestFalse(TEXT("Lost round rejects another key before consuming it"), Mode->RegisterActivatedKey([&] { ++KeyCommitments; return true; }));
	TestEqual(TEXT("Rejected key callback never consumes a key"), KeyCommitments, 0);
	TestEqual(TEXT("Rejected key does not advance shared progress"), State->GetObjectiveState().ActivatedKeys, 1);
	TestFalse(TEXT("The lost key fixture is still incomplete, isolating the defeat guard"), State->IsObjectiveComplete());
	// 单独提供已收齐钥匙的权威快照，避免胜利拒绝只因目标未完成而假通过。
	auto CompletedButLost = State->GetObjectiveState();
	CompletedButLost.ActivatedKeys = CompletedButLost.RequiredKeys;
	State->ApplyAuthoritativeState(CompletedButLost);
	TestTrue(TEXT("Victory boundary fixture really satisfies its objective"), State->IsObjectiveComplete());
	TestFalse(TEXT("Even sufficient players and keys cannot win a lost round"), Mode->TryCompleteCoopGame(2, 2));
	TestTrue(TEXT("Victory rejection retains defeat and never flips the won flag"), State->HasGameLost() && !State->HasGameWon());
	TestTrue(TEXT("Progress-only snapshot change does not rebroadcast the lost transition"), LostMarker->IsActive());

	State->GetClass()->SetUpRuntimeReplicationData();
	TArray<FLifetimeProperty> StateReplication;
	State->GetLifetimeReplicatedProps(StateReplication);
	const auto* SnapshotProperty = FindFProperty<FStructProperty>(State->GetClass(), TEXT("ObjectiveState"));
	const auto* SnapshotRegistration = SnapshotProperty ? StateReplication.FindByPredicate([SnapshotProperty](const FLifetimeProperty& Entry)
		{ return Entry.RepIndex == SnapshotProperty->RepIndex; }) : nullptr;
	TestTrue(TEXT("The persistent result is in the actual replicated RepNotify snapshot"), SnapshotRegistration && SnapshotProperty->HasAnyPropertyFlags(CPF_Net)
		&& SnapshotProperty->RepNotifyFunc == FName(TEXT("OnRep_ObjectiveState"))
		&& FindFProperty<FBoolProperty>(FmultiplayerCoopObjectiveState::StaticStruct(), TEXT("bGameLost")));
	const FmultiplayerCoopObjectiveState Snapshot = State->GetObjectiveState();
	TestTrue(TEXT("Current result snapshot explicitly carries loss for late consumers"), Snapshot.bGameLost && !Snapshot.bGameWon);
	auto* LatePlayer = Fixture.Character(FVector(1000, 0, 100));
	TestTrue(TEXT("A newly created server player reads the existing lost result without a historical event"),
		LatePlayer->GetWorld()->GetGameState<AmultiplayerCoopGameState>()->HasGameLost());
	UFunction* RepNotify = State->FindFunction(TEXT("OnRep_ObjectiveState"));
	if (!TestNotNull(TEXT("The production result RepNotify remains available"), RepNotify)) return false;
	for (int32 Repeat = 0; Repeat < 3; ++Repeat)
	{
		Mode->NotifyCombatPlayerDeath();
		TestTrue(TEXT("Repeated death notification cannot broadcast loss again"), LostMarker->IsActive());
		State->ApplyAuthoritativeState(Snapshot);
		TestTrue(TEXT("Applying the same authoritative snapshot cannot broadcast loss again"), LostMarker->IsActive());
		// 直接测试 RepNotify 的边沿语义，不把这个单世界函数调用称作真实网络传输。
		State->ProcessEvent(RepNotify, nullptr);
		TestTrue(TEXT("Repeated delivery of the same result does not repeat the lost broadcast"), LostMarker->IsActive());
	}
	State->OnGameLost.Remove(LostNotification);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDownedPresentationTest,"Multiplayer.Combat.Coop.DownedPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 正式角色倒地、获救、死亡及 EndPlay 均正确维护同一动作实例，不残留延迟表现回调。
bool FCoopCombatDownedPresentationTest::RunTest(const FString&)
{
	FCombatCoopWorld Fixture;
	UClass* Class=LoadClass<AmultiplayerCombatCharacter>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Formal saved player presentation BP loads"),Class)) return false;
	auto* Target=Fixture.Character(FVector(0,0,100),true,Class);
	auto* Rescuer=Fixture.Character(FVector(-150,0,100));
	if (!TestNotNull(TEXT("Real BP target spawns with owned native attributes"),Target)) return false;
	const auto* DeathField=FindFProperty<FObjectPropertyBase>(Target->GetClass(),TEXT("DeathMontage"));
	auto* Death=DeathField ? Cast<UAnimMontage>(DeathField->GetObjectPropertyValue_InContainer(Target)) : nullptr;
	auto* Animation=Target->GetMesh()->GetAnimInstance();
	if (!TestNotNull(TEXT("Formal player has its actual animation instance"),Animation)
		|| !TestNotNull(TEXT("Formal player has its configured death montage"),Death)) return false;
	TestFalse(TEXT("Downed montage holds its final pose rather than automatically returning to idle"),Death->bEnableAutoBlendOut);
	const FVector OriginalScale=Target->GetActorScale3D();
	LethalDamage(Target);
	TestTrue(TEXT("Real lethal GE enters downed, not final death"),Target->GetCoopComponent()->IsDowned() && Target->IsAlive());
	for (int32 Repeat=0;Repeat<3;++Repeat) Target->RequestLifePresentationRefresh();
	Fixture.Advance(.05f);
	auto* First=Animation->GetActiveInstanceForMontage(Death);
	if (!TestNotNull(TEXT("Deferred local state starts the actual configured montage"),First)) return false;
	const int32 FirstId=First->GetInstanceID();
	const float FirstPosition=First->GetPosition();
	for (int32 Repeat=0;Repeat<3;++Repeat) Target->RequestLifePresentationRefresh();
	Fixture.Advance(.1f);
	First=Animation->GetActiveInstanceForMontage(Death);
	TestTrue(TEXT("Repeated notifications neither replace the instance nor rewind its time"),First && First->GetInstanceID()==FirstId && First->GetPosition()>=FirstPosition);
	TestTrue(TEXT("Pose playback never enables ragdoll or changes actor scale"),!Target->GetMesh()->IsSimulatingPhysics() && Target->GetActorScale3D().Equals(OriginalScale));
	Rescuer->Rescue();
	if (!TestTrue(TEXT("Real rescue ability begins against the posed downed target"),HasRescueState(Rescuer))) return false;
	Fixture.Advance(.7f);
	TestTrue(TEXT("Actual rescue still restores the production life state"),Target->IsCombatReady() && !Target->GetCoopComponent()->IsDowned());
	TestNull(TEXT("Rescue stops the owned downed montage after its short blend"),Animation->GetActiveInstanceForMontage(Death));
	TestEqual(TEXT("Presentation did not replace the original movement restoration"),Target->GetCharacterMovement()->MovementMode.GetValue(),MOVE_Flying);
	// 模拟重连先收到权威 HP1，再恢复剩余倒地时长；这里不冒充真实网络传输。
	Target->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(),1.f);
	Target->GetCoopComponent()->RestoreDowned(1.5f);
	Fixture.Advance(.05f);
	auto* Restored=Animation->GetActiveInstanceForMontage(Death);
	if (!TestNotNull(TEXT("Restored downed state also starts local presentation"),Restored)) return false;
	const int32 RestoredId=Restored->GetInstanceID();
	Fixture.Advance(1.6f);
	TestFalse(TEXT("The original BleedOut callback stack still commits final death"),Target->IsAlive());
	Restored=Animation->GetActiveInstanceForMontage(Death);
	TestTrue(TEXT("ClearLifeState followed by death in one stack retains the existing montage instance"),Restored && Restored->GetInstanceID()==RestoredId);
	UFunction* LifeNotify=Target->GetCoopComponent()->FindFunction(TEXT("NotifyLifeState"));
	UFunction* DeadNotify=Target->FindFunction(TEXT("OnRep_Dead"));
	if (!TestNotNull(TEXT("Life RepNotify exists"),LifeNotify) || !TestNotNull(TEXT("Dead RepNotify exists"),DeadNotify)) return false;
	// 死亡先到、组件清倒地后到，以及重复到达都只能恢复同一个持久末帧。
	Target->ProcessEvent(DeadNotify,nullptr);
	Target->GetCoopComponent()->ProcessEvent(LifeNotify,nullptr);
	Target->GetCoopComponent()->ProcessEvent(LifeNotify,nullptr);
	Fixture.Advance(.1f);
	Restored=Animation->GetActiveInstanceForMontage(Death);
	TestTrue(TEXT("Reverse/repeated RepNotify order cannot stop or restart the inherited death pose"),Restored && Restored->GetInstanceID()==RestoredId);
	auto* Direct=Fixture.Character(FVector(500,0,100),false,Class);
	LethalDamage(Direct);
	Fixture.Advance(.05f);
	TestTrue(TEXT("A death without prior downing still plays the configured montage normally"),!Direct->IsAlive()
		&& Direct->GetMesh()->GetAnimInstance()->GetActiveInstanceForMontage(Death));
	// 保留 Actor 供一帧只读观察，实际调用生产 EndPlay；若 Super 内组件清理又排了回调，
	// 下一帧会错误停止仍在播放的倒地姿态，直接暴露清理顺序，不能靠对象销毁让 WeakLambda 静默失效。
	auto* Leaving=Fixture.Character(FVector(1500,0,100),true,Class);
	LethalDamage(Leaving);
	Fixture.Advance(.05f);
	auto* LeavingAnimation=Leaving->GetMesh()->GetAnimInstance();
	auto* LeavingPose=LeavingAnimation->GetActiveInstanceForMontage(Death);
	if (!TestNotNull(TEXT("Exit cleanup begins with an actually running downed pose"),LeavingPose)) return false;
	const int32 LeavingPoseId=LeavingPose->GetInstanceID();
	Leaving->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
	TestFalse(TEXT("Production EndPlay ended the actual actor and its cooperative component"),Leaving->HasActorBegunPlay()
		|| Leaving->GetCoopComponent()->HasBegunPlay());
	Fixture.Advance(.03f);
	LeavingPose=LeavingAnimation->GetActiveInstanceForMontage(Death);
	TestTrue(TEXT("No pending presentation callback stops or replaces the pose after EndPlay"),LeavingPose
		&& LeavingPose->GetInstanceID()==LeavingPoseId && !LeavingPose->IsStopped());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDownedPresentationServerTest,"Multiplayer.Combat.Coop.DownedPresentationDedicatedServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 专服仍执行真实倒地／死亡规则，但不启动仅供画面使用的倒地蒙太奇。
bool FCoopCombatDownedPresentationServerTest::RunTest(const FString&)
{
	FCombatCoopWorld Fixture(false,NM_DedicatedServer);
	UClass* Class=LoadClass<AmultiplayerCombatCharacter>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Formal presentation BP loads for DS suppression check"),Class)) return false;
	auto* Target=Fixture.Character(FVector(0,0,100),true,Class);
	Fixture.Character(FVector(-150,0,100));
	TestEqual(TEXT("Fixture really uses the dedicated-server path"),Target->GetNetMode(),NM_DedicatedServer);
	const auto* Field=FindFProperty<FObjectPropertyBase>(Target->GetClass(),TEXT("DeathMontage"));
	auto* Death=Field ? Cast<UAnimMontage>(Field->GetObjectPropertyValue_InContainer(Target)) : nullptr;
	if (!TestNotNull(TEXT("DS test does not pass merely because the montage is absent"),Death)) return false;
	LethalDamage(Target);
	Target->RequestLifePresentationRefresh();
	Fixture.Advance(.1f);
	TestTrue(TEXT("DS still commits actual downed rules"),Target->GetCoopComponent()->IsDowned());
	if (auto* Animation=Target->GetMesh()->GetAnimInstance())
		TestNull(TEXT("DS never starts the purely local downed montage"),Animation->GetActiveInstanceForMontage(Death));
	Target->GetCoopComponent()->RestoreDowned(.1f);
	Fixture.Advance(.2f);
	TestFalse(TEXT("DS still resolves actual bleedout death"),Target->IsAlive());
	if (auto* Animation=Target->GetMesh()->GetAnimInstance())
		TestNull(TEXT("DS also suppresses final death presentation"),Animation->GetActiveInstanceForMontage(Death));
	return !HasAnyErrors();
}

#endif
