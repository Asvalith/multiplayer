// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Tests/AutomationCommon.h"

namespace
{
	// 测试世界独立于编辑中的关卡；结束后释放 Context、Actor、定时器和物理场景。
	struct FCombatTestWorld
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		uint64 PreviousFrame = GFrameCounter;
		FCombatTestWorld()
		{
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
		}
		~FCombatTestWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		// 固定步长推进真实投射物和 GAS 到期回收，不靠等待墙钟时间判断结果。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0; Elapsed < Seconds; Elapsed += 0.05f)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, 0.05f);
			}
		}
	};

	// 伤害走真实 GameplayEffect；没有直接改 Health 来假装验证伤害系统。
	void ApplyTestDamage(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target, float Damage)
	{
		UAbilitySystemComponent* ASC = Source->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Damage);
		ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
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
	TestEqual(TEXT("Player receives three skills"), ASC->GetActivatableAbilities().Num(), 3);
	TestEqual(TEXT("Enemy receives only attack"), Enemy->GetAbilitySystemComponent()->GetActivatableAbilities().Num(), 1);
	TestTrue(TEXT("Only defense uses local prediction"), GetDefault<UmultiplayerDefendAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestTrue(TEXT("Attack remains server only"), GetDefault<UmultiplayerAttackAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestTrue(TEXT("Healing remains server only"), GetDefault<UmultiplayerHealAbility>()->GetNetExecutionPolicy()
		== EGameplayAbilityNetExecutionPolicy::ServerOnly);
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
	Friend->GetAbilitySystemComponent()->ApplyGameplayEffectToSelf(GetDefault<UmultiplayerHealEffect>(), 1, ASC->MakeEffectContext());
	TestEqual(TEXT("Raw healing effect cannot resurrect"), Friend->GetHealth(), 0.0f);
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
					Test->AddInfo(FString::Printf(TEXT("MapAI Recast: generation=%d tiles=%d valid bounds=%d bounds=%s"),
						static_cast<int32>(Recast->GetRuntimeGenerationMode()), Recast->GetNavMeshTilesCount(),
						Recast->GetNavMeshBounds().IsValid, *Recast->GetNavMeshBounds().ToString()));
				}
				for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
					Test->AddInfo(FString::Printf(TEXT("MapAI volume: position=%s scale=%s bounds=%s"),
						*It->GetActorLocation().ToString(), *It->GetActorScale3D().ToString(), *It->GetComponentsBoundingBox().ToString()));
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

				// 只隔离其他两个敌人，待测敌人仍使用自己的计时器、导航请求和 GAS 攻击。
				for (int32 Index = 1; Index < Enemies.Num(); ++Index) Enemies[Index]->Destroy();
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
