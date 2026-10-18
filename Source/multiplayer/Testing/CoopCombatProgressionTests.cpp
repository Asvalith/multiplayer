// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"

namespace
{
	// 真实角色、ASC 和永久效果夹具；不启 AI，不改项目素材和玩家实际存档。
	struct FCombatProgressionWorld
	{
		UWorld* World;
		FCombatProgressionWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL()); World->SetBegunPlay(true);
			// 活跃技能边界使用真实可施法地面，不以悬空落点绕过目标资格检查。
			auto* Ground = World->SpawnActor<AActor>();
			auto* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(5000, 5000, 10));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0, 0, -10));
		}
		// 结束真实世界，由生产 EndPlay 释放自己持有的能力和效果句柄。
		~FCombatProgressionWorld()
		{
			World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
		}
		// 原生玩家默认已接纳，不依赖编辑器蓝图或网络假回执。
		AmultiplayerCombatCharacter* Character()
		{
			auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(AmultiplayerCombatCharacter::StaticClass(),
				FTransform(FVector(0, 0, 100)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled; Pawn->FinishSpawning(FTransform(FVector(0, 0, 100)));
			Pawn->GetCharacterMovement()->DisableMovement(); return Pawn;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatProgressionGrowthTest, "Multiplayer.Combat.Progression.GrowthAndRealAbilityLevels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 发奖、去重、升级和学习都落到真实 Spec/属性聚合，不只检查界面快照。
bool FCombatProgressionGrowthTest::RunTest(const FString&)
{
	FCombatProgressionWorld Fixture; auto* Pawn = Fixture.Character(); auto* Progression = Pawn->GetProgressionComponent();
	if (!TestNotNull(TEXT("Character owns progression"), Progression)) return false;
	auto* ASC = Pawn->GetAbilitySystemComponent();
	TestTrue(TEXT("Profile is assigned by the server"), Progression->GetState().ProfileId.IsValid());
	TestEqual(TEXT("Four starter skills plus rescue are initially available"), ASC->GetActivatableAbilities().Num(), 5);
	TestNull(TEXT("Unlearned dash has not been granted a real GAS spec"), ASC->FindAbilitySpecFromClass(UmultiplayerDashAbility::StaticClass()));
	TestNotNull(TEXT("Cooperative rescue is granted separately from progression skills"), ASC->FindAbilitySpecFromClass(UmultiplayerRescueAbility::StaticClass()));
	TestEqual(TEXT("Dash is explicitly unlearned in the progression snapshot"), Progression->GetSkillLevel(EMultiplayerCombatSkill::Dash), 0);
	const auto Profile = Progression->GetState().ProfileId;
	Progression->InitializeLoadout(); Progression->InitializeLoadout();
	TestEqual(TEXT("Repeated initialization does not duplicate starter skills or rescue"), ASC->GetActivatableAbilities().Num(), 5);
	TestEqual(TEXT("Repeated initialization keeps the server profile"), Progression->GetState().ProfileId, Profile);
	const auto Reward = FGuid::NewGuid();
	TestTrue(TEXT("First server reward is accepted"), Progression->AwardExperience(25, Reward));
	TestFalse(TEXT("The same death reward is not paid twice"), Progression->AwardExperience(25, Reward));
	TestEqual(TEXT("Duplicate callback leaves XP unchanged"), Progression->GetState().Experience, 25);
	TestFalse(TEXT("Negative awards are rejected"), Progression->AwardExperience(-1, FGuid::NewGuid()));
	TestFalse(TEXT("A reward needs a stable identifier"), Progression->AwardExperience(25, FGuid()));
	Progression->AwardExperience(75, FGuid::NewGuid());
	TestEqual(TEXT("One hundred XP reaches level two"), Progression->GetState().Level, 2);
	TestEqual(TEXT("Upgrade grants one skill point"), Progression->GetState().SkillPoints, 3);
	TestEqual(TEXT("Growth adds five actual max health"), Pawn->GetMaxHealth(), 105.f);
	TestEqual(TEXT("Growth does not heal the player"), Pawn->GetHealth(), 100.f);
	TestEqual(TEXT("Growth adds two actual attack power"), ASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()), 2.f);
	TestEqual(TEXT("Growth leaves max-health base unchanged"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute()), 100.f);
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	const auto* Attack = ASC->FindAbilitySpecFromClass(Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill::Attack));
	if (!TestNotNull(TEXT("The granted attack remains a real GAS spec"), Attack)) return false;
	TestEqual(TEXT("Learning changes GAS Spec.Level, not just UI data"), Attack->Level, 2);
	TestEqual(TEXT("Learning spends exactly one point"), Progression->GetState().SkillPoints, 2);
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	TestEqual(TEXT("Level-two character cannot learn skill level three"), Progression->GetSkillLevel(EMultiplayerCombatSkill::Attack), 2);
	Progression->AwardExperience(500, FGuid::NewGuid());
	TestEqual(TEXT("Multiple thresholds use each current character level"), Progression->GetState().Level, 4);
	TestEqual(TEXT("Growth replaces its effect rather than stacking old levels"), Pawn->GetMaxHealth(), 115.f);
	TestEqual(TEXT("Accumulated attack growth is exactly six"), ASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()), 6.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatProgressionLoadoutTest, "Multiplayer.Combat.Progression.PassiveAndSlotLoadout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 换装只更换本组件的被动效果；技能槽位交换后仍走原本的施法入口。
bool FCombatProgressionLoadoutTest::RunTest(const FString&)
{
	FCombatProgressionWorld Fixture; auto* Pawn = Fixture.Character(); auto* Progression = Pawn->GetProgressionComponent();
	auto* ASC = Pawn->GetAbilitySystemComponent();
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
	TestEqual(TEXT("Fortitude grants twenty real max health"), Pawn->GetMaxHealth(), 120.f);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
	TestEqual(TEXT("Equipping the same passive cannot stack it"), Pawn->GetMaxHealth(), 120.f);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Power);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 115.f);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Power);
	TestEqual(TEXT("Power replaces Fortitude instead of retaining its health bonus"), Pawn->GetMaxHealth(), 100.f);
	TestEqual(TEXT("Removing a health cap clamps current health without healing"), Pawn->GetHealth(), 100.f);
	TestEqual(TEXT("Power is a real attack modifier"), ASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()), 5.f);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::None);
	TestEqual(TEXT("Unequipping only removes the passive attack bonus"), ASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()), 0.f);
	Progression->RequestEquipSkill(0, EMultiplayerCombatSkill::Heal);
	TestEqual(TEXT("Slot zero now selects heal"), Progression->GetSlotSkill(0), EMultiplayerCombatSkill::Heal);
	TestEqual(TEXT("The previous heal slot now selects attack"), Progression->GetSlotSkill(2), EMultiplayerCombatSkill::Attack);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	TestTrue(TEXT("Activating the reassigned slot submits its real ability"), Progression->ActivateSlot(0));
	TestTrue(TEXT("The reassigned slot actually heals"), Pawn->GetHealth() > 40.f);
	TestFalse(TEXT("Invalid slots do not fall back to casting attack"), Progression->ActivateSlot(99));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatProgressionMutationBoundaryTest, "Multiplayer.Combat.Progression.ActiveSkillAndValidationBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 正在施法和未接纳时都不能换装；拒绝请求不取消旧技能，也不修改等级或扣点。
