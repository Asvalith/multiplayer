// Copyright Epic Games, Inc. All Rights Reserved.
// 服务器感知与行为树只产生目标和技能请求，GAS 负责资格与伤害，CMC 负责移动与避让。
// 出生器只安排有限单波的错峰生成，死亡和关卡退出分别清理各自任务与订阅。

#include "Combat/multiplayerCombatEnemy.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Core/multiplayerLog.h"
#include "Core/multiplayerGameMode.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationSystem.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "TimerManager.h"
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
	// RVO 只在服务器修正邻近敌人的移动速度，不替代行为树寻路或另外复制一套位置。
	GetCharacterMovement()->bUseRVOAvoidance = true;
	GetCharacterMovement()->AvoidanceConsiderationRadius = 400.0f;
	EnemyWeapon = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EnemyWeapon"));
	EnemyWeapon->SetupAttachment(GetMesh());
	EnemyWeapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EnemyWeapon->SetGenerateOverlapEvents(false);

	EnemyVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EnemyVisual"));
	EnemyVisual->SetupAttachment(GetCapsuleComponent());
	EnemyVisual->SetRelativeScale3D(FVector(0.7f, 0.7f, 1.4f));
	EnemyVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		EnemyVisual->SetStaticMesh(SphereMesh.Object);
	}
	// 世界空间组件跟随平滑身体；血量只响应 GAS 属性事件，不在 Tick 查询。
	HealthBarWidget = CreateDefaultSubobject<UmultiplayerEnemyHealthWidgetComponent>(TEXT("HealthBarWidget"));
	HealthBarWidget->SetupAttachment(GetCapsuleComponent());
	HealthBarWidget->SetRelativeLocation(FVector(0, 0, 115));
}

// 复用保存蓝图的动作和表现，只复制本实例的攻击参数；不修改共享资产或运行中的 ASC 授予。
bool AmultiplayerCombatEnemy::ConfigureRangedArchetype()
{
	if (!HasAuthority() || IsActorInitialized() || HasActorBegunPlay()) return false;
	CombatConfig = DuplicateObject<UmultiplayerCombatConfig>(GetCombatConfigAsset(), this);
	CombatConfig->Attack.Range = 1100.f;
	CombatConfig->Attack.Amount = 15.f;
	CombatConfig->Attack.Cost = 0.f;
	CombatConfig->Attack.Cooldown = 1.8f;
	AttackDistance = CombatConfig->Attack.Range;
	AttackAbilityClass = UmultiplayerAttackAbility::StaticClass();
	return true;
}

// 决策只在服务器执行；导航追击需要关卡中存在覆盖活动区域的 NavMesh。
void AmultiplayerCombatEnemy::BeginPlay()
{
	Super::BeginPlay();
	HealthBarWidget->InitializeForCharacter(this);
	if (HasAuthority())
	{
		// 现有敌人蓝图可能保存过旧默认值，实际出生时仍通过 CMC 正式入口注册避让。
		GetCharacterMovement()->SetAvoidanceEnabled(true);
		if (AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(GetController())) AI->StartCombat();
	}
}

// 停止世界血条订阅后交还基类的战斗和 Actor 生命周期清理。
void AmultiplayerCombatEnemy::EndPlay(const EEndPlayReason::Type Reason)
{
	HealthBarWidget->StopPresentation();
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
	HealthBarWidget->StopPresentation();
	GetCharacterMovement()->SetAvoidanceEnabled(false);
	Super::HandleDeath();
	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		if (UAIPerceptionComponent* Perception = AI->GetPerceptionComponent())
		{
			Perception->SetSenseEnabled(UAISense_Sight::StaticClass(), false);
			Perception->ForgetAll();
		}
		if (UBrainComponent* Brain = AI->GetBrainComponent()) Brain->StopLogic(TEXT("Combat enemy died"));
		AI->StopMovement();
		AI->ClearFocus(EAIFocusPriority::Gameplay);
		if (UBlackboardComponent* Blackboard = AI->GetBlackboardComponent())
		{
			Blackboard->ClearValue(CombatAIKeys::TargetActor);
			Blackboard->SetValueAsBool(CombatAIKeys::CanAttack, false);
			Blackboard->SetValueAsBool(CombatAIKeys::CanMove, false);
			Blackboard->ClearValue(CombatAIKeys::LastKnownLocation);
			Blackboard->ClearValue(CombatAIKeys::PatrolLocation);
		}
	}
	SetLifeSpan(4.0f);
}

// 感知只给服务器 AI 提供候选；阵营、玩家资格、技能和伤害仍由本项目规则判断。
AmultiplayerCombatAIController::AmultiplayerCombatAIController()
{
	UAIPerceptionComponent* Perception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("CombatPerception"));
	SetPerceptionComponent(*Perception);
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("CombatSight"));
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = true;
	SightConfig->SetMaxAge(2.f);
	Perception->ConfigureSense(*SightConfig);
	Perception->SetDominantSense(UAISense_Sight::StaticClass());
}

