// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Navigation/PathFollowingComponent.h"

namespace
{
	// 只为 AI 状态边界创建真实世界；无导航，不把测试中的位置变化当成追击验收。
	struct FCombatAIBoundaryWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle SpawnedHandle;
		int32 ProjectileCount = 0;

		FCombatAIBoundaryWorld()
		{
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
				if (Actor->IsA<AmultiplayerCombatProjectile>()) ++ProjectileCount;
			}));
		}

		~FCombatAIBoundaryWorld()
		{
			World->RemoveOnActorSpawnedHandler(SpawnedHandle);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 推进真实 BT、GAS 和投射物，不直接执行 Service 或 Task。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.0f; Elapsed < Seconds; Elapsed += 0.05f)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, 0.05f);
			}
		}

		AmultiplayerCombatCharacter* SpawnPlayer()
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AmultiplayerCombatCharacter* Player = World->SpawnActor<AmultiplayerCombatCharacter>(
				FVector(300, 0, 100), FRotator::ZeroRotator, Spawn);
			APlayerController* PC = World->SpawnActor<APlayerController>();
			PC->SetPlayerState(World->SpawnActor<APlayerState>());
			PC->Possess(Player);
			Player->GetCharacterMovement()->DisableMovement();
			return Player;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAIControlBoundaryTest, "Multiplayer.Combat.AIControlBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Loose tag 只驱动 AI 订阅边界；眩晕伤害、效果持续时间和网络复制由技能用例另行验证。
bool FCoopCombatAIControlBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerCombatEnemy* Enemy = Fixture.World->SpawnActor<AmultiplayerCombatEnemy>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	Enemy->GetCharacterMovement()->DisableMovement();
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer();
	AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	UBehaviorTreeComponent* Brain = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;
	if (!TestNotNull(TEXT("Enemy owns its real AI controller"), AI)
		|| !TestNotNull(TEXT("Enemy runs its saved behavior tree"), Brain)) return false;
	UBlackboardComponent* Blackboard = AI->GetBlackboardComponent();
	if (!TestNotNull(TEXT("Tree owns its blackboard"), Blackboard)) return false;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	Fixture.Advance(0.15f);
	TestTrue(TEXT("Living uncontrolled enemy can decide"), Brain->IsRunning());
	TestTrue(TEXT("Normal tree selected the player"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);

	// 第一层控制立即停止旧行为；第二层控制不能被第一层的解除提前恢复。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	TestFalse(TEXT("Stun stops tree immediately without waiting for Service"), Brain->IsRunning());
	TestEqual(TEXT("Stun leaves no active path"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
	TestNull(TEXT("Stun clears stale pursuit target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("Stun clears attack eligibility"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	const int32 BeforeBlocked = Fixture.ProjectileCount;
	ASC->AddLooseGameplayTag(CombatTags::State_Casting);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	AI->StartCombat();
	Fixture.Advance(0.70f);
	TestFalse(TEXT("Casting still blocks after stun is removed"), Brain->IsRunning());
	TestEqual(TEXT("Blocked tree emits no new projectiles"), Fixture.ProjectileCount, BeforeBlocked);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Casting);
	Fixture.Advance(0.40f);
	TestTrue(TEXT("Last control removal restarts decision"), Brain->IsRunning());
	TestTrue(TEXT("Resumed tree selects current player"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	TestTrue(TEXT("Resumed tree can request GAS attack"), Fixture.ProjectileCount > BeforeBlocked);

	// 解除控制移除旧 Pawn 的订阅；旧标签变化不能让无 Pawn 的控制器重新运行。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	AI->UnPossess();
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.35f);
	TestFalse(TEXT("Old pawn control removal cannot restart an unpossessed AI"), Brain->IsRunning());
	AI->Possess(Enemy);
	Enemy->GetCharacterMovement()->DisableMovement();
	Fixture.Advance(0.35f);
	TestTrue(TEXT("Repossess restores normal decision"), Brain->IsRunning());
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	TestFalse(TEXT("Repossessed pawn still has its control subscription"), Brain->IsRunning());

	// 暂停期间目标销毁，恢复时从根重新选目标，不复用旧 MoveTo 或攻击快照。
	Player->Destroy();
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.35f);
	TestNull(TEXT("Destroyed target is not restored with the old behavior"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	AmultiplayerCombatCharacter* Replacement = Fixture.SpawnPlayer();
	Fixture.Advance(0.35f);
	TestTrue(TEXT("Tree can acquire a new player after recovery"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Replacement);

	// 再用正式权威接口施加短眩晕，验证实际 GE 到期也走同一条恢复链路。
	Enemy->ApplyStun(0.35f);
	TestTrue(TEXT("Authoritative stun applies the real state effect"), ASC->HasMatchingGameplayTag(CombatTags::State_Stunned));
	TestFalse(TEXT("Real stun effect immediately stops the tree"), Brain->IsRunning());
	const int32 BeforeRealStun = Fixture.ProjectileCount;
	Fixture.Advance(0.15f);
	TestEqual(TEXT("Real active stun emits no new attacks"), Fixture.ProjectileCount, BeforeRealStun);
	Fixture.Advance(0.35f);
	TestFalse(TEXT("Real stun effect expires"), ASC->HasMatchingGameplayTag(CombatTags::State_Stunned));
	TestTrue(TEXT("Actual effect expiration restarts the tree"), Brain->IsRunning());

	// 死亡优先于控制到期；真实伤害走 GE，移除控制不能使尸体重新执行树。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	UAbilitySystemComponent* SourceASC = Replacement->GetAbilitySystemComponent();
	FGameplayEffectSpecHandle Damage = SourceASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f, SourceASC->MakeEffectContext());
	Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.0f);
	SourceASC->ApplyGameplayEffectSpecToTarget(*Damage.Data.Get(), ASC);
	const int32 BeforeDeathRecovery = Fixture.ProjectileCount;
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.70f);
	TestFalse(TEXT("Dead enemy stays dead"), Enemy->IsAlive());
	TestFalse(TEXT("Control ending never restarts a dead enemy"), Brain->IsRunning());
	TestEqual(TEXT("Dead enemy cannot emit new attacks"), Fixture.ProjectileCount, BeforeDeathRecovery);
	return true;
}

#endif