bool FCombatProgressionMutationBoundaryTest::RunTest(const FString&)
{
	FCombatProgressionWorld Fixture; auto* Pawn = Fixture.Character(); auto* Progression = Pawn->GetProgressionComponent();
	auto* ASC = Pawn->GetAbilitySystemComponent(); Progression->AwardExperience(100, FGuid::NewGuid());
	TestTrue(TEXT("Area skill starts through the character aim path"), Progression->ActivateSlot(3));
	auto* Area = ASC->FindAbilitySpecFromClass(Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill::Area));
	if (!TestTrue(TEXT("Boundary fixture owns an actually active ability"), Area && Area->IsActive())) return false;
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	Progression->RequestEquipSkill(0, EMultiplayerCombatSkill::Heal);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Power);
	TestEqual(TEXT("Casting blocks learning without spending points"), Progression->GetState().SkillPoints, 3);
	TestEqual(TEXT("Casting blocks slot mutation"), Progression->GetSlotSkill(0), EMultiplayerCombatSkill::Attack);
	TestFalse(TEXT("Restore cannot replace an executing loadout"), Progression->RestoreState(Progression->CaptureState()));
	TestTrue(TEXT("Rejected mutation leaves the old ability executing"), Area->IsActive());
	ASC->CancelAllAbilities(); Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	TestEqual(TEXT("Learning is available after normal cancellation"), Progression->GetSkillLevel(EMultiplayerCombatSkill::Attack), 2);
	Pawn->SetCombatSessionReady(false); Progression->RequestEquipSkill(0, EMultiplayerCombatSkill::Heal);
	TestEqual(TEXT("Unaccepted player cannot change slots"), Progression->GetSlotSkill(0), EMultiplayerCombatSkill::Attack);
	const auto Good = Progression->CaptureState(); auto Bad = Good;
	Bad.Version = 2; TestFalse(TEXT("Unknown save versions are rejected"), Progression->RestoreState(Bad));
	Bad = Good; Bad.SkillPoints = 999; TestFalse(TEXT("Corrupt point accounting is rejected"), Progression->RestoreState(Bad));
	Bad = Good; Bad.Slots[0] = Bad.Slots[1]; TestFalse(TEXT("Duplicated slots are rejected"), Progression->RestoreState(Bad));
	Bad = Good; Bad.ActiveLevels.Reset(); TestFalse(TEXT("Incomplete ability arrays are rejected"), Progression->RestoreState(Bad));
	Bad = Good; Bad.Experience = -1; TestFalse(TEXT("Negative saved XP is rejected"), Progression->RestoreState(Bad));
	Bad = Good; Bad.EquippedPassive = EMultiplayerCombatPassive::Power;
	TestFalse(TEXT("Unlearned equipped passives are rejected"), Progression->RestoreState(Bad));
	TestTrue(TEXT("Trusted valid resume state can restore before admission"), Progression->RestoreState(Good));
	TestFalse(TEXT("Restoring progression does not admit the combat session"), Pawn->IsCombatReady());
	for (FName Name : { FName("ServerLearnSkill"), FName("ServerEquipSkill"), FName("ServerLearnPassive"), FName("ServerEquipPassive") })
	{
		const auto* Function = Progression->FindFunction(Name);
		TestTrue(TEXT("Loadout requests use reliable server RPCs"), Function && Function->HasAllFunctionFlags(FUNC_NetServer | FUNC_NetReliable));
	}
	// 检查生产注册的复制条件，而不把仅有 UPROPERTY(Net) 误当作已经限制到拥有者。
	// 独立夹具未创建网络 RepLayout；先完成引擎正常会做的 RepIndex 初始化，再只收集本组件的注册。
	Progression->GetClass()->SetUpRuntimeReplicationData();
	TArray<FLifetimeProperty> ProgressionReplication;
	static_cast<UObject*>(Progression)->GetLifetimeReplicatedProps(ProgressionReplication);
	const auto* StateProperty = FindFProperty<FProperty>(Progression->GetClass(), TEXT("State"));
	const auto* StateReplication = StateProperty ? ProgressionReplication.FindByPredicate([StateProperty](const FLifetimeProperty& Entry)
		{ return Entry.RepIndex == StateProperty->RepIndex; }) : nullptr;
	TestTrue(TEXT("Progression snapshot is owner-only with rep-notify"), StateReplication
		&& StateReplication->Condition == COND_OwnerOnly && StateReplication->RepNotifyCondition == REPNOTIFY_Always);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatProgressionPersistenceTest, "Multiplayer.Combat.Progression.VersionedPersistenceAndRewardWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 使用正式 SaveGame 序列化到内存验证长期格式，避免覆盖用户固定单机槽；恢复仍落到真实 GAS。
