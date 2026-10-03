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

	// 返回关卡配置要求；运行时有效且去重后的压力板数量不足时保持失败关闭，不能偷偷降低门槛。
	int32 GetRequiredPlateCount() const;

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
	// 统计激活板数和不同玩家数，并组合可选的钥匙目标前置条件。
	void EvaluateGateState();
	// 服务器写入与客户端 OnRep 共用；初始对齐目标，后续仅过渡阶段启用 Tick。
	void ApplyGateState(bool bSnapToTarget);

	// 从关卡配置生成有效且不重复的运行时依赖集合；之后所有绑定和计数都只使用该集合。
	void RebuildRuntimeRequiredPlates();
	// 仅服务器绑定外部压力板，客户端不重复执行规则组合。
	void BindRequiredPlates();
	// 与 BindRequiredPlates 对称，处理关卡卸载和 Actor 销毁。
	void UnbindRequiredPlates();
	// 关卡卸载和单块板销毁共用解绑顺序；调用方保证指针可用，允许销毁回调中的板进入。
	void UnbindRequiredPlate(AmultiplayerPressurePlate* Plate);

	FVector GetMeshTargetLocation() const;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> DoorMesh;

	// 用两个可视化端点定义门的行程，设计者可以在蓝图中直接调整，无需填写难理解的坐标。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> ClosedPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> OpenPoint;

	// 关卡实例显式配置依赖关系，比运行时按类型查找更可控，也支持一个关卡中多组独立机关。
	UPROPERTY(EditInstanceOnly, Category = "Coop Gate|Rules")
	TArray<TObjectPtr<AmultiplayerPressurePlate>> RequiredPlates;

	// 服务器在 BeginPlay 从 RequiredPlates 生成，避免空引用或重复引用改变规则含义。
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
