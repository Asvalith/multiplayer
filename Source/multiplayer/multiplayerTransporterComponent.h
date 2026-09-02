// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerTransporterComponent.generated.h"

/**
 * 可复用的服务器移动组件。所属 Actor 负责复制位置，本组件只负责往返规则，
 * 到达任一端点后停止 Tick，避免静止时持续计算。
 *
 * 组件刻意不复制 bTransportActive：它只在服务器计算 Actor 的真实 Transform，网络层随后通过
 * 所属平台的 ReplicateMovement 把结果发送给客户端。这样移动算法和网络策略互不耦合，组件也能
 * 复用于其他由服务器驱动、但采用不同复制方式的机关。
 *
 * (*) ActorComponent 的 Tick 不会自动获得网络权威，仍要检查 Owner->HasAuthority()。
 * (**) SetTransportActive 只有目标状态真正变化时才重新启用 Tick，避免 Delegate 重复通知造成空转。
 */
UCLASS(ClassGroup = (Coop), meta = (BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerTransporterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerTransporterComponent();

	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * 设置期望端点。激活时前往 ActiveLocation；取消激活且允许返回时前往 StartLocation。
	 * 同值调用不会重新启动已经完成的运动。
	 */
	void SetTransportActive(bool bNewActive);

	/**
	 * 保存所属机关提供的固定世界坐标端点。
	 * (**) 端点组件会跟随平台一起移动；必须在移动前保存世界坐标，
	 * 如果每帧读取子组件位置，目标点也会移动，平台将永远追不上目标。
	 */
	void ConfigureWorldTargets(const FVector& InStartLocation, const FVector& InActiveLocation);

private:
	// 根据当前激活状态选择目标；端点均为 BeginPlay 前缓存的固定世界坐标。
	FVector GetTargetLocation() const;
	// 到达容差内后精确对齐目标并关闭 Tick，消除浮点尾差和长期空转。
	void FinishMovement();

	UPROPERTY(EditAnywhere, Category = "Coop|Transport", meta = (ClampMin = "1.0"))
	float MoveSpeed = 150.0f;

	// false 表示失活后停在终点，true 表示失活后返回起点。
	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	bool bReturnWhenInactive = true;

	FVector StartLocation = FVector::ZeroVector;
	FVector ActiveLocation = FVector::ZeroVector;
	bool bTransportActive = false;
	bool bMoving = false;
};
