// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatEnemy.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Core/multiplayerLog.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationSystem.h"
#include "UObject/ConstructorHelpers.h"

// 使用原生外观；行为树控制决策，GAS 执行技能，移动仍由 CharacterMovement 完成。
AmultiplayerCombatEnemy::AmultiplayerCombatEnemy()
{
	CombatTeam = EMultiplayerCombatTeam::Enemies;
	bGrantSupportAbilities = false;
	GetAbilitySystemComponent()->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
	AIControllerClass = AmultiplayerCombatAIController::StaticClass();
	CombatBehaviorTree = FSoftObjectPath(TEXT("/Game/Combat/AI/BT_CombatEnemy.BT_CombatEnemy"));
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
	// 屏幕空间组件跟随头顶位置；血量只响应 GAS 属性事件，不在 Tick 查询。
	HealthBarWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("HealthBarWidget"));
	HealthBarWidget->SetupAttachment(GetCapsuleComponent());
	HealthBarWidget->SetRelativeLocation(FVector(0, 0, 115));
	HealthBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
	HealthBarWidget->SetDrawSize(FVector2D(180, 30));
	HealthBarWidget->SetPivot(FVector2D(.5f, 1));
	HealthBarWidget->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HealthBarWidget->SetGenerateOverlapEvents(false);
}

// 决策只在服务器执行；导航追击需要关卡中存在覆盖活动区域的 NavMesh。
void AmultiplayerCombatEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() != NM_DedicatedServer)
	{
		HealthBarWidget->InitWidget();
		if (auto* HealthWidget = Cast<UmultiplayerEnemyHealthWidget>(HealthBarWidget->GetUserWidgetObject())) HealthWidget->BindCharacter(this);
	}
	if (HasAuthority())
	{
		if (AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(GetController())) AI->StartCombat();
	}
}

void AmultiplayerCombatEnemy::EndPlay(const EEndPlayReason::Type Reason)
{
	if (auto* HealthWidget = Cast<UmultiplayerEnemyHealthWidget>(HealthBarWidget->GetUserWidgetObject())) HealthWidget->BindCharacter(nullptr);
	HealthBarWidget->SetWidget(nullptr);
	Super::EndPlay(Reason);
}

// 行为树是可编辑资产；软引用保留烘焙依赖，只在服务器启动决策时加载。
UBehaviorTree* AmultiplayerCombatEnemy::GetCombatBehaviorTree() const
{
	return CombatBehaviorTree.LoadSynchronous();
}

// 停止攻击和寻路后再保留短暂尸体，死亡角色不再参与战斗。
void AmultiplayerCombatEnemy::HandleDeath()
{
	Super::HandleDeath();
	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		if (UBrainComponent* Brain = AI->GetBrainComponent()) Brain->StopLogic(TEXT("Combat enemy died"));
		AI->StopMovement();
		AI->ClearFocus(EAIFocusPriority::Gameplay);
		if (UBlackboardComponent* Blackboard = AI->GetBlackboardComponent())
		{
			Blackboard->ClearValue(CombatAIKeys::TargetActor);
			Blackboard->SetValueAsBool(CombatAIKeys::CanAttack, false);
		}
	}
	SetLifeSpan(4.0f);
}

// 控制器可能早于 Pawn 的 BeginPlay 创建；此时等待角色完成 GAS 初始化。
void AmultiplayerCombatAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	if (AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(InPawn))
	{
		for (const FGameplayTag& Tag : { CombatTags::State_Stunned.GetTag(), CombatTags::State_Casting.GetTag() })
		{
			Enemy->GetAbilitySystemComponent()->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
				.AddUObject(this, &ThisClass::HandleActionStateChanged);
		}
	}
	StartCombat();
}

// 解绑先于 Super 清空 Pawn；引擎继续负责停止路径和清理 Brain。
void AmultiplayerCombatAIController::OnUnPossess()
{
	if (AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(GetPawn()))
	{
		for (const FGameplayTag& Tag : { CombatTags::State_Stunned.GetTag(), CombatTags::State_Casting.GetTag() })
		{
			Enemy->GetAbilitySystemComponent()->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
				.RemoveAll(this);
		}
	}
	bCombatActionBlocked = false;
	Super::OnUnPossess();
}

// 首次开局由 Enemy::BeginPlay 补齐启动，后续 Possess 则直接使用已经就绪的角色。
void AmultiplayerCombatAIController::StartCombat()
{
	AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(GetPawn());
	if (Enemy == nullptr || !Enemy->HasActorBegunPlay() || !Enemy->IsAlive()) return;
	const UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	bCombatActionBlocked = ASC->HasMatchingGameplayTag(CombatTags::State_Stunned)
		|| ASC->HasMatchingGameplayTag(CombatTags::State_Casting);
	if (bCombatActionBlocked) return;
	if (BrainComponent != nullptr && BrainComponent->IsRunning()) return;
	if (!RunBehaviorTree(Enemy->GetCombatBehaviorTree()))
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Combat enemy could not start its behavior tree: %s"), *Enemy->GetName());
	}
}