// 控制器可能早于 Pawn 的 BeginPlay 创建；此时等待角色完成 GAS 初始化。
void AmultiplayerCombatAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	if (AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(InPawn))
	{
		SightConfig->SightRadius = Enemy->GetDetectionRadius();
		SightConfig->LoseSightRadius = Enemy->GetDetectionRadius() * 1.2f;
		SightConfig->PeripheralVisionAngleDegrees = Enemy->GetPeripheralVisionAngle();
		GetPerceptionComponent()->ConfigureSense(*SightConfig);
		GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), true);
		for (const FGameplayTag& Tag : { CombatTags::State_Stunned.GetTag(), CombatTags::State_Casting.GetTag(),
			CombatTags::State_Rooted.GetTag(), CombatTags::State_MovementBlocked.GetTag() })
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
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), false);
	GetPerceptionComponent()->ForgetAll();
	if (AmultiplayerCombatEnemy* Enemy = Cast<AmultiplayerCombatEnemy>(GetPawn()))
	{
		for (const FGameplayTag& Tag : { CombatTags::State_Stunned.GetTag(), CombatTags::State_Casting.GetTag(),
			CombatTags::State_Rooted.GetTag(), CombatTags::State_MovementBlocked.GetTag() })
		{
			Enemy->GetAbilitySystemComponent()->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
				.RemoveAll(this);
		}
	}
	bCombatActionBlocked = false;
	Super::OnUnPossess();
	if (UBlackboardComponent* CombatBlackboard = GetBlackboardComponent())
	{
		CombatBlackboard->SetValueAsBool(CombatAIKeys::CanMove, false);
		CombatBlackboard->ClearValue(CombatAIKeys::HomeLocation);
		CombatBlackboard->ClearValue(CombatAIKeys::PatrolLocation);
		CombatBlackboard->ClearValue(CombatAIKeys::LastKnownLocation);
		CombatBlackboard->ClearValue(CombatAIKeys::SearchUntil);
	}
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
	// 禁锢只停路径，不停感知与整棵树；射程内仍可攻击，解除后由移动分支的装饰器恢复寻路。
	if (Tag == CombatTags::State_Rooted || Tag == CombatTags::State_MovementBlocked)
	{
		if (NewCount > 0) StopMovement();
		if (UBlackboardComponent* CombatBlackboard = GetBlackboardComponent())
			CombatBlackboard->SetValueAsBool(CombatAIKeys::CanMove,
				Enemy->IsCombatReady() && !Enemy->IsMovementBlocked() && !Enemy->IsCasting());
		return;
	}
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
		CombatBlackboard->SetValueAsBool(CombatAIKeys::CanMove, false);
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
	AmultiplayerCombatCharacter* PreviousTarget = Cast<AmultiplayerCombatCharacter>(Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	float NearestDistanceSquared = MAX_flt;
	// 只读筛选当前合法敌对玩家，复用于可见候选和旧目标失效判断。
	const auto IsValidPlayer = [Enemy](const AmultiplayerCombatCharacter* Candidate)
	{
		return IsValid(Candidate) && Candidate->IsPlayerControlled() && Candidate->IsCombatReady() && Enemy->IsHostileTo(Candidate);
	};
	if (Enemy->IsAlive())
	{
		// 只从 Sight 当前实际看见的对象中选择，不再逐个扫描世界里的玩家。
		TArray<AActor*> VisibleActors;
		if (UAIPerceptionComponent* Perception = AI->GetPerceptionComponent())
			Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), VisibleActors);
		for (AActor* Actor : VisibleActors)
		{
			AmultiplayerCombatCharacter* Candidate = Cast<AmultiplayerCombatCharacter>(Actor);
			if (!IsValidPlayer(Candidate) || !AI->LineOfSightTo(Candidate)) continue;
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
		&& NearestDistanceSquared <= FMath::Square(Enemy->GetAttackDistance());
	Blackboard->PauseObserverNotifications();
	if (!Blackboard->IsVectorValueSet(CombatAIKeys::HomeLocation))
		Blackboard->SetValueAsVector(CombatAIKeys::HomeLocation, Enemy->GetActorLocation());
	// 丢失时保留上次可见快照，不用失败感知刺激中的新坐标追踪墙后的玩家。
	if (Target)
	{
		Blackboard->SetValueAsVector(CombatAIKeys::LastKnownLocation, Target->GetActorLocation());
		Blackboard->SetValueAsFloat(CombatAIKeys::SearchUntil, Enemy->GetWorld()->GetTimeSeconds() + Enemy->GetSearchDuration());
	}
	else if ((PreviousTarget && !IsValidPlayer(PreviousTarget))
		|| Blackboard->GetValueAsFloat(CombatAIKeys::SearchUntil) <= Enemy->GetWorld()->GetTimeSeconds())
	{
		// 记忆从最后一次看见时计时；施法清空 TargetActor 也不会让旧位置无限续期。
		Blackboard->ClearValue(CombatAIKeys::LastKnownLocation);
		Blackboard->ClearValue(CombatAIKeys::SearchUntil);
	}
	Blackboard->SetValueAsObject(CombatAIKeys::TargetActor, Target);
	Blackboard->SetValueAsBool(CombatAIKeys::CanAttack, bCanAttack);
	// 移动权限与攻击资格分开；保留禁锢期间的目标和限时记忆，不靠清空感知来阻止 MoveTo。
	Blackboard->SetValueAsBool(CombatAIKeys::CanMove,
		Enemy->IsCombatReady() && !Enemy->IsMovementBlocked() && !Enemy->IsCasting());
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
// 攻击类型由敌人蓝图选择；近战小兵不再激活玩家的投射物技能。
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
	Enemy->GetAbilitySystemComponent()->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass());
	// 成功表示合法请求已交给 GAS，不表示命中；冷却拒绝也进入后续 Wait，避免快速重试。
	return EBTNodeResult::Succeeded;
}

