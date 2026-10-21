// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopGate.generated.h"

class AmultiplayerPressurePlate;
class AmultiplayerCoopGameState;
class UArrowComponent;
class USceneComponent;
class UStaticMeshComponent;

/**
 * 服务器组合 RequiredPlates 的激活板数、不同玩家数和可选目标条件，决定门的开关。
 * 只复制开关及初始速度，各端在 ClosedPoint/OpenPoint 之间移动门网格，静止时关闭 Tick。
 * 未同步动画起始时间，延迟下各端过渡进度可能不同；角色最终位置仍以服务器为准。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCoopGate : public AActor
{
	GENERATED_BODY()

public:
	// 创建门网格与开关端点，静止时休眠且不逐帧执行。
	AmultiplayerCoopGate();

	// 各端只在过渡中移动门网格，到位即关闭 Tick。
	virtual void Tick(float DeltaSeconds) override;
	// 复制开关与初始速度，不复制每帧网格位置。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// 各端校准初始表现，服务器收集并订阅有效压力板和目标状态。
	virtual void BeginPlay() override;
	// 成组解绑压力板与 GameState，关卡卸载不留下外部订阅。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 任意依赖板开关变化后从完整当前状态重新评估。
	UFUNCTION()
	void HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive);

	// 板上人数变化也要重算跨板不同玩家数，即便板仍保持压下。
	UFUNCTION()
	void HandleRequiredPlateOccupancyChanged(AmultiplayerPressurePlate* Plate, int32 PlayerCount);

	// 依赖销毁后解绑并剔除，不能把失效板继续算作激活。
	UFUNCTION()
	void HandleRequiredPlateDestroyed(AActor* DestroyedActor);

	// 钥匙目标更新时补足“先站上压力板、后完成目标”的开启条件。
	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	// 本机收到开关快照后启动网格过渡，服务器提交时也共用此出口。
	UFUNCTION()
	void OnRep_GateOpen();

private:
	// 保留配置门槛，即使有效压力板不足也不下调。
	int32 GetRequiredPlateCount() const { return FMath::Max(1, RequiredActivePlateCount); }

	// 仅服务器组合板数、不同玩家数与目标条件，变化时唤醒休眠并提交复制。
	void EvaluateGateState();

	// 调用方保证指针可用，包括正在执行销毁回调的板。
	void UnbindRequiredPlate(AmultiplayerPressurePlate* Plate);

	// 按当前开关选择关卡配置的世界坐标端点，不修改规则状态。
	FVector GetMeshTargetLocation() const;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> DoorMesh;

	// 蓝图可通过两个可视化端点调整门的行程。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> ClosedPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> OpenPoint;

	// 每扇门显式配置自己的压力板。
	UPROPERTY(EditInstanceOnly, Category = "Coop Gate|Rules")
	TArray<TObjectPtr<AmultiplayerPressurePlate>> RequiredPlates;

	// 服务器在 BeginPlay 从 RequiredPlates 生成。
	UPROPERTY(Transient)
	TArray<TObjectPtr<AmultiplayerPressurePlate>> RuntimeRequiredPlates;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules", meta = (ClampMin = "1"))
	int32 RequiredActivePlateCount = 1;

	// 一次开启后保持打开；否则任一必要压力板释放时门会重新关闭。
	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bStayOpenOnceActivated = false;

	// 开启前还可以等待钥匙目标完成，从而复用为关卡末端机关。
	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bRequireObjectiveComplete = false;

	// 独立运行期字段不读取旧蓝图速度；0 为未就绪，保证服务器正速度进入初始复制。
	UPROPERTY(VisibleInstanceOnly, Transient, Replicated, Category = "Coop Gate|Movement")
	float RuntimeDoorMoveSpeed = 0.0f;

	UPROPERTY(ReplicatedUsing = OnRep_GateOpen)
	bool bGateOpen = false;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
