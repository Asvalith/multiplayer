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
	AmultiplayerCoopGate();

	// 保留配置门槛，即使有效压力板不足也不下调。
	int32 GetRequiredPlateCount() const { return FMath::Max(1, RequiredActivePlateCount); }

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive);

	UFUNCTION()
	void HandleRequiredPlateOccupancyChanged(AmultiplayerPressurePlate* Plate, int32 PlayerCount);

	UFUNCTION()
	void HandleRequiredPlateDestroyed(AActor* DestroyedActor);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_GateOpen();

private:
	void EvaluateGateState();

	// 调用方保证指针可用，包括正在执行销毁回调的板。
	void UnbindRequiredPlate(AmultiplayerPressurePlate* Plate);

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