// 寻路失败交给树中的短暂等待，不新增逐帧随机点查询或另一套移动定时器。
UmultiplayerBTTask_SelectPatrolLocation::UmultiplayerBTTask_SelectPatrolLocation()
{
	NodeName = TEXT("Choose reachable patrol point");
}

// 使用黑板出生区域和有限导航候选确定巡逻点，不在任务内自行寻路或移动。
EBTNodeResult::Type UmultiplayerBTTask_SelectPatrolLocation::ExecuteTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AI = OwnerComp.GetAIOwner();
	AmultiplayerCombatEnemy* Enemy = AI ? Cast<AmultiplayerCombatEnemy>(AI->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	// 装饰器负责中止旧移动分支；任务入口再挡住同次搜索里尚未消费的旧权限快照。
	if (!Enemy || !Enemy->IsCombatReady() || Enemy->IsMovementBlocked() || Enemy->IsCasting()) return EBTNodeResult::Failed;
	UNavigationSystemV1* Navigation = Enemy ? UNavigationSystemV1::GetCurrent(Enemy->GetWorld()) : nullptr;
	if (!Navigation || !Blackboard || !Blackboard->IsVectorValueSet(CombatAIKeys::HomeLocation)) return EBTNodeResult::Failed;
	const float MinimumTravel = FMath::Min(150.f, Enemy->GetPatrolRadius() * .5f);
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		FNavLocation Point;
		if (Navigation->GetRandomReachablePointInRadius(Blackboard->GetValueAsVector(CombatAIKeys::HomeLocation),
			Enemy->GetPatrolRadius(), Point) && FVector::DistSquared2D(Point.Location, Enemy->GetActorLocation()) >= FMath::Square(MinimumTravel))
		{
			Blackboard->SetValueAsVector(CombatAIKeys::PatrolLocation, Point.Location);
			return EBTNodeResult::Succeeded;
		}
	}
	Blackboard->ClearValue(CombatAIKeys::PatrolLocation);
	return EBTNodeResult::Failed;
}

// 出生器本身不复制；真正生成的 Character 由服务器复制给客户端。
AmultiplayerEnemySpawner::AmultiplayerEnemySpawner()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SpawnerRoot")));
	EnemyClass = AmultiplayerCombatEnemy::StaticClass();
}

// 延迟生成完成前固定单波类型与数量，开始后不接受配置覆盖去重状态。
bool AmultiplayerEnemySpawner::ConfigureWave(TSubclassOf<AmultiplayerCombatEnemy> InEnemyClass, int32 Count)
{
	if (GetNetMode() == NM_Client || IsActorInitialized() || HasActorBegunPlay() || !InEnemyClass) return false;
	EnemyClass = InEnemyClass;
	EnemyCount = FMath::Clamp(Count, 1, 10);
	return true;
}

// 关卡中配置位置和数量即可，不把战斗刷怪逻辑混入机关 GameMode。
void AmultiplayerEnemySpawner::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_Client) return;
	AmultiplayerGameMode* GameMode = Cast<AmultiplayerGameMode>(GetWorld()->GetAuthGameMode());
	if (GetNetMode() == NM_Standalone || !GameMode)
	{
		SpawnWave();
		return;
	}
	// 先订阅再读取当前状态，兼容出生器晚于首个玩家接纳完成时加载。
	WaitingGameMode = GameMode;
	SessionAcceptedHandle = GameMode->OnCombatSessionAccepted.AddUObject(this, &ThisClass::SpawnWave);
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsPlayerControlled() && It->IsCombatReady())
		{
			SpawnWave();
			break;
		}
	}
}

