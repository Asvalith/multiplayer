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
 * (**) SetTransportActive 会忽略正在执行的同目标通知；若被外力移开或初始未对齐，则恢复移动。
 * (**) 当前机关采用关卡设计好的固定轨道，不用 Sweep 把乘客或装饰几何当成路径阻挡；
 * 到达判断仍读取移动后的 Actor 位置，不能拿尚未执行的计划位置判断。
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
	 * 同值调用不会重启正在进行或已经到达的运动，但会修正未位于目标点的异常位置。
	 */
	void SetTransportActive(bool bNewActive);

	/**
	 * 保存所属机关提供的固定世界坐标端点。
	 * (**) 端点组件会跟随平台一起移动；必须在移动前保存世界坐标，
	 * 如果每帧读取子组件位置，目标点也会移动，平台将永远追不上目标。
	 */
	void ConfigureWorldTargets(const FVector& InStartLocation, const FVector& InActiveLocation);

private:
	// 根据当前激活状态选择目标；端点由平台 BeginPlay 在开始移动前缓存为固定世界坐标。
	FVector GetTargetLocation() const;
	// 仅清除运动标记并关闭 Tick；移动与最终位置对齐由调用方负责。
	void FinishMovement();

	UPROPERTY(EditAnywhere, Category = "Coop|Transport", meta = (ClampMin = "1.0"))
	float MoveSpeed = 150.0f;

	// false 表示忽略取消激活，途中也继续前往终点；true 表示取消激活后返回起点。
	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	bool bReturnWhenInactive = true;

	FVector StartLocation = FVector::ZeroVector;
	FVector ActiveLocation = FVector::ZeroVector;
	// 前者选择期望端点，后者记录运动 Tick 是否进行中；二者都只是服务器运行期状态。
	bool bTransportActive = false;
	bool bMoving = false;
};