// 不用 Tick 反复禁止移动；标签首次进入时结束旧行为，重叠控制不会提前恢复。
void AmultiplayerCombatAIController::HandleActionStateChanged(FGameplayTag Tag, int32 NewCount)
{
	AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(GetPawn());
	if (Enemy == nullptr) return;
	const UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	const bool bBlocked = ASC->HasMatchingGameplayTag(CombatTags::State_Stunned)
		|| ASC->HasMatchingGameplayTag(CombatTags::State_Casting);
	if (bBlocked == bCombatActionBlocked) return;
	bCombatActionBlocked = bBlocked;
	if (!bBlocked)
	{
		StartCombat();
		return;
	}

	if (BrainComponent != nullptr) BrainComponent->StopLogic(TEXT("Combat action blocked by GAS"));
	StopMovement();
	ClearFocus(EAIFocusPriority::Gameplay);
	if (UBlackboardComponent* CombatBlackboard = GetBlackboardComponent())
	{
		CombatBlackboard->PauseObserverNotifications();
		CombatBlackboard->ClearValue(CombatAIKeys::TargetActor);
		CombatBlackboard->SetValueAsBool(CombatAIKeys::CanAttack, false);
		CombatBlackboard->ResumeObserverNotifications(true);
	}
}

// 沿用原来约 0.25 秒的决策节奏，首次搜索立即填黑板；不再额外维护 Pawn Timer。
UmultiplayerBTService_UpdateCombatTarget::UmultiplayerBTService_UpdateCombatTarget()
{
	NodeName = TEXT("Update combat target");
	Interval = 0.25f;
	RandomDeviation = 0.05f;
	bCallTickOnSearchStart = true;
}

// 目标选择、视线和距离都使用服务器状态，客户端不能自行决定敌人的目标或伤害。
void UmultiplayerBTService_UpdateCombatTarget::TickNode(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);
	AAIController* AI = OwnerComp.GetAIOwner();
	AmultiplayerCombatEnemy* Enemy = AI ? Cast<AmultiplayerCombatEnemy>(AI->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Enemy == nullptr || Blackboard == nullptr) return;

	AmultiplayerCombatCharacter* Target = nullptr;
	float NearestDistanceSquared = FMath::Square(Enemy->GetDetectionRadius());
	if (Enemy->IsAlive())
	{
		for (TActorIterator<AmultiplayerCombatCharacter> It(Enemy->GetWorld()); It; ++It)
		{
			AmultiplayerCombatCharacter* Candidate = *It;
			if (!Candidate->IsPlayerControlled() || !Candidate->IsCombatReady() || !Enemy->IsHostileTo(Candidate)) continue;
			const float DistanceSquared = FVector::DistSquared(Enemy->GetActorLocation(), Candidate->GetActorLocation());
			if (DistanceSquared < NearestDistanceSquared)
			{
				NearestDistanceSquared = DistanceSquared;
				Target = Candidate;
			}
		}
	}

	// 进入射程且视线无遮挡才攻击；条件改变由黑板装饰器中止追击或攻击分支。
	const bool bCanAttack = Target != nullptr
		&& NearestDistanceSquared <= FMath::Square(Enemy->GetAttackDistance()) && AI->LineOfSightTo(Target);
	Blackboard->PauseObserverNotifications();
	Blackboard->SetValueAsObject(CombatAIKeys::TargetActor, Target);
	Blackboard->SetValueAsBool(CombatAIKeys::CanAttack, bCanAttack);
	Blackboard->ResumeObserverNotifications(true);
	if (Target != nullptr) AI->SetFocus(Target);
	else AI->ClearFocus(EAIFocusPriority::Gameplay);
}

// 不把技能冷却再写一遍到行为树；树中的 Wait 控制请求节奏，GAS 判断是否真正激活。
UmultiplayerBTTask_RequestCombatAttack::UmultiplayerBTTask_RequestCombatAttack()
{
	NodeName = TEXT("Request GAS attack");
}

// 面向目标后只提交技能请求，投射物、伤害效果和死亡仍走现有 GAS 链路。
EBTNodeResult::Type UmultiplayerBTTask_RequestCombatAttack::ExecuteTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AI = OwnerComp.GetAIOwner();
	AmultiplayerCombatEnemy* Enemy = AI ? Cast<AmultiplayerCombatEnemy>(AI->GetPawn()) : nullptr;
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	AmultiplayerCombatCharacter* Target = Blackboard
		? Cast<AmultiplayerCombatCharacter>(Blackboard->GetValueAsObject(CombatAIKeys::TargetActor)) : nullptr;
	if (Enemy == nullptr || !Enemy->IsAlive() || !IsValid(Target) || !Target->IsPlayerControlled()
		|| Enemy->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Stunned)
		|| Enemy->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Casting)
		|| !Target->IsCombatReady() || !Enemy->IsHostileTo(Target)
		|| FVector::DistSquared(Enemy->GetActorLocation(), Target->GetActorLocation()) > FMath::Square(Enemy->GetAttackDistance())
		|| !AI->LineOfSightTo(Target)) return EBTNodeResult::Failed;

	AI->StopMovement();
	AI->SetControlRotation((Target->GetActorLocation() - Enemy->GetActorLocation()).Rotation());
	Enemy->GetAbilitySystemComponent()->TryActivateAbilityByClass(UmultiplayerAttackAbility::StaticClass());
	// 成功表示合法请求已交给 GAS，不表示命中；冷却拒绝也进入后续 Wait，避免快速重试。
	return EBTNodeResult::Succeeded;
}

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
