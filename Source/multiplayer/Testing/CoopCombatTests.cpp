// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Tests/AutomationCommon.h"
#include <limits>
#include "UObject/UnrealType.h"
#if WITH_EDITORONLY_DATA
#include "EdGraph/EdGraph.h"
#endif

namespace
{
	// 测试世界独立于编辑中的关卡；结束后释放 Context、Actor、定时器和物理场景。
	// 独立世界显式创建 AISystem，确保实际执行树；本组不创建导航、不伪造追击结果。
	struct FCombatTestWorld
	{
		UWorld* World = nullptr;
		uint64 PreviousFrame = GFrameCounter;
		TArray<double> ShotTimes;
		FDelegateHandle SpawnedHandle;
		FCombatTestWorld()
		{
			// 原生敌人现在运行真实行为树，独立测试世界也需提供 BTManager 所属的 AISystem。
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(true);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			SpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (Actor->IsA<AmultiplayerCombatProjectile>()) ShotTimes.Add(World->GetTimeSeconds());
			}));
		}
		~FCombatTestWorld()
		{
			World->RemoveOnActorSpawnedHandler(SpawnedHandle);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		// 固定步长推进真实投射物和 GAS 到期回收，不靠等待墙钟时间判断结果。
		// 只推进真实世界、BT、GAS 与投射物，不直接调用 Service 或攻击任务。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0; Elapsed < Seconds; Elapsed += 0.05f)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, 0.05f);
			}
		}

		APlayerController* Control(AmultiplayerCombatCharacter* Character)
		{
			// 阵营筛选用例会将同阵营敌人交给玩家控制，先正常解除它原来的 AI。
			if (AAIController* OldAI = Cast<AAIController>(Character->GetController()))
			{
				OldAI->UnPossess();
				OldAI->Destroy();
			}
			APlayerController* PC = World->SpawnActor<APlayerController>();
			PC->SetPlayerState(World->SpawnActor<APlayerState>());
			PC->Possess(Character);
			Character->GetCharacterMovement()->DisableMovement();
			return PC;
		}

		AmultiplayerCombatCharacter* SpawnPlayer(const FVector& Location, bool bControlled = true)
		{
			FActorSpawnParameters Parameters;
			Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AmultiplayerCombatCharacter* Player = World->SpawnActor<AmultiplayerCombatCharacter>(Location,
				FRotator::ZeroRotator, Parameters);
			Player->GetCharacterMovement()->DisableMovement();
			if (bControlled) Control(Player);
			return Player;
		}

		AmultiplayerCombatEnemy* SpawnEnemy(const FVector& Location = FVector(0, 0, 100))
		{
			FActorSpawnParameters Parameters;
			Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AmultiplayerCombatEnemy* Enemy = World->SpawnActor<AmultiplayerCombatEnemy>(Location,
				FRotator::ZeroRotator, Parameters);
			Enemy->GetCharacterMovement()->DisableMovement();
			return Enemy;
		}
	};

	// 伤害走真实 GameplayEffect；没有直接改 Health 来假装验证伤害系统。
	// 死亡也走真实伤害效果，不直接修改死亡布尔值或黑板。
	void ApplyTestDamage(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target, float Damage)
	{
		UAbilitySystemComponent* ASC = Source->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Damage);
		ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
	}

	// 检查保存的运行时节点，而不是只凭资产名称判断已经迁移到行为树。
	bool ContainsCombatAINode(const UBTCompositeNode* Composite, const UClass* Class)
	{
		if (Composite == nullptr) return false;
		for (const UBTService* Service : Composite->Services)
			if (Service != nullptr && Service->IsA(Class)) return true;
		for (const FBTCompositeChild& Child : Composite->Children)
		{
			if (Child.ChildTask != nullptr && Child.ChildTask->IsA(Class)) return true;
			if (ContainsCombatAINode(Child.ChildComposite, Class)) return true;
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRulesTest, "Multiplayer.Combat.CoreRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 验证技能条件、伤害、治疗、死亡与机关占用；此用例不是双客户端网络验收。
bool FCoopCombatRulesTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	UWorld* World = Fixture.World;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerCombatCharacter* Player = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	AmultiplayerCombatCharacter* Friend = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(300, 0, 100), FRotator::ZeroRotator, Spawn);
	AmultiplayerCombatEnemy* Enemy = World->SpawnActor<AmultiplayerCombatEnemy>(FVector(500, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("Player created"), Player) || !TestNotNull(TEXT("Friend created"), Friend) || !TestNotNull(TEXT("Enemy created"), Enemy)) return false;
	for (AmultiplayerCombatCharacter* Character : { Player, Friend, static_cast<AmultiplayerCombatCharacter*>(Enemy) })
		Character->GetCharacterMovement()->DisableMovement();
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	TestEqual(TEXT("Initial health"), Player->GetHealth(), 100.0f);
	TestEqual(TEXT("Player receives four skills"), ASC->GetActivatableAbilities().Num(), 4);
	TestEqual(TEXT("Enemy receives only attack"), Enemy->GetAbilitySystemComponent()->GetActivatableAbilities().Num(), 1);
	TestTrue(TEXT("Defense uses local prediction"), GetDefault<UmultiplayerDefendAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestTrue(TEXT("Attack predicts cooldown, not authoritative projectile damage"), GetDefault<UmultiplayerAttackAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestTrue(TEXT("Healing predicts cost and cooldown, not target health"), GetDefault<UmultiplayerHealAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestFalse(TEXT("Friends are not hostile"), Player->IsHostileTo(Friend));
	TestTrue(TEXT("Enemies are hostile"), Player->IsHostileTo(Enemy));

	// 没有受伤目标时，不消耗能量或启动冷却。
	Player->Heal();
	TestEqual(TEXT("Empty heal costs nothing"), Player->GetEnergy(), 100.0f);
	TestFalse(TEXT("Empty heal has no cooldown"), ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Heal));
	ApplyTestDamage(Enemy, Friend, 60);
	Player->Heal();
	TestEqual(TEXT("Heal selects injured teammate"), Friend->GetHealth(), 70.0f);
	TestEqual(TEXT("Heal energy cost"), Player->GetEnergy(), 75.0f);
	Player->Heal();
	TestEqual(TEXT("Repeated heal rejected by cooldown"), Friend->GetHealth(), 70.0f);
	TestEqual(TEXT("Rejected heal not charged twice"), Player->GetEnergy(), 75.0f);

	// 防御不叠加；到期后恢复正常伤害，没有手动恢复属性的 Timer。
	Player->Defend();
	TestTrue(TEXT("Defense grants state"), ASC->HasMatchingGameplayTag(CombatTags::State_Defending));
	TestTrue(TEXT("Defense query exposes active effect"), Player->IsDefending() && Player->GetDefenseRemaining() > 2.9f);
	TestTrue(TEXT("Defense cooldown query exposes active effect"), Player->GetDefenseCooldownRemaining() > 7.9f);
	TestEqual(TEXT("Defense cost"), Player->GetEnergy(), 55.0f);
	Player->Defend();
	TestEqual(TEXT("Defense repeat costs nothing"), Player->GetEnergy(), 55.0f);
	ApplyTestDamage(Enemy, Player, 20);
	TestEqual(TEXT("Defense halves damage"), Player->GetHealth(), 90.0f);
	Fixture.Advance(3.2f);
	TestFalse(TEXT("Defense expires"), ASC->HasMatchingGameplayTag(CombatTags::State_Defending));
	TestFalse(TEXT("Expired defense query is clear"), Player->IsDefending());
	TestEqual(TEXT("Expired defense has zero remaining time"), Player->GetDefenseRemaining(), 0.0f);
	ApplyTestDamage(Enemy, Player, 20);
	TestEqual(TEXT("Damage returns to normal"), Player->GetHealth(), 70.0f);

	// 真实扫掠弹道穿过友军，命中敌人只扣一次；不能用直接效果替代这条验证。
	Player->Attack();
	int32 ProjectileCount = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) ++ProjectileCount;
	TestEqual(TEXT("Attack creates one projectile"), ProjectileCount, 1);
	Player->Attack();
	ProjectileCount = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) ++ProjectileCount;
	TestEqual(TEXT("Attack cooldown blocks duplicate"), ProjectileCount, 1);
	Fixture.Advance(0.5f);
	TestEqual(TEXT("Projectile damages enemy once"), Enemy->GetHealth(), 75.0f);
	TestEqual(TEXT("Projectile does not damage friend"), Friend->GetHealth(), 70.0f);

	// 空能量不能施放防御；普通攻击不耗能，仍由自己的冷却约束。
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 0);
	Fixture.Advance(5.0f);
	Player->Defend();
	TestFalse(TEXT("Insufficient energy rejects defense"), ASC->HasMatchingGameplayTag(CombatTags::State_Defending));
	TestEqual(TEXT("Energy never negative"), Player->GetEnergy(), 0.0f);

	// 真实区域重叠与 Possess 先产生玩家人数；死亡关闭所有碰撞，立即撤销占用。
	APlayerController* PC = World->SpawnActor<APlayerController>(Spawn);
	// 独立测试世界没有 GameMode 自动创建 PlayerState，补齐真实玩家资格再占有角色。
	PC->SetPlayerState(World->SpawnActor<APlayerState>(Spawn));
	PC->Possess(Friend);
	Friend->GetCharacterMovement()->DisableMovement();
	TestTrue(TEXT("Fixture pawn is player controlled"), Friend->IsPlayerControlled());
	AActor* TriggerActor = World->SpawnActor<AActor>(Spawn);
	UBoxComponent* Box = NewObject<UBoxComponent>(TriggerActor);
	TriggerActor->SetRootComponent(Box);
	Box->SetBoxExtent(FVector(100));
	Box->SetGenerateOverlapEvents(true);
	Box->SetCollisionObjectType(ECC_WorldDynamic);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Box->SetCollisionResponseToAllChannels(ECR_Ignore);
	Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Box->RegisterComponent();
	TriggerActor->SetActorLocation(Friend->GetActorLocation());
	UmultiplayerPlayerOccupancyComponent* Occupancy = NewObject<UmultiplayerPlayerOccupancyComponent>(TriggerActor);
	Occupancy->RegisterComponent();
	Fixture.Advance(0.1f);
	Friend->GetCapsuleComponent()->UpdateOverlaps();
	Box->UpdateOverlaps();
	Occupancy->BindTrigger(Box);
	TestTrue(TEXT("Fixture box contains pawn overlap"), Box->IsOverlappingActor(Friend));
	TestEqual(TEXT("Living player occupies trigger"), Occupancy->GetPlayerCount(), 1);
	ApplyTestDamage(Enemy, Friend, 1000);
	TestFalse(TEXT("Lethal damage marks dead"), Friend->IsAlive());
	TestEqual(TEXT("Dead player no longer occupies trigger"), Occupancy->GetPlayerCount(), 0);
	TestEqual(TEXT("Lethal damage clamps health"), Friend->GetHealth(), 0.0f);
	TestTrue(TEXT("Death tag committed"), Friend->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dead));
	TestFalse(TEXT("Dead character cannot display defense"), Friend->IsDefending());
	TestEqual(TEXT("Dead character displays no defense cooldown"), Friend->GetDefenseCooldownRemaining(), 0.0f);
	Friend->Heal();
	Friend->Attack();
	TestEqual(TEXT("Dead actor cannot heal itself"), Friend->GetHealth(), 0.0f);
	FGameplayEffectSpecHandle DeadHeal = ASC->MakeOutgoingSpec(UmultiplayerHealEffect::StaticClass(), 1, ASC->MakeEffectContext());
	DeadHeal.Data->SetSetByCallerMagnitude(CombatTags::Data_Heal, 30);
	Friend->GetAbilitySystemComponent()->ApplyGameplayEffectSpecToSelf(*DeadHeal.Data.Get());
	TestEqual(TEXT("Raw healing effect cannot resurrect"), Friend->GetHealth(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionAndResumeTest, "Multiplayer.Combat.AdmissionAndResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 服务器快照与接纳边界；网络票据及自动重连另由真实 DS 双客户端验证。
bool FCoopCombatAdmissionAndResumeTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer(FVector(0, 0, 100), false);
	AmultiplayerCombatCharacter* Friend = Fixture.SpawnPlayer(FVector(300, 0, 100), false);
	AmultiplayerCombatEnemy* Enemy = Fixture.SpawnEnemy(FVector(2000, 0, 100));
	Player->SetCombatSessionReady(false);
	Player->Attack();
	Player->Defend();
	ApplyTestDamage(Enemy, Player, 25.0f);
	TestEqual(TEXT("Unadmitted pawn cannot create authoritative projectile"), Fixture.ShotTimes.Num(), 0);
	TestEqual(TEXT("Unadmitted pawn cannot spend resources"), Player->GetEnergy(), 100.0f);
	TestEqual(TEXT("Unadmitted pawn cannot take pre-restore damage"), Player->GetHealth(), 100.0f);
	TestFalse(TEXT("Unadmitted pawn cannot occupy mechanisms"), Player->GetActorEnableCollision());
	Player->SetCombatSessionReady(true);
	Player->Defend();
	ApplyTestDamage(Enemy, Player, 20.0f);
	ApplyTestDamage(Enemy, Friend, 60.0f);
	Player->Heal();
	Player->Attack();
	const FmultiplayerCombatResumeState Saved = Player->CaptureResumeState();
	TestEqual(TEXT("Snapshot captures actual health"), Saved.Health, 90.0f);
	TestEqual(TEXT("Snapshot captures actual energy after both costs"), Saved.Energy, 55.0f);
	AmultiplayerCombatCharacter* Resumed = Fixture.SpawnPlayer(FVector(3000, 3000, 100), false);
	Resumed->SetCombatSessionReady(false);
	Resumed->RestoreResumeState(Saved, 1.0f);
	Resumed->SetCombatSessionReady(true);
	TestEqual(TEXT("Reconnect does not refill health"), Resumed->GetHealth(), 90.0f);
	TestEqual(TEXT("Reconnect does not refill energy"), Resumed->GetEnergy(), 55.0f);
	TestTrue(TEXT("Restore uses the saved server position"), Resumed->GetActorLocation().Equals(Saved.Location, 1.0f));
	TestTrue(TEXT("Defense lifetime decreases during offline time"),
		FMath::IsNearlyEqual(Resumed->GetDefenseRemaining(), 2.0f, 0.1f));
	TestTrue(TEXT("Defense cooldown decreases during offline time"),
		FMath::IsNearlyEqual(Resumed->GetDefenseCooldownRemaining(), 7.0f, 0.1f));
	TestFalse(TEXT("Expired attack cooldown is not recreated"),
		Resumed->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Attack));
	TestTrue(TEXT("Longer heal cooldown is preserved"),
		Resumed->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Heal));
	Resumed->Defend();
	TestEqual(TEXT("Restored cooldown rejects immediate resource bypass"), Resumed->GetEnergy(), 55.0f);
	ApplyTestDamage(Enemy, Player, 1000.0f);
	const FmultiplayerCombatResumeState Dead = Player->CaptureResumeState();
	AmultiplayerCombatCharacter* DeadResume = Fixture.SpawnPlayer(FVector(4000, 3000, 100), false);
	DeadResume->SetCombatSessionReady(false);
	DeadResume->RestoreResumeState(Dead, 5.0f);
	DeadResume->SetCombatSessionReady(true);
	TestFalse(TEXT("Reconnect cannot revive a dead pawn"), DeadResume->IsAlive());
	TestFalse(TEXT("Dead resumed pawn remains outside mechanisms"), DeadResume->GetActorEnableCollision());
	DeadResume->Attack();
	TestEqual(TEXT("Dead reconnect cannot generate another projectile"), Fixture.ShotTimes.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHealingAuthorityTest, "Multiplayer.Combat.HealingAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 同一服务器目标规则同时限制距离、遮挡、阵营与接纳状态，不接客户端目标或回血参数。
bool FCoopCombatHealingAuthorityTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer(FVector(0, 0, 100), false);
	AmultiplayerCombatCharacter* Friend = Fixture.SpawnPlayer(FVector(800, 0, 100), false);
	AmultiplayerCombatEnemy* Enemy = Fixture.SpawnEnemy(FVector(2000, 0, 100));
	ApplyTestDamage(Enemy, Friend, 60.0f);
	ApplyTestDamage(Player, Enemy, 50.0f);
	Player->Heal();
	TestEqual(TEXT("Distant friend and hostile wounded enemy cannot consume heal cost"), Player->GetEnergy(), 100.0f);
	Friend->SetActorLocation(FVector(300, 0, 100));
	AActor* Wall = Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
	Wall->SetRootComponent(Box);
	Box->SetBoxExtent(FVector(20, 100, 200));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	Wall->SetActorLocation(FVector(150, 0, 150));
	Player->Heal();
	TestEqual(TEXT("Blocked friend is not healed through a wall"), Friend->GetHealth(), 40.0f);
	TestEqual(TEXT("Blocked heal costs nothing"), Player->GetEnergy(), 100.0f);
	Wall->Destroy();
	Friend->SetCombatSessionReady(false);
	Player->Heal();
	TestEqual(TEXT("Handshake-pending friend cannot be targeted"), Player->GetEnergy(), 100.0f);
	Friend->SetCombatSessionReady(true);
	Player->Heal();
	TestEqual(TEXT("Valid retry heals the actual nearby friendly target"), Friend->GetHealth(), 70.0f);
	TestEqual(TEXT("Valid retry consumes exactly one cost"), Player->GetEnergy(), 75.0f);
	TestFalse(TEXT("Nonfinite projectile damage rejected at server entry"), Player->FireProjectile(std::numeric_limits<float>::quiet_NaN(), 1600.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileInterpolationTest, "Multiplayer.Combat.ProjectileInterpolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 保留原测试名，驱动真实 RepNotify 和时钟 Tick，验证初始状态重建而非持续位置复制。
bool FCoopCombatProjectileInterpolationTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	const FVector Start(0, 1000, 100);
	const FTransform Transform(FRotator(0, 30, 0), Start);
	AmultiplayerCombatCharacter* Source = Fixture.SpawnPlayer(FVector(0, 0, 100), false);
	AmultiplayerCombatProjectile* Server = Fixture.World->SpawnActorDeferred<AmultiplayerCombatProjectile>(
		AmultiplayerCombatProjectile::StaticClass(), Transform, Source, Source, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Authoritative projectile created"), Server)) return false;
	Server->Initialize(Source, 25, 1200);
	Server->FinishSpawning(Transform);
	const FMultiplayerCombatProjectileLaunchState Launch = Server->GetLaunchState();
	TestTrue(TEXT("Server initializes an immutable launch state"), Launch.bInitialized);
	TestTrue(TEXT("Launch records the actual origin"), FVector(Launch.Origin).Equals(Start, 0.01f));
	TestTrue(TEXT("Launch records non-default world velocity"), FVector(Launch.Velocity).Equals(Transform.GetRotation().GetForwardVector() * 1200, 0.01f));
	TestFalse(TEXT("Projectile does not continuously replicate movement"), Server->IsReplicatingMovement());
	UProjectileMovementComponent* ServerMovement = Server->FindComponentByClass<UProjectileMovementComponent>();
	if (!TestNotNull(TEXT("Server projectile movement exists"), ServerMovement)) return false;
	TestTrue(TEXT("Server keeps authoritative swept movement"), ServerMovement->bSimulationEnabled && ServerMovement->bSweepCollision && ServerMovement->IsComponentTickEnabled());
	TestTrue(TEXT("Launch contract is straight without gravity bounce or homing"), ServerMovement->ProjectileGravityScale == 0
		&& !ServerMovement->bShouldBounce && !ServerMovement->bIsHomingProjectile);
	Fixture.Advance(0.25f);
	TestTrue(TEXT("Real server component advances the projectile"), FVector::Dist(Server->GetActorLocation(), Start) > 200);

	// 模拟晚相关首包；根的 Spawn 位置不是起点，但显示必须从原始发射状态和时钟重建。
	const FTransform LateTransform(Transform.GetRotation(), Server->GetActorLocation());
	AmultiplayerCombatProjectile* Projectile = Fixture.World->SpawnActorDeferred<AmultiplayerCombatProjectile>(
		AmultiplayerCombatProjectile::StaticClass(), LateTransform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Simulated projectile created"), Projectile)) return false;
	Projectile->SetRole(ROLE_SimulatedProxy);
	Projectile->FinishSpawning(LateTransform);
	UProjectileMovementComponent* Movement = Projectile->FindComponentByClass<UProjectileMovementComponent>();
	UStaticMeshComponent* Mesh = Projectile->FindComponentByClass<UStaticMeshComponent>();
	USphereComponent* Collision = Projectile->FindComponentByClass<USphereComponent>();
	if (!TestNotNull(TEXT("Projectile movement exists"), Movement)
		|| !TestNotNull(TEXT("Projectile mesh exists"), Mesh)
		|| !TestNotNull(TEXT("Projectile collision exists"), Collision)) return false;
	TestFalse(TEXT("Client has no continuous movement replication"), Projectile->IsReplicatingMovement());
	TestFalse(TEXT("Client does not run projectile physics simulation"), Movement->bSimulationEnabled);
	TestFalse(TEXT("Client movement component does not tick a second trajectory"), Movement->IsComponentTickEnabled());
	TestTrue(TEXT("Client does not collide"), Collision->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestFalse(TEXT("Visual stays hidden before the launch state arrives"), Mesh->IsVisible());
	FStructProperty* LaunchProperty = FindFProperty<FStructProperty>(Projectile->GetClass(), TEXT("LaunchState"));
	if (!TestNotNull(TEXT("Initial launch state is reflected for replication"), LaunchProperty)) return false;
	*LaunchProperty->ContainerPtrToValuePtr<FMultiplayerCombatProjectileLaunchState>(Projectile) = Launch;
	Projectile->ProcessEvent(Projectile->FindFunctionChecked(TEXT("OnRep_LaunchState")), nullptr);
	TestFalse(TEXT("Launch waits hidden rather than using an unrelated local clock"), Mesh->IsVisible());
	TestFalse(TEXT("Missing GameState does not start a visual tick"), Projectile->IsActorTickEnabled());

	AGameStateBase* Clock = Fixture.World->SpawnActor<AGameStateBase>();
	if (!TestNotNull(TEXT("Real GameState clock created"), Clock)) return false;
	Fixture.World->SetGameState(Clock);
	FFloatProperty* ClockDelta = FindFProperty<FFloatProperty>(Clock->GetClass(), TEXT("ServerWorldTimeSecondsDelta"));
	if (!TestNotNull(TEXT("Server clock delta exists"), ClockDelta)) return false;
	ClockDelta->SetPropertyValue_InContainer(Clock, -0.2f);
	Fixture.Advance(0.1f);
	const auto ExpectedPosition = [&]()
	{
		const double Age = FMath::Clamp(Clock->GetServerWorldTimeSeconds() - Launch.ServerLaunchTime, 0.0, 3.0);
		return Start + FVector(Launch.Velocity) * Age;
	};
	TestTrue(TEXT("Late GameState retries and starts the visual trajectory"), Projectile->IsActorTickEnabled() && Mesh->IsVisible());
	TestTrue(TEXT("Late relevant projectile compensates for launch age"), Projectile->GetActorLocation().Equals(ExpectedPosition(), 0.1f));
	ClockDelta->SetPropertyValue_InContainer(Clock, 0.0f);
	Fixture.Advance(0.1f);
	TestTrue(TEXT("Later clock synchronization corrects the trajectory without another position packet"), Projectile->GetActorLocation().Equals(ExpectedPosition(), 0.1f));
	TestTrue(TEXT("Visible mesh follows the rebuilt root without interpolation lag"), Mesh->GetComponentLocation().Equals(Projectile->GetActorLocation(), 0.01f));
	Projectile->Initialize(Source, 999, 999);
	TestTrue(TEXT("Client Initialize cannot change authoritative launch velocity"), FVector(Projectile->GetLaunchState().Velocity).Equals(Launch.Velocity, 0.01f));
	TestEqual(TEXT("Client Initialize cannot change the authoritative launch time"), Projectile->GetLaunchState().ServerLaunchTime, Launch.ServerLaunchTime);

	TWeakObjectPtr<AmultiplayerCombatProjectile> ServerLifetime = Server;
	Fixture.Advance(3.1f);
	TestFalse(TEXT("Authority expires its real projectile"), ServerLifetime.IsValid());
	TestFalse(TEXT("Expired visual is hidden while waiting for authoritative destruction"), Mesh->IsVisible());
	TestTrue(TEXT("Client does not locally destroy the authoritative actor"), IsValid(Projectile));
	TestTrue(TEXT("Delayed destruction cannot extend the displayed trajectory past lifetime"), Projectile->GetActorLocation().Equals(ExpectedPosition(), 0.1f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileWallTest, "Multiplayer.Combat.ProjectileWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 降低位置复制频率不改变服务器扫掠；枪口前方的墙也不能被初始出生位置绕过。
bool FCoopCombatProjectileWallTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	UWorld* World = Fixture.World;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerCombatCharacter* Source = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	AmultiplayerCombatEnemy* Enemy = World->SpawnActor<AmultiplayerCombatEnemy>(FVector(500, 0, 100), FRotator::ZeroRotator, Spawn);
	AActor* WallActor = World->SpawnActor<AActor>(Spawn);
	if (!TestNotNull(TEXT("Wall test source created"), Source) || !TestNotNull(TEXT("Wall test enemy created"), Enemy)
		|| !TestNotNull(TEXT("Wall actor created"), WallActor)) return false;
	Source->GetCharacterMovement()->DisableMovement();
	Enemy->GetCharacterMovement()->DisableMovement();
	UBoxComponent* Wall = NewObject<UBoxComponent>(WallActor);
	WallActor->SetRootComponent(Wall);
	Wall->SetBoxExtent(FVector(5, 150, 200));
	Wall->SetCollisionObjectType(ECC_WorldStatic);
	Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Wall->SetCollisionResponseToAllChannels(ECR_Block);
	Wall->RegisterComponent();
	WallActor->SetActorLocation(FVector(175, 0, 140));
	TestTrue(TEXT("Clear muzzle creates real projectile"), Source->FireProjectile(25, 1600));
	TWeakObjectPtr<AmultiplayerCombatProjectile> FiredProjectile;
	for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) FiredProjectile = *It;
	TestTrue(TEXT("Wall test observes the fired projectile"), FiredProjectile.IsValid());
	Fixture.Advance(0.4f);
	TestFalse(TEXT("Server swept projectile is destroyed at near wall"), FiredProjectile.IsValid());
	TestEqual(TEXT("Near wall prevents damage to enemy behind it"), Enemy->GetHealth(), 100.0f);

	// 墙位于来源到 75 cm 枪口之间时，出生前检测直接拒绝发射。
	WallActor->SetActorLocation(FVector(40, 0, 140));
	TestFalse(TEXT("Wall between source and muzzle rejects firing"), Source->FireProjectile(25, 1600));
	int32 ProjectileCount = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) ++ProjectileCount;
	TestEqual(TEXT("Rejected muzzle shot creates no projectile"), ProjectileCount, 0);
	Fixture.Advance(0.4f);
	TestEqual(TEXT("Rejected muzzle shot applies no damage"), Enemy->GetHealth(), 100.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatBehaviorAssetsTest, "Multiplayer.Combat.BehaviorAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 运行用资产必须带黑板、实际节点和可在编辑器打开的图，不临时生成替代树。
bool FCoopCombatBehaviorAssetsTest::RunTest(const FString& Parameters)
{
	UBehaviorTree* Tree = GetDefault<AmultiplayerCombatEnemy>()->GetCombatBehaviorTree();
	if (!TestNotNull(TEXT("Default enemy references a saved behavior tree"), Tree)) return false;
	TestEqual(TEXT("Default behavior tree is the project asset"), Tree->GetPathName(),
		FString(TEXT("/Game/Combat/AI/BT_CombatEnemy.BT_CombatEnemy")));
	if (!TestNotNull(TEXT("Behavior tree has a blackboard"), Tree->BlackboardAsset.Get())
		|| !TestNotNull(TEXT("Behavior tree has a compiled root"), Tree->RootNode.Get())) return false;
	const UBlackboardData* Blackboard = Tree->BlackboardAsset;
	TestTrue(TEXT("TargetActor is an object blackboard key"),
		Blackboard->GetKeyType(Blackboard->GetKeyID(CombatAIKeys::TargetActor)) == UBlackboardKeyType_Object::StaticClass());
	const FBlackboardEntry* TargetEntry = Blackboard->GetKey(Blackboard->GetKeyID(CombatAIKeys::TargetActor));
	const FBlackboardEntry* AttackEntry = Blackboard->GetKey(Blackboard->GetKeyID(CombatAIKeys::CanAttack));
	TestTrue(TEXT("Each AI owns its target and attack condition instead of sharing blackboard values"),
		TargetEntry != nullptr && AttackEntry != nullptr && !TargetEntry->bInstanceSynced && !AttackEntry->bInstanceSynced);
	const UBlackboardKeyType_Object* TargetType = TargetEntry ? Cast<UBlackboardKeyType_Object>(TargetEntry->KeyType) : nullptr;
	TestTrue(TEXT("TargetActor accepts actor objects rather than unrelated UObject data"),
		TargetType != nullptr && TargetType->BaseClass != nullptr && TargetType->BaseClass->IsChildOf(AActor::StaticClass()));
	TestTrue(TEXT("CanAttack is a bool blackboard key"),
		Blackboard->GetKeyType(Blackboard->GetKeyID(CombatAIKeys::CanAttack)) == UBlackboardKeyType_Bool::StaticClass());
	TestTrue(TEXT("Tree contains the combat target service"),
		ContainsCombatAINode(Tree->RootNode, UmultiplayerBTService_UpdateCombatTarget::StaticClass()));
	TestTrue(TEXT("Tree contains the GAS attack request task"),
		ContainsCombatAINode(Tree->RootNode, UmultiplayerBTTask_RequestCombatAttack::StaticClass()));
	TestTrue(TEXT("Tree uses the native MoveTo task"), ContainsCombatAINode(Tree->RootNode, UBTTask_MoveTo::StaticClass()));
	TestTrue(TEXT("Tree has waits for bounded retry"), ContainsCombatAINode(Tree->RootNode, UBTTask_Wait::StaticClass()));
#if WITH_EDITORONLY_DATA
	if (TestNotNull(TEXT("Behavior tree preserves its editable graph"), Tree->BTGraph.Get()))
		TestTrue(TEXT("Editable behavior graph contains actual nodes"), Tree->BTGraph->Nodes.Num() > 1);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatBehaviorDecisionTest, "Multiplayer.Combat.BehaviorDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实树负责寻敌、分支与攻击；无导航世界只验证决策，追击位移由 MapAI 验证。
bool FCoopCombatBehaviorDecisionTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	AmultiplayerCombatEnemy* Enemy = Fixture.SpawnEnemy();
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer(FVector(300, 0, 100));
	APlayerController* PC = Cast<APlayerController>(Player->GetController());
	Fixture.SpawnPlayer(FVector(80, 150, 100), false);
	AmultiplayerCombatCharacter* DeadPlayer = Fixture.SpawnPlayer(FVector(150, 150, 100));
	ApplyTestDamage(Enemy, DeadPlayer, 1000.0f);
	AmultiplayerCombatEnemy* Friendly = Fixture.SpawnEnemy(FVector(100, -150, 100));
	Fixture.Control(Friendly);
	Fixture.SpawnPlayer(FVector(2000, 0, 100));
	AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	UBehaviorTreeComponent* Brain = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;
	if (!TestNotNull(TEXT("Enemy has the combat AI controller"), AI)
		|| !TestNotNull(TEXT("Enemy has a real behavior tree component"), Brain)) return false;
	TestTrue(TEXT("Tree is actually running"), Brain->IsRunning());
	TestTrue(TEXT("Running tree matches the saved enemy asset"), Brain->GetRootTree() == Enemy->GetCombatBehaviorTree());
	UBlackboardComponent* Blackboard = AI->GetBlackboardComponent();
	if (!TestNotNull(TEXT("Running tree owns a blackboard"), Blackboard)) return false;

	Fixture.Advance(0.45f);
	TestTrue(TEXT("Service ignores uncontrolled, dead, friendly and out-of-range candidates"),
		Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	TestTrue(TEXT("Visible target in range enables the attack branch"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	TestEqual(TEXT("Tree requested one real projectile before cooldown expires"), Fixture.ShotTimes.Num(), 1);
	TestEqual(TEXT("Real GAS projectile damages the player"), Player->GetHealth(), 75.0f);
	Fixture.Advance(0.60f);
	TestTrue(TEXT("Tree continues to request attacks after cooldown"), Fixture.ShotTimes.Num() >= 2);
	for (int32 Index = 1; Index < Fixture.ShotTimes.Num(); ++Index)
		TestTrue(TEXT("Successful attacks remain limited by the GAS 0.5 second cooldown"),
			Fixture.ShotTimes[Index] - Fixture.ShotTimes[Index - 1] >= 0.49);

	// 插入真实阻挡物；CanAttack 由 Service 重新计算，不在测试里直接改黑板。
	AActor* Wall = Fixture.World->SpawnActor<AActor>();
	UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
	Wall->SetRootComponent(Box);
	Box->SetBoxExtent(FVector(20, 250, 300));
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	Wall->SetActorLocation(FVector(150, 0, 100));
	const int32 ShotsBeforeWall = Fixture.ShotTimes.Num();
	Fixture.Advance(0.60f);
	TestTrue(TEXT("Occluded player remains a pursuit target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	TestFalse(TEXT("Wall disables the attack branch"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	TestEqual(TEXT("Occluded tree does not create additional projectiles"), Fixture.ShotTimes.Num(), ShotsBeforeWall);

	PC->UnPossess();
	Fixture.Advance(0.35f);
	TestNull(TEXT("Disconnect-style unpossession clears the still-existing pawn target"),
		Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("No valid player disables attacking"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	PC->Possess(Player);
	Player->GetCharacterMovement()->DisableMovement();
	Fixture.Advance(0.35f);
	TestTrue(TEXT("Player control restoration is picked up by the tree"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	ApplyTestDamage(Enemy, Player, 1000.0f);
	Fixture.Advance(0.35f);
	TestNull(TEXT("Target death clears the blackboard target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	AmultiplayerCombatCharacter* Replacement = Fixture.SpawnPlayer(FVector(300, 0, 100));
	Fixture.Advance(0.35f);
	TestTrue(TEXT("Tree can select a new living player"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Replacement);
	Replacement->Destroy();
	Fixture.Advance(0.35f);
	TestNull(TEXT("Destroyed player does not remain a target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestEqual(TEXT("Invalid and blocked targets never bypass GAS attack eligibility"), Fixture.ShotTimes.Num(), ShotsBeforeWall);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatBehaviorLifecycleTest, "Multiplayer.Combat.BehaviorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 解除控制/重新接管与死亡通过实际角色生命周期触发，不直接操作 BT 内部执行状态。
bool FCoopCombatBehaviorLifecycleTest::RunTest(const FString& Parameters)
{
	FCombatTestWorld Fixture;
	AmultiplayerCombatEnemy* Enemy = Fixture.SpawnEnemy();
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer(FVector(300, 0, 100));
	AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	UBehaviorTreeComponent* Brain = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;
	if (!TestNotNull(TEXT("Lifecycle fixture has an AI controller"), AI)
		|| !TestNotNull(TEXT("Lifecycle fixture has an actual tree"), Brain)) return false;
	Fixture.Advance(0.35f);
	TestTrue(TEXT("Living enemy starts the behavior tree"), Brain->IsRunning());
	TestTrue(TEXT("Initial tree emitted a real attack"), Fixture.ShotTimes.Num() > 0);
	const int32 ShotsBeforeUnpossess = Fixture.ShotTimes.Num();
	AI->UnPossess();
	Fixture.Advance(0.70f);
	TestFalse(TEXT("UnPossess stops the tree"), Brain->IsRunning());
	TestEqual(TEXT("UnPossess stops path following"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
	TestEqual(TEXT("Unpossessed enemy emits no new attacks"), Fixture.ShotTimes.Num(), ShotsBeforeUnpossess);

	AI->Possess(Enemy);
	Enemy->GetCharacterMovement()->DisableMovement();
	Fixture.Advance(0.40f);
	TestTrue(TEXT("Possess restarts the saved behavior tree"), Brain->IsRunning());
	TestTrue(TEXT("Restarted tree selects the player"), AI->GetBlackboardComponent()->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	TestTrue(TEXT("Restarted tree can attack through GAS"), Fixture.ShotTimes.Num() > ShotsBeforeUnpossess);
	ApplyTestDamage(Player, Enemy, 1000.0f);
	const int32 ShotsBeforeDeath = Fixture.ShotTimes.Num();
	const FVector DeathLocation = Enemy->GetActorLocation();
	Fixture.Advance(1.0f);
	TestFalse(TEXT("Lethal GAS effect marks enemy dead"), Enemy->IsAlive());
	TestFalse(TEXT("Death stops the actual behavior tree"), Brain->IsRunning());
	TestEqual(TEXT("Death leaves no active path"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
	TestTrue(TEXT("Dead enemy remains stationary"), Enemy->GetActorLocation().Equals(DeathLocation, 0.01f));
	TestNull(TEXT("Death clears the target"), AI->GetBlackboardComponent()->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("Death clears the attack condition"), AI->GetBlackboardComponent()->GetValueAsBool(CombatAIKeys::CanAttack));
	TestEqual(TEXT("Dead enemy emits no new projectiles; existing projectiles may still finish"), Fixture.ShotTimes.Num(), ShotsBeforeDeath);
	AI->StartCombat();
	Fixture.Advance(0.35f);
	TestFalse(TEXT("Explicit start cannot reactivate a dead enemy"), Brain->IsRunning());
	TestEqual(TEXT("Dead start does not emit an attack"), Fixture.ShotTimes.Num(), ShotsBeforeDeath);
	return true;
}

namespace
{
	// 使用真实 CombatTest 地图的导航与出生器，采样期间不调用 MoveTo、Attack 或修改敌人位置。
	class FCombatMapAICommand : public IAutomationLatentCommand
	{
	public:
		explicit FCombatMapAICommand(FAutomationTestBase* InTest) : Test(InTest) {}

		bool Update() override
		{
			UWorld* World = AutomationCommon::GetAnyGameWorld();
			if (FPlatformTime::Seconds() - StartedAt > 45.0)
			{
				Test->AddError(FString::Printf(TEXT("MapAI timeout: phase=%s, path=%d, travel=%.1f cm"),
					*Phase, bObservedPath, MaxTravel));
				return true;
			}
			if (World == nullptr || !World->HasBegunPlay()) return false;
			if (World->GetNetMode() != NM_Standalone || !World->GetMapName().EndsWith(TEXT("CombatTest")))
			{
				Test->AddError(TEXT("MapAI must run in -game /Game/Combat/Maps/CombatTest standalone; not an empty synthetic world"));
				return true;
			}

			if (Phase == TEXT("Prepare"))
			{
				APlayerController* PC = World->GetFirstPlayerController();
				Player = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetCharacter()) : nullptr;
				UNavigationSystemV1* Navigation = UNavigationSystemV1::GetCurrent(World);
				if (!Player.IsValid() || Navigation == nullptr || Navigation->IsNavigationBuildInProgress()) return false;
				if (!Player->IsAlive())
				{
					Test->AddError(TEXT("Actual map player died before AI fixture preparation"));
					return true;
				}

				TArray<AmultiplayerCombatEnemy*> Enemies;
				for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It) Enemies.Add(*It);
				if (!Test->TestEqual(TEXT("Real map spawner generated three enemies"), Enemies.Num(), 3)) return true;
				for (AmultiplayerCombatEnemy* SpawnedEnemy : Enemies)
					if (!Test->TestNotNull(TEXT("Spawned enemy has AI controller"), Cast<AAIController>(SpawnedEnemy->GetController()))) return true;
				Enemy = Enemies[0];
				AI = Cast<AAIController>(Enemy->GetController());
				const ANavigationData* NavData = Navigation->GetNavDataForProps(Enemy->GetNavAgentPropertiesRef());
				if (NavData == nullptr) return false;
				if (const ARecastNavMesh* Recast = Cast<ARecastNavMesh>(NavData))
				{
					Test->AddInfo(FString::Printf(TEXT("MapAI Recast: generation=%d tileCapacity=%d valid bounds=%d bounds=%s"),
						static_cast<int32>(Recast->GetRuntimeGenerationMode()), Recast->GetNavMeshTilesCount(),
						Recast->GetNavMeshBounds().IsValid, *Recast->GetNavMeshBounds().ToString()));
				}
				for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
					Test->AddInfo(FString::Printf(TEXT("MapAI volume: position=%s scale=%s bounds=%s"),
						*It->GetActorLocation().ToString(), *It->GetActorScale3D().ToString(), *It->GetComponentsBoundingBox(true).ToString()));
				FNavLocation EnemyPoint;
				const bool bEnemyProjected = Navigation->ProjectPointToNavigation(Enemy->GetActorLocation(), EnemyPoint,
					FVector(150, 150, 300), NavData);
				Test->AddInfo(FString::Printf(TEXT("MapAI navigation: enemy=%s projected=%d point=%s agent radius=%.1f height=%.1f data=%s"),
					*Enemy->GetActorLocation().ToString(), bEnemyProjected, *EnemyPoint.Location.ToString(),
					Enemy->GetNavAgentPropertiesRef().AgentRadius, Enemy->GetNavAgentPropertiesRef().AgentHeight, *NavData->GetName()));

				// 固定方向候选点并验证完整可达路径，不凭“存在 NavMesh Actor”认定导航有效。
				FNavLocation PlayerPoint;
				bool bFoundPoint = false;
				for (int32 Direction = 0; Direction < 8; ++Direction)
				{
					const float Angle = UE_TWO_PI * Direction / 8.0f;
					const FVector Candidate = Enemy->GetActorLocation() + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * 900.0f;
					if (!Navigation->ProjectPointToNavigation(Candidate, PlayerPoint, FVector(150, 150, 300), NavData))
					{
						Test->AddInfo(FString::Printf(TEXT("MapAI candidate %d: projection failed at %s"), Direction, *Candidate.ToString()));
						continue;
					}
					const float Distance = FVector::Dist2D(Enemy->GetActorLocation(), PlayerPoint.Location);
					if (Distance < 700.0f || Distance > 1100.0f) continue;
					FPathFindingQuery Query(AI.Get(), *NavData, Enemy->GetNavAgentLocation(), PlayerPoint.Location);
					const FPathFindingResult Path = Navigation->FindPathSync(Query);
					Test->AddInfo(FString::Printf(TEXT("MapAI candidate %d: point=%s distance=%.1f path=%d partial=%d"),
						Direction, *PlayerPoint.Location.ToString(), Distance, Path.IsSuccessful(), Path.IsPartial()));
					if (Path.IsSuccessful() && !Path.IsPartial()) { bFoundPoint = true; break; }
				}
				if (!Test->TestTrue(TEXT("Map provides an actual reachable chase point 700-1100 cm away"), bFoundPoint)) return true;

				// 只隔离其他两个敌人，待测敌人仍使用自己的行为树、导航请求和 GAS 攻击。
				for (int32 Index = 1; Index < Enemies.Num(); ++Index) Enemies[Index]->Destroy();
				UBehaviorTreeComponent* Brain = Cast<UBehaviorTreeComponent>(AI->GetBrainComponent());
				if (!Test->TestTrue(TEXT("Real map enemy is running its saved behavior tree"),
					Brain != nullptr && Brain->IsRunning() && Brain->GetRootTree() == Enemy->GetCombatBehaviorTree())) return true;
				Player->GetCharacterMovement()->DisableMovement();
				Player->SetActorLocation(PlayerPoint.Location + FVector(0, 0, Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
				EnemyStart = Enemy->GetActorLocation();
				InitialHealth = Player->GetHealth();
				Phase = TEXT("Chase");
				return false;
			}

			if (!Player.IsValid() || !Enemy.IsValid() || !AI.IsValid())
			{
				Test->AddError(TEXT("Player, real spawned enemy or AI controller disappeared during MapAI"));
				return true;
			}
			if (Phase == TEXT("Chase"))
			{
				const UPathFollowingComponent* Following = AI->GetPathFollowingComponent();
				const FNavPathSharedPtr Path = Following ? Following->GetPath() : nullptr;
				bObservedPath |= Path.IsValid() && Path->IsValid() && !Path->IsPartial();
				MaxTravel = FMath::Max(MaxTravel, static_cast<float>(FVector::Dist2D(EnemyStart, Enemy->GetActorLocation())));
				const float Distance = FVector::Dist(Enemy->GetActorLocation(), Player->GetActorLocation());
				if (!bObservedPath || MaxTravel < 200.0f || Distance > 440.0f || Player->GetHealth() >= InitialHealth) return false;
				Test->TestTrue(TEXT("AI followed a real non-partial navigation path"), bObservedPath);
				Test->TestTrue(TEXT("AI moved more than 200 cm without test driving it"), MaxTravel >= 200.0f);
				Test->TestTrue(TEXT("AI reached attack range and its real projectile damaged the player"), Player->GetHealth() < InitialHealth);
				Test->AddInfo(FString::Printf(TEXT("MapAI chase: travel=%.1f cm, distance=%.1f cm, player health=%.1f -> %.1f"),
					MaxTravel, Distance, InitialHealth, Player->GetHealth()));
				ApplyTestDamage(Player.Get(), Enemy.Get(), 1000.0f);
				EnemyDeathLocation = Enemy->GetActorLocation();
				DeathAt = FPlatformTime::Seconds();
				Phase = TEXT("Dead");
				return false;
			}

			// 击杀后留一秒正常运行，检查寻路停止与位移停止，而不只检查刚写入的血量。
			if (FPlatformTime::Seconds() - DeathAt < 1.0) return false;
			Test->TestFalse(TEXT("Enemy is dead after real lethal effect"), Enemy->IsAlive());
			const UBehaviorTreeComponent* Brain = Cast<UBehaviorTreeComponent>(AI->GetBrainComponent());
			Test->TestTrue(TEXT("Dead map enemy stopped its behavior tree"), Brain != nullptr && !Brain->IsRunning());
			Test->TestEqual(TEXT("Dead AI has no active path following"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
			Test->TestTrue(TEXT("Dead enemy stayed in place"), FVector::Dist(EnemyDeathLocation, Enemy->GetActorLocation()) < 1.0f);
			Test->TestFalse(TEXT("Dead enemy collision is disabled"), Enemy->GetActorEnableCollision());
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TWeakObjectPtr<AmultiplayerCombatCharacter> Player;
		TWeakObjectPtr<AmultiplayerCombatEnemy> Enemy;
		TWeakObjectPtr<AAIController> AI;
		FString Phase = TEXT("Prepare");
		FVector EnemyStart, EnemyDeathLocation;
		double StartedAt = FPlatformTime::Seconds(), DeathAt = 0.0;
		float InitialHealth = 0.0f, MaxTravel = 0.0f;
		bool bObservedPath = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMapAITest, "Multiplayer.Combat.MapAI",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// 仅在真实运行中的 CombatTest 地图验证导航、追击和攻击，不替代 DS 双客户端或视觉验收。
bool FCoopCombatMapAITest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FCombatMapAICommand(this));
	return true;
}

#endif