// 取消待生成任务并解除接纳事件，销毁出生器不会把旧波次留到下一关。
void AmultiplayerEnemySpawner::EndPlay(const EEndPlayReason::Type Reason)
{
	GetWorldTimerManager().ClearTimer(SpawnTimer);
	PendingSpawnCount = 0;
	if (AmultiplayerGameMode* GameMode = WaitingGameMode.Get())
		GameMode->OnCombatSessionAccepted.Remove(SessionAcceptedHandle);
	SessionAcceptedHandle.Reset();
	WaitingGameMode.Reset();
	Super::EndPlay(Reason);
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
	if (AmultiplayerGameMode* GameMode = WaitingGameMode.Get())
		GameMode->OnCombatSessionAccepted.Remove(SessionAcceptedHandle);
	SessionAcceptedHandle.Reset();
	WaitingGameMode.Reset();
	const int32 Count = FMath::Clamp(EnemyCount, 1, 10);
	PendingSpawnCount = Count;
	// 第一只立即出现，其余按间隔生成；总数与重复调用保护仍属于同一波。
	// 错峰只摊平生成峰值，不减少存活 AI 的持续开销，也不等于敌人对象池。
	SpawnNextEnemy(Count);
}

// 只拆出定时器需要的单次生成入口，落点选择和碰撞处理保持在一起。
void AmultiplayerEnemySpawner::SpawnNextEnemy(int32 WaveCount)
{
	PendingSpawnCount = FMath::Max(0, PendingSpawnCount - 1);
	UWorld* World = GetWorld();
	UNavigationSystemV1* Navigation = UNavigationSystemV1::GetCurrent(World);
	const auto* Capsule = EnemyClass.GetDefaultObject()->GetCapsuleComponent();
	const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const int32 SpawnIndex = NextSpawnIndex++;
	// 每次独立抽取下次间隔，只安排一个单次任务；生成回调若销毁出生器，EndPlay 也能取消它。
	if (NextSpawnIndex < WaveCount)
	{
		const float MinDelay = FMath::Max(MinSpawnInterval, 0.05f);
		const float MaxDelay = FMath::Max(MaxSpawnInterval, MinDelay);
		GetWorldTimerManager().SetTimer(SpawnTimer,
			FTimerDelegate::CreateUObject(this, &ThisClass::SpawnNextEnemy, WaveCount), FMath::FRandRange(MinDelay, MaxDelay), false);
	}
	// 导航点只保证地面可走，不保证出生胶囊没有与玩家/机关相交；有限搜索空间候选，不强行穿墙生成。
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CoopEnemySpawnGround), false, this);
	const auto Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius()+1.f,CapsuleHalfHeight);
	for (int32 Candidate=0; Candidate<8; ++Candidate)
	{
		const float Angle = UE_TWO_PI * (float(SpawnIndex)/WaveCount+float(Candidate)/8.f);
		FVector SpawnLocation = GetActorLocation()+FVector(FMath::Cos(Angle),FMath::Sin(Angle),0.f)*SpawnRadius;
		FNavLocation ReachablePoint;
		if (Navigation && Navigation->GetRandomReachablePointInRadius(GetActorLocation(),SpawnRadius,ReachablePoint))
			SpawnLocation=ReachablePoint.Location+FVector(0,0,CapsuleHalfHeight+2.f);
		else
		{
			// 环形备用出生点做落地检测，避免胶囊中心直接放在地板中。
			FHitResult GroundHit;
			if (World->LineTraceSingleByChannel(GroundHit,SpawnLocation+FVector(0,0,1000),SpawnLocation-FVector(0,0,2000),ECC_WorldStatic,QueryParams))
				SpawnLocation.Z=GroundHit.ImpactPoint.Z+CapsuleHalfHeight+2.f;
		}
		if (World->OverlapBlockingTestByProfile(SpawnLocation,FQuat::Identity,Capsule->GetCollisionProfileName(),Shape,QueryParams)) continue;
		const FTransform Transform(GetActorRotation(),SpawnLocation);
		if (auto* Enemy=World->SpawnActorDeferred<AmultiplayerCombatEnemy>(EnemyClass,Transform,nullptr,nullptr,
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding))
		{
			if (bIncludeRangedMinion && SpawnIndex==2) Enemy->ConfigureRangedArchetype();
			Enemy->FinishSpawning(Transform);
			if (IsValid(Enemy)) { OnEnemySpawned.Broadcast(Enemy); return; }
		}
	}
	UE_LOG(LogTemp,Warning,TEXT("Coop enemy spawn has no clear capsule position within eight candidates: %s slot=%d"),*GetName(),SpawnIndex);
}
