// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "multiplayerCombatEnemy.generated.h"

class UStaticMeshComponent;
class UBehaviorTree;
class UWidgetComponent;
class UmultiplayerEnemyHealthWidgetComponent;
class AmultiplayerGameMode;
class UAISenseConfig_Sight;

// 与 BB_CombatEnemy 中的键一致；当前目标属于各 AI 的黑板，不保存在共享节点对象上。
namespace CombatAIKeys
{
	inline const FName TargetActor(TEXT("TargetActor"));
	inline const FName CanAttack(TEXT("CanAttack"));
	inline const FName CanMove(TEXT("CanMove"));
	inline const FName LastKnownLocation(TEXT("LastKnownLocation"));
	inline const FName SearchUntil(TEXT("SearchUntil"));
	inline const FName HomeLocation(TEXT("HomeLocation"));
	inline const FName PatrolLocation(TEXT("PatrolLocation"));
}

/** 服务器驱动的基础敌人：寻找玩家、导航追击，并通过 GAS 发起攻击。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatEnemy : public AmultiplayerCombatCharacter
{
	GENERATED_BODY()

public:
	AmultiplayerCombatEnemy();
	// 范围配置由 Service 和攻击任务读取，运行中的目标只保存在黑板。
	float GetDetectionRadius() const { return DetectionRadius; }
	float GetAttackDistance() const { return AttackDistance; }
	float GetPeripheralVisionAngle() const { return PeripheralVisionAngle; }
	float GetPatrolRadius() const { return PatrolRadius; }
	float GetSearchDuration() const { return SearchDuration; }
	UBehaviorTree* GetCombatBehaviorTree() const;
	// 出生前选择攻击类型，不更换小兵外观、行为树或已运行的技能实例。
	bool ConfigureRangedArchetype();
	int32 GetExperienceReward() const { return FMath::Max(0, ExperienceReward); }

protected:
	// GAS 初始化完成后通知控制器启动行为树；客户端只显示复制的移动和战斗状态。
	virtual void BeginPlay() override;
	// 退出前解除头顶血条与角色的订阅，不影响服务器行为树的原有清理。
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	// 基类完成死亡结算后，立即停止追击并延迟清理尸体。
	virtual void HandleDeath() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float DetectionRadius = 1500.0f;
	// 视角为单侧半角；视线被挡后只搜索最后看见的位置，不再读取隐藏玩家的当前位置。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "1.0", ClampMax = "180.0"))
	float PeripheralVisionAngle = 75.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float PatrolRadius = 700.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "0.1"))
	float SearchDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float AttackDistance = 400.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|AI")
	TSoftObjectPtr<UBehaviorTree> CombatBehaviorTree;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0"))
	int32 ExperienceReward = 50;
	// 武器只随右手骨骼显示；近战判定由 GAS 服务器距离/朝向检查完成，不使用武器碰撞。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Presentation")
	TObjectPtr<UStaticMeshComponent> EnemyWeapon;

private:
	UPROPERTY(VisibleAnywhere, Category = "Combat|AI")
	TObjectPtr<UStaticMeshComponent> EnemyVisual;
	UPROPERTY(VisibleAnywhere, Category = "Combat|UI")
	TObjectPtr<UmultiplayerEnemyHealthWidgetComponent> HealthBarWidget;
};

/** 服务器控制器负责行为树生命周期；GAS 状态决定是否允许决策，移动仍交给 CMC。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatAIController : public AAIController
{
	GENERATED_BODY()

public:
	AmultiplayerCombatAIController();
	// 自动控制与 GAS 初始化先后可能不同；两个就绪入口汇合到这里。
	void StartCombat();

protected:
	// 支持开始游戏后重新接管敌人，不重新初始化血量或授予技能。
	virtual void OnPossess(APawn* InPawn) override;
	// 标签监听跟随当前 Pawn，解除控制后不能让旧角色恢复这棵行为树。
	virtual void OnUnPossess() override;

private:
	UPROPERTY()
	TObjectPtr<UAISenseConfig_Sight> SightConfig;
	// 控制状态变化时中断当前行为；所有阻断状态解除后从根节点重新选目标。
	void HandleActionStateChanged(FGameplayTag Tag, int32 NewCount);
	bool bCombatActionBlocked = false;
};

/** 低频更新目标和攻击条件，不在 Service 内寻路、施放技能或扣血。 */
UCLASS()
class MULTIPLAYER_API UmultiplayerBTService_UpdateCombatTarget : public UBTService
{
	GENERATED_BODY()

public:
	UmultiplayerBTService_UpdateCombatTarget();

protected:
	// 每次选择最近的存活玩家；失去控制、死亡、销毁或超出范围时不再保留旧目标。
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
};

/** 一次合法的攻击请求；是否满足冷却和最终伤害结算继续由 GAS 负责。 */
UCLASS()
class MULTIPLAYER_API UmultiplayerBTTask_RequestCombatAttack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UmultiplayerBTTask_RequestCombatAttack();

protected:
	// 请求前复核目标与视线，防止两次 Service 更新之间使用失效的黑板快照。
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};

/** 只选择巡逻落点；原生 MoveTo 和 Wait 负责实际移动、停留以及被战斗分支中止。 */
UCLASS()
class MULTIPLAYER_API UmultiplayerBTTask_SelectPatrolLocation : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UmultiplayerBTTask_SelectPatrolLocation();

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};

/** 放入关卡即可由服务器生成一波敌人；客户端不独立生成敌人。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerEnemySpawner : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerEnemySpawner();

	// 一次性刷怪入口，重复调用不会生成第二波。
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat")
	void SpawnWave();
	// 关卡波次编排只在出生前配置单波，不重置已启动出生器的去重状态。
	bool ConfigureWave(TSubclassOf<AmultiplayerCombatEnemy> InEnemyClass, int32 Count);
	DECLARE_MULTICAST_DELEGATE_OneParam(FEnemySpawned, AmultiplayerCombatEnemy*);
	FEnemySpawned OnEnemySpawned;
	int32 GetPendingSpawnCount() const { return PendingSpawnCount; }

protected:
	// 随关卡启动一波；重开加载新世界时会创建新的出生器。
	// 联机时等首位玩家接纳后再启动，让客户端能看到实际分批出现过程。
	virtual void BeginPlay() override;
	// 出生器退出时取消尚未生成的敌人，不把旧关卡的刷怪任务留到后续流程。
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<AmultiplayerCombatEnemy> EnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "1", ClampMax = "10"))
	int32 EnemyCount = 3;
	// 显式启用时，同一波第三只改用远程攻击；普通玩法默认全部为近战小兵。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	bool bIncludeRangedMinion = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "100.0"))
	float SpawnRadius = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0.05"))
	float MinSpawnInterval = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0.05"))
	float MaxSpawnInterval = 2.5f;

private:
	// 每次只尝试生成一只；整波数量在启动时固定，避免失败后无限补刷。
	void SpawnNextEnemy(int32 WaveCount);
	FTimerHandle SpawnTimer;
	TWeakObjectPtr<AmultiplayerGameMode> WaitingGameMode;
	FDelegateHandle SessionAcceptedHandle;
	int32 NextSpawnIndex = 0;
	int32 PendingSpawnCount = 0;
	bool bWaveSpawned = false;
};
