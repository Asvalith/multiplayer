// Copyright Epic Games, Inc. All Rights Reserved.
// 关卡战斗模块只编排有限波次与一次性补给，服务器提交状态供 GameState/HUD 使用。
// 单个敌人的生成、决策和技能继续交给出生器、行为树与 GAS，不在这里重复实现。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "multiplayerCombatEncounter.generated.h"

class AmultiplayerCombatEnemy;
class AmultiplayerCoopGameState;
class AmultiplayerEnemySpawner;
class AmultiplayerGameMode;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

// 只编排关卡波次；每波继续复用现有出生器、BT、GAS和CMC，不建立另一套战斗逻辑。
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatEncounter : public AActor
{
	GENERATED_BODY()
public:
	// 创建无 Tick 的关卡编排器；敌人类型由正式蓝图配置，不在构造中生成敌人。
	AmultiplayerCombatEncounter();
	// 返回已启动的波次序号；客户端界面应读取 GameState 的复制快照。
	int32 GetCurrentWave() const { return CurrentWave; }
	// 按仍有效且存活的已登记敌人计数，不包含出生器尚未生成的数量。
	int32 GetLivingEnemyCount() const;
protected:
	// 服务器订阅接纳及终局事件，补读当前状态后尝试启动。
	virtual void BeginPlay() override;
	// 终止出生任务并解绑本关卡的玩家、敌人和终局监听。
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditAnywhere, Category="Encounter") TSubclassOf<AmultiplayerCombatEnemy> EnemyClass;
	UPROPERTY(EditAnywhere, Category="Encounter") TArray<int32> WaveSizes{3, 4, 5};
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="5")) float WaveInterval = 30.f;
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="1", ClampMax="20")) int32 MaxLivingEnemies = 8;
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="1", ClampMax="4")) int32 StartPlayers = 2;
private:
	// 达到已接纳玩家人数后只启动一次，单机预览允许一人。
	void TryStart();
	// 将存活与待生成数量一起限流，容量不足仅安排下一次检查。
	void StartNextWave();
	// 登记真实出生敌人并订阅状态和销毁事件，不接管敌人的战斗执行。
	void RegisterEnemy(AmultiplayerCombatEnemy* Enemy);
	// 发布完整关卡快照到 GameState，让 HUD 不必扫描敌人。
	void PublishState();
	// 敌人生命阶段变化后重新统计，不按通知次数推算存活数量。
	UFUNCTION() void HandleEnemyChanged();
	// 删除失效弱引用并发布当前计数，销毁通知不再补刷敌人。
	UFUNCTION() void HandleEnemyDestroyed(AActor* Actor);
	// 终局先提交停止标记，再取消出生并停止已有敌人，重复调用无副作用。
	UFUNCTION() void StopEncounter();
	FTimerHandle WaveTimer;
	FDelegateHandle AcceptedHandle;
	TWeakObjectPtr<AmultiplayerGameMode> BoundGameMode;
	TWeakObjectPtr<AmultiplayerCoopGameState> BoundGameState;
	TArray<TWeakObjectPtr<AmultiplayerCombatEnemy>> Enemies;
	TArray<TWeakObjectPtr<AmultiplayerEnemySpawner>> Spawners;
	int32 CurrentWave = 0;
	bool bStarted = false;
	bool bStopped = false;
};

// 补给经同一个GAS属性入口生效；满资源不消耗，服务器先占用再施加，避免双人重复领取。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatSupplyEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 定义生命和能量的即时补充，数值由服务器补给 Spec 写入。
	UmultiplayerCombatSupplyEffect();
};

// 补给 Actor 只复制一次性消费状态；实际加资源仍走 ASC 和 AttributeSet。
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatSupply : public AActor
{
	GENERATED_BODY()
public:
	// 创建触发球和空的表现组件；文字、网格和可见性由正式补给蓝图配置。
	AmultiplayerCombatSupply();
	// 复制是否消费，不复制可由客户端重复提交的奖励请求。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 返回已提交的消费状态，不根据表现可见性判断是否领取。
	bool IsConsumed() const { return bConsumed; }
protected:
	// 只有服务器监听领取重叠，所有端补读当前消费表现。
	virtual void BeginPlay() override;
	// 随 Actor 退出解绑重叠，避免残留领取回调。
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditAnywhere, Category="Supply", meta=(ClampMin="0")) float HealthAmount = 35.f;
	UPROPERTY(EditAnywhere, Category="Supply", meta=(ClampMin="0")) float EnergyAmount = 40.f;
private:
	// 服务器验证玩家和资源缺口，先占用补给再施加效果，防止同步通知重复领取。
	UFUNCTION() void HandleOverlap(UPrimitiveComponent* Component, AActor* Other, UPrimitiveComponent* OtherComponent,
		int32 BodyIndex, bool bFromSweep, const FHitResult& Hit);
	// 消费后两端隐藏表现，碰撞只在未消费的服务器开启。
	UFUNCTION() void OnRep_Consumed();
	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Trigger;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> Label;
	UPROPERTY(ReplicatedUsing=OnRep_Consumed) bool bConsumed = false;
};
