// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "GameFramework/Actor.h"
#include "multiplayerCombatEnemy.generated.h"

class UStaticMeshComponent;

/** 服务器驱动的基础敌人：寻找玩家、导航追击，并通过 GAS 发起攻击。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatEnemy : public AmultiplayerCombatCharacter
{
	GENERATED_BODY()

public:
	AmultiplayerCombatEnemy();

protected:
	// 服务器启动低频决策；客户端只显示复制的移动和战斗状态。
	virtual void BeginPlay() override;
	// 离开关卡时取消决策，避免定时器继续访问旧世界。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 基类完成死亡结算后，立即停止追击并延迟清理尸体。
	virtual void HandleDeath() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float DetectionRadius = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|AI", meta = (ClampMin = "100.0"))
	float AttackDistance = 400.0f;

private:
	// 每次决策选择最近的存活玩家；持续追击交给引擎导航，不手动修改位置。
	void UpdateCombat();

	UPROPERTY(VisibleAnywhere, Category = "Combat|AI")
	TObjectPtr<UStaticMeshComponent> EnemyVisual;

	TWeakObjectPtr<AmultiplayerCombatCharacter> ChaseTarget;
	FTimerHandle DecisionTimer;
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