bool FCombatProgressionPersistenceTest::RunTest(const FString&)
{
	FCombatProgressionWorld Fixture; auto* Pawn = Fixture.Character(); auto* Progression = Pawn->GetProgressionComponent();
	Progression->AwardExperience(300, FGuid::NewGuid());
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
	auto* Save = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::CreateSaveGameObject(UmultiplayerCombatProgressionSaveGame::StaticClass()));
	Save->Progression = Progression->CaptureState(); TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Versioned progression serializes with SaveGame"), UGameplayStatics::SaveGameToMemory(Save, Bytes))) return false;
	const auto* Loaded = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::LoadGameFromMemory(Bytes));
	if (!TestNotNull(TEXT("Saved progression retains its expected class"), Loaded)) return false;
	auto* NewPawn = Fixture.Character(); auto* NewProgression = NewPawn->GetProgressionComponent(); NewPawn->SetCombatSessionReady(false);
	TestTrue(TEXT("Serialized growth restores on a fresh server pawn"), NewProgression->RestoreState(Loaded->Progression));
	TestEqual(TEXT("Server profile identity survives a pawn replacement"), NewProgression->GetState().ProfileId, Save->Progression.ProfileId);
	TestEqual(TEXT("Restored level three and Fortitude apply exactly once"), NewPawn->GetMaxHealth(), 130.f);
	TestTrue(TEXT("Repeating a restore does not stack permanent effects"), NewProgression->RestoreState(Loaded->Progression));
	TestEqual(TEXT("Repeated restore preserves the same aggregate cap"), NewPawn->GetMaxHealth(), 130.f);
	TestNull(TEXT("Long-term format has no health resource"), FindFProperty<FProperty>(FmultiplayerCombatProgressionState::StaticStruct(), TEXT("Health")));
	TestNull(TEXT("Long-term format has no movement location"), FindFProperty<FProperty>(FmultiplayerCombatProgressionState::StaticStruct(), TEXT("Location")));
	FGuid LastReward;
	for (int32 Index = 0; Index < 70; ++Index) { LastReward = FGuid::NewGuid(); NewProgression->AwardExperience(1, LastReward); }
	TestEqual(TEXT("Reward idempotency window remains bounded"), NewProgression->GetState().ClaimedRewards.Num(), 64);
	TestFalse(TEXT("Newest claimed reward is still rejected after trimming"), NewProgression->AwardExperience(1, LastReward));
	TestTrue(TEXT("Large server award cannot overflow XP arithmetic"), NewProgression->AwardExperience(MAX_int32, FGuid::NewGuid()));
	TestEqual(TEXT("Character levels stop at the configured cap"), NewProgression->GetState().Level, 100);
	TestEqual(TEXT("Capped XP does not wrap or retain unusable overflow"), NewProgression->GetState().Experience, 0);
	TestEqual(TEXT("Capped level plus the equipped passive has one coherent bonus"), NewPawn->GetMaxHealth(), 615.f);
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatProgressionDiskPersistenceTest, "Multiplayer.Combat.Progression.UniqueProfileDiskPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 正式读写服务器签发 GUID 对应的槽；先确认不存在，再仅清理测试本次创建的确切文件。
bool FCombatProgressionDiskPersistenceTest::RunTest(const FString&)
{
	FCombatProgressionWorld Fixture;
	// 不监听、不连接、不推进未初始化驱动的 Tick；这里只采用正式服务器存档命名策略。
	UClass* DriverClass = LoadClass<UNetDriver>(nullptr, TEXT("/Script/OnlineSubsystemUtils.IpNetDriver"));
	if (!TestNotNull(TEXT("Concrete IP driver can be loaded without a new module dependency"), DriverClass)) return false;
	auto* Driver = NewObject<UNetDriver>(GetTransientPackage(), DriverClass);
	Fixture.World->SetNetDriver(Driver);
	Driver->SetWorld(Fixture.World);
	ON_SCOPE_EXIT
	{
		Driver->SetWorld(nullptr);
		Fixture.World->SetNetDriver(nullptr);
	};
	if (!TestTrue(TEXT("Disk fixture uses server profile slots, never the standalone local slot"),
		Fixture.World->GetNetMode() == NM_ListenServer || Fixture.World->GetNetMode() == NM_DedicatedServer)
		|| !TestNull(TEXT("Disk fixture has no remote connection"), Driver->ServerConnection.Get())) return false;
	auto* Pawn = Fixture.Character();
	auto* Progression = Pawn->GetProgressionComponent();
	auto* ASC = Pawn->GetAbilitySystemComponent();
	const FGuid ProfileId = Progression->GetState().ProfileId;
	if (!TestTrue(TEXT("Server issued a fresh valid profile ID"), ProfileId.IsValid())) return false;
	const FString Slot = FString::Printf(TEXT("CombatProgress_%s"), *ProfileId.ToString(EGuidFormats::Digits));
	if (!TestFalse(TEXT("Unique GUID slot must not already exist before the test owns it"), UGameplayStatics::DoesSaveGameExist(Slot, 0))) return false;
	ON_SCOPE_EXIT
	{
		// 此槽通过“不存在”检查且来自本次服务器 ProfileId；绝不枚举或删除其他存档。
		if (UGameplayStatics::DoesSaveGameExist(Slot, 0))
			TestTrue(TEXT("Failure cleanup removes only this test's GUID slot"), UGameplayStatics::DeleteGameInSlot(Slot, 0));
	};
	Progression->AwardExperience(300, FGuid::NewGuid());
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Power);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
	Progression->RequestEquipSkill(0, EMultiplayerCombatSkill::Heal);
	const FmultiplayerCombatProgressionState Saved = Progression->CaptureState();
	const int32 AbilityCount = ASC->GetActivatableAbilities().Num();
	if (!TestTrue(TEXT("Formal SaveProgress writes the unique profile file"), Progression->SaveProgress())
		|| !TestTrue(TEXT("Formal save creates the exact server GUID slot"), UGameplayStatics::DoesSaveGameExist(Slot, 0))) return false;
	const auto* Disk = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
	if (!TestNotNull(TEXT("Actual disk file has the production SaveGame class"), Disk)) return false;
	TestTrue(TEXT("Actual disk contents equal the complete saved snapshot"),
		FmultiplayerCombatProgressionState::StaticStruct()->CompareScriptStruct(&Saved, &Disk->Progression, 0));
	Progression->AwardExperience(300, FGuid::NewGuid());
	Progression->RequestLearnSkill(EMultiplayerCombatSkill::Attack);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Power);
	Progression->RequestEquipSkill(0, EMultiplayerCombatSkill::Attack);
	TestTrue(TEXT("Growth, passive and loadout really change after saving"), Progression->GetState().Level != Saved.Level
		&& Progression->GetState().EquippedPassive != Saved.EquippedPassive && Progression->GetSlotSkill(0) != Saved.Slots[0]);
	Pawn->Defend();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 41.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 29.f);
	const float ExistingCooldown = Pawn->GetDefenseCooldownRemaining();
	if (!TestTrue(TEXT("Resource fixture has a real running cooldown"), ExistingCooldown > 0.f)) return false;
	FGameplayEffectQuery PermanentEffects;
	PermanentEffects.EffectDefinition = UmultiplayerCombatProgressionEffect::StaticClass();
	const auto CheckResourcesAndGrants = [&]
	{
		TestEqual(TEXT("Disk load cannot refund health"), Pawn->GetHealth(), 41.f);
		TestEqual(TEXT("Disk load cannot refund energy"), Pawn->GetEnergy(), 29.f);
		TestTrue(TEXT("Disk load cannot clear or restart cooldown"), FMath::IsNearlyEqual(Pawn->GetDefenseCooldownRemaining(), ExistingCooldown, .001f));
		TestEqual(TEXT("Disk load never duplicates granted abilities"), ASC->GetActivatableAbilities().Num(), AbilityCount);
		TestEqual(TEXT("Growth and equipped passive retain exactly two real effects"), ASC->GetActiveEffects(PermanentEffects).Num(), 2);
	};
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		if (!TestTrue(TEXT("Formal LoadProgress reads the actual GUID file"), Progression->LoadProgress())) return false;
		TestTrue(TEXT("Disk loading restores every saved progression field"),
			FmultiplayerCombatProgressionState::StaticStruct()->CompareScriptStruct(&Saved, &Progression->GetState(), 0));
		TestEqual(TEXT("Restored growth and Fortitude apply exactly once"), Pawn->GetMaxHealth(), 130.f);
		const auto* Attack = ASC->FindAbilitySpecFromClass(Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill::Attack));
		TestTrue(TEXT("Restored GAS attack spec uses its saved level"), Attack && Attack->Level == Saved.ActiveLevels[0]);
		CheckResourcesAndGrants();
	}

	// 三种坏档都实际写入同一个测试槽，再从正式 LoadProgress 验证拒绝；不重复内存序列化测试。
	auto* Invalid = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::CreateSaveGameObject(UmultiplayerCombatProgressionSaveGame::StaticClass()));
	for (int32 Corruption = 0; Corruption < 3; ++Corruption)
	{
		Invalid->Progression = Saved;
		if (Corruption == 0) Invalid->Progression.SkillPoints = 999;
		else if (Corruption == 1) Invalid->Progression.ProfileId = FGuid::NewGuid();
		else Invalid->Progression.Version = 2;
		if (!TestTrue(TEXT("Invalid data is written only to this test's owned GUID slot"), UGameplayStatics::SaveGameToSlot(Invalid, Slot, 0))) return false;
		TestFalse(*FString::Printf(TEXT("Disk load rejects %s"), Corruption == 0 ? TEXT("corrupt point accounting")
			: Corruption == 1 ? TEXT("another server profile") : TEXT("unsupported version two")), Progression->LoadProgress());
		TestTrue(TEXT("Rejected disk data cannot partially overwrite any current state"),
			FmultiplayerCombatProgressionState::StaticStruct()->CompareScriptStruct(&Saved, &Progression->GetState(), 0));
		CheckResourcesAndGrants();
	}
	TestTrue(TEXT("Success cleanup deletes only this test's GUID slot"), UGameplayStatics::DeleteGameInSlot(Slot, 0));
	TestFalse(TEXT("Unique test file is gone after cleanup"), UGameplayStatics::DoesSaveGameExist(Slot, 0));
	return !HasAnyErrors();
}

#endif
