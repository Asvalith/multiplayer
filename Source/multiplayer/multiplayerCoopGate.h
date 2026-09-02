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
 * 由服务器判定、在所需压力板激活后开启的合作门。
 *
 * 压力板负责检测玩家，门只组合多个压力板的结果。关卡设计者可摆放任意数量的压力板，
 * 再通过 RequiredPlates 建立引用，避免门和具体触发区域绑死。
 *
 * 服务器监听每块压力板以及可选的目标进度变化，重新计算 bGateOpen；客户端只收到开关状态，
 * 使用相同的 ClosedPoint/OpenPoint 插值门网格。门的碰撞随网格移动，不由客户端决定能否通过。
 *
 * (*) 门只复制开关状态，各端根据相同端点播放表现；相比持续复制门的位置，网络开销更小。
 * (**) 判定时同时检查激活压力板数量和不同玩家数量，防止一个玩家同时压住多块板绕过双人条件。
 * (**) RequiredPlates 是关卡实例引用，必须在 EndPlay 对每个外部 Delegate 对称解绑；否则重新加载
 * 关卡或销毁机关后，压力板仍可能回调失效对象。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCoopGate : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopGate();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器返回权威状态，客户端返回最近一次复制状态；不应用网格当前位置反推逻辑状态。
	bool IsGateOpen() const { return bGateOpen; }

	// 对配置值和实际引用数量取安全范围，避免要求数量超过已配置压力板。
	int32 GetRequiredPlateCount() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_GateOpen();

private:
	// 仅服务器绑定外部压力板，客户端不重复执行规则组合。
	void BindRequiredPlates();
	// 与 BindRequiredPlates 对称，处理关卡卸载和 Actor 销毁。
	void UnbindRequiredPlates();
	// 统计激活板数和不同玩家数，并组合可选的钥匙目标前置条件。
	void EvaluateGateState();
	// 服务器写入与客户端 OnRep 的共同表现出口。
	void HandleGateStateChanged();
	// 初始加载时直接对齐目标；状态改变后只在过渡阶段启用 Tick。
	void ApplyGateState(bool bSnapToTarget);

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

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules", meta = (ClampMin = "1"))
	int32 RequiredActivePlateCount = 1;

	// 一次开启后保持打开；否则任一必要压力板释放时门会重新关闭。
	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bStayOpenOnceActivated = false;

	// 开启前还可以等待钥匙目标完成，从而复用为关卡末端机关。
	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bRequireObjectiveComplete = false;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Movement", meta = (ClampMin = "1.0"))
	float DoorMoveSpeed = 250.0f;

	// 只复制逻辑开关，不复制门网格的每帧位置。
	UPROPERTY(ReplicatedUsing = OnRep_GateOpen)
	bool bGateOpen = false;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
