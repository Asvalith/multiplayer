// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "GameFramework/Actor.h"
#include "multiplayerCombatEnemy.generated.h"

class UStaticMeshComponent;
class UBehaviorTree;
class UWidgetComponent;

// 与 BB_CombatEnemy 中的键一致；当前目标属于各 AI 的黑板，不保存在共享节点对象上。
namespace CombatAIKeys
{
	inline const FName TargetActor(TEXT("TargetActor"));
	inline const FName CanAttack(TEXT("CanAttack"));
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
	UBehaviorTree* GetCombatBehaviorTree() const;

protected:
	// GAS 初始化完成后通知控制器启动行为树；客户端只显示复制的移动和战斗状态。
	virtual void BeginPlay() override;
	// 退出前解除头顶血条与角色的订阅，不影响服务器行为树的原有清理。
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	// 基类完成死亡结算后，立即停止追击并延迟清理尸体。
	virtual void HandleDeath() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float DetectionRadius = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float AttackDistance = 400.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|AI")
	TSoftObjectPtr<UBehaviorTree> CombatBehaviorTree;

private:
	UPROPERTY(VisibleAnywhere, Category = "Combat|AI")
	TObjectPtr<UStaticMeshComponent> EnemyVisual;
	UPROPERTY(VisibleAnywhere, Category = "Combat|UI")
	TObjectPtr<UWidgetComponent> HealthBarWidget;
};

/** 服务器控制器负责行为树生命周期；GAS 状态决定是否允许决策，移动仍交给 CMC。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatAIController : public AAIController
{
	GENERATED_BODY()

public:
	// 自动控制与 GAS 初始化先后可能不同；两个就绪入口汇合到这里。
	void StartCombat();

protected:
	// 支持开始游戏后重新接管敌人，不重新初始化血量或授予技能。
	virtual void OnPossess(APawn* InPawn) override;
	// 标签监听跟随当前 Pawn，解除控制后不能让旧角色恢复这棵行为树。
	virtual void OnUnPossess() override;

private:
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

protected:
	// 随关卡启动一波；重开加载新世界时会创建新的出生器。
	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<AmultiplayerCombatEnemy> EnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "1", ClampMax = "10"))
	int32 EnemyCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "100.0"))
	float SpawnRadius = 500.0f;

private:
	bool bWaveSpawned = false;
};
