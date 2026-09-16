// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"

/*
 * 固定轨道运动组件只生成服务器位置：激活选终点、失活可选返回起点，到位后停 Tick。
 * 网络发送由所属 Actor 负责；此处没有自行复制属性，也没有处理障碍绕行、挤压或客户端预测。
 */

/** 声明组件能执行 Tick，但把默认状态设为静止，等收到有效移动目标后才开启。 */
UmultiplayerTransporterComponent::UmultiplayerTransporterComponent()
{
	// 组件具备 Tick 能力，但初始关闭；只有目标端点发生变化且尚未到达时才开启。
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

/**
 * 仅在权威 Owner 上按固定速度更新位置；到达容差内后贴合端点并结束本次运动。
 * (**) 不做 Sweep，因此轨道避障由关卡布置保证，不能把网格的 BlockAll 当作平台自动防穿墙。
 */
void UmultiplayerTransporterComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 移动规则只在服务器执行。客户端位置来自所属 Actor 的 ReplicateMovement，不能再计算第二份轨迹。
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		SetComponentTickEnabled(false);
		return;
	}

	// VInterpConstantTo 使用固定世界速度，不会因距离不同改变速度，便于关卡设计者预估到达时间。
	const FVector TargetLocation = GetTargetLocation();
	const FVector NewLocation = FMath::VInterpConstantTo(
		Owner->GetActorLocation(),
		TargetLocation,
		DeltaTime,
		MoveSpeed);

	// 这是固定轨道机关，不使用 Sweep：乘客和轨道旁装饰物都不应改变平台的权威路径。
	// PlatformMesh 仍保留碰撞作为角色承载面；轨道是否穿过场景由关卡配置保证。
	Owner->SetActorLocation(NewLocation, false);

	if (Owner->GetActorLocation().Equals(TargetLocation, 0.5f))
	{
		Owner->SetActorLocation(TargetLocation, false);
		FinishMovement();
	}
}

/**
 * 服务器请求切换期望端点；运动中的同目标请求直接忽略，静止时检查位置是否需要恢复。
 * 单程配置忽略所有取消激活请求，包括尚在途中时的失活；本函数不会立即传送到新端点。
 */
void UmultiplayerTransporterComponent::SetTransportActive(bool bNewActive)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (bTransportActive == bNewActive && bMoving)
	{
		// 已在前往同一端点时不重复重启 Tick；同值但未移动的情况仍在下面检查实际位置。
		return;
	}

	if (!bReturnWhenInactive && !bNewActive)
	{
		// 单程平台关闭返回功能后，失活事件不应把已经到达终点的平台拉回起点。
		return;
	}

	// 这里保存的是“期望端点”而不是网络状态；服务器移动后的 Transform 才由所属 Actor 复制。
	bTransportActive = bNewActive;
	if (Owner->GetActorLocation().Equals(GetTargetLocation(), 0.5f))
	{
		// 同值通知只有在真实位于目标点时才是空操作；未对齐时仍会恢复移动。
		FinishMovement();
		return;
	}

	// 只有尚未位于目标点时才开启 Tick；已经到达的重复通知会在上面直接 FinishMovement。
	bMoving = true;
	SetComponentTickEnabled(true);
}

/** 保存两个固定世界坐标；仅设置目标数据，不立即启动移动或改变 Owner 位置。 */
void UmultiplayerTransporterComponent::ConfigureWorldTargets(
	const FVector& InStartLocation,
	const FVector& InActiveLocation)
{
	// 调用方在平台开始移动前传入固定世界坐标，避免附着的 Arrow 端点随 Owner 一起移动。
	StartLocation = InStartLocation;
	ActiveLocation = InActiveLocation;
}

/** 根据期望激活状态选起点或终点；不读取跟随平台移动的端点组件。 */
FVector UmultiplayerTransporterComponent::GetTargetLocation() const
{
	if (!bTransportActive)
	{
		return StartLocation;
	}

	return ActiveLocation;
}

/** 只结束运动调度，不修改位置；之后新的激活通知仍可重新开启 Tick。 */
void UmultiplayerTransporterComponent::FinishMovement()
{
	// 关闭 Tick 是静止机关最直接的性能收益；无需每帧反复比较已经相等的位置。
	bMoving = false;
	SetComponentTickEnabled(false);
}
