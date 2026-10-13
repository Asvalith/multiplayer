// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatEnemy.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// 出生器本身不复制；真正生成的 Character 由服务器复制给客户端。
AmultiplayerEnemySpawner::AmultiplayerEnemySpawner()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SpawnerRoot")));
	EnemyClass = AmultiplayerCombatEnemy::StaticClass();
}

// 关卡中配置位置和数量即可，不把战斗刷怪逻辑混入机关 GameMode。
void AmultiplayerEnemySpawner::BeginPlay()
{
	Super::BeginPlay();
	SpawnWave();
}

// 优先选择导航可达点；没有导航时仍可生成站立靶敌，但不会伪造导航追击。
void AmultiplayerEnemySpawner::SpawnWave()
{
	// 非复制的关卡 Actor 在客户端也可能拥有 Authority，必须按世界网络模式阻止客户端刷怪。
	if (GetNetMode() == NM_Client || bWaveSpawned || EnemyClass == nullptr)
	{
		return;
	}
	bWaveSpawned = true;

	UWorld* World = GetWorld();
	UNavigationSystemV1* Navigation = UNavigationSystemV1::GetCurrent(World);
	const int32 Count = FMath::Clamp(EnemyCount, 1, 10);
	const float CapsuleHalfHeight = EnemyClass.GetDefaultObject()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Angle = UE_TWO_PI * Index / Count;
		FVector SpawnLocation = GetActorLocation() + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * SpawnRadius;
		FNavLocation ReachablePoint;
		if (Navigation != nullptr && Navigation->GetRandomReachablePointInRadius(GetActorLocation(), SpawnRadius, ReachablePoint))
		{
			SpawnLocation = ReachablePoint.Location + FVector(0.0f, 0.0f, CapsuleHalfHeight);
		}
		else
		{
			// 环形备用出生点做落地检测，避免胶囊中心直接放在地板中。
			FHitResult GroundHit;
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CoopEnemySpawnGround), false, this);
			if (World->LineTraceSingleByChannel(GroundHit, SpawnLocation + FVector(0.0f, 0.0f, 1000.0f),
				SpawnLocation - FVector(0.0f, 0.0f, 2000.0f), ECC_WorldStatic, QueryParams))
			{
				SpawnLocation.Z = GroundHit.ImpactPoint.Z + CapsuleHalfHeight;
			}
		}
		World->SpawnActor<AmultiplayerCombatEnemy>(EnemyClass, SpawnLocation, GetActorRotation(), SpawnParameters);
	}
}

// 使用原生外观和 AIController，第一版不依赖额外的动画或行为树资产。
AmultiplayerCombatEnemy::AmultiplayerCombatEnemy()
{
	CombatTeam = EMultiplayerCombatTeam::Enemies;
	bGrantSupportAbilities = false;
	GetAbilitySystemComponent()->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
	AIControllerClass = AAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	bUseControllerRotationYaw = false;
	GetCapsuleComponent()->InitCapsuleSize(40.0f, 80.0f);
	GetCharacterMovement()->MaxWalkSpeed = 220.0f;
	GetCharacterMovement()->bOrientRotationToMovement = true;

	EnemyVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EnemyVisual"));
	EnemyVisual->SetupAttachment(GetCapsuleComponent());
	EnemyVisual->SetRelativeScale3D(FVector(0.7f, 0.7f, 1.4f));
	EnemyVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		EnemyVisual->SetStaticMesh(SphereMesh.Object);
	}
}

// 决策只在服务器执行；导航追击需要关卡中存在覆盖活动区域的 NavMesh。
void AmultiplayerCombatEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(DecisionTimer, this, &AmultiplayerCombatEnemy::UpdateCombat, 0.25f, true);
	}
}

// 显式清理定时器，涵盖正常销毁、重开和服务器关卡退出。
void AmultiplayerCombatEnemy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(DecisionTimer);
	Super::EndPlay(EndPlayReason);
}

// 停止攻击和寻路后再保留短暂尸体，死亡角色不再参与战斗。
void AmultiplayerCombatEnemy::HandleDeath()
{
	Super::HandleDeath();
	GetWorldTimerManager().ClearTimer(DecisionTimer);
	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		AI->StopMovement();
	}
	ChaseTarget.Reset();
	SetLifeSpan(4.0f);
}

// 目标选择、导航和攻击都使用服务器状态，客户端不能自行决定敌人的目标或伤害。
void AmultiplayerCombatEnemy::UpdateCombat()
{
	AAIController* AI = Cast<AAIController>(GetController());
	if (AI == nullptr || !IsAlive())
	{
		return;
	}

	AmultiplayerCombatCharacter* Target = nullptr;
	float NearestDistanceSquared = FMath::Square(DetectionRadius);
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
	{
		AmultiplayerCombatCharacter* Candidate = *It;
		if (!Candidate->IsPlayerControlled() || !Candidate->IsAlive() || !IsHostileTo(Candidate))
		{
			continue;
		}
		const float DistanceSquared = FVector::DistSquared(GetActorLocation(), Candidate->GetActorLocation());
		if (DistanceSquared < NearestDistanceSquared)
		{
			NearestDistanceSquared = DistanceSquared;
			Target = Candidate;
		}
	}

	// 没有合法玩家就结束追击，不继续奔向已经离开或死亡的旧目标。
	if (Target == nullptr)
	{
		AI->StopMovement();
		ChaseTarget.Reset();
		return;
	}

	// 进入射程且视线无遮挡才攻击；技能自身处理冷却和死亡等激活限制。
	const bool bHasLineOfSight = AI->LineOfSightTo(Target);
	if (NearestDistanceSquared <= FMath::Square(AttackDistance) && bHasLineOfSight)
	{
		AI->StopMovement();
		ChaseTarget.Reset();
		AI->SetControlRotation((Target->GetActorLocation() - GetActorLocation()).Rotation());
		GetAbilitySystemComponent()->TryActivateAbilityByClass(UmultiplayerAttackAbility::StaticClass());
		return;
	}

	// MoveToActor 会跟踪目标移动，不需要每轮决策重新发出同一个寻路请求。
	if (ChaseTarget.Get() != Target || AI->GetMoveStatus() == EPathFollowingStatus::Idle)
	{
		// 隔墙时继续接近，不能用射程作为到达条件，否则会隔着墙停住。
		const float AcceptanceRadius = bHasLineOfSight ? AttackDistance * 0.75f : 50.0f;
		AI->MoveToActor(Target, AcceptanceRadius, true, true, false, nullptr, false);
		ChaseTarget = Target;
	}
}
