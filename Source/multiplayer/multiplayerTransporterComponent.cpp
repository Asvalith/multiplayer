// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"

UmultiplayerTransporterComponent::UmultiplayerTransporterComponent()
{
	// 组件具备 Tick 能力，但初始关闭；只有目标端点发生变化且尚未到达时才开启。
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

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

	// Sweep 保留为 true，让平台移动时参与碰撞检测，而不是直接穿过阻挡物。
	Owner->SetActorLocation(NewLocation, true);
	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		Owner->SetActorLocation(TargetLocation, true);
		FinishMovement();
	}
}

void UmultiplayerTransporterComponent::SetTransportActive(bool bNewActive)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
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
		FinishMovement();
		return;
	}

	// 只有尚未位于目标点时才开启 Tick；已经到达的重复通知会在上面直接 FinishMovement。
	bMoving = true;
	SetComponentTickEnabled(true);
}

void UmultiplayerTransporterComponent::ConfigureWorldTargets(
	const FVector& InStartLocation,
	const FVector& InActiveLocation)
{
	// 调用方在平台开始移动前传入固定世界坐标，避免附着的 Arrow 端点随 Owner 一起移动。
	StartLocation = InStartLocation;
	ActiveLocation = InActiveLocation;
}

FVector UmultiplayerTransporterComponent::GetTargetLocation() const
{
	if (!bTransportActive)
	{
		return StartLocation;
	}

	return ActiveLocation;
}

void UmultiplayerTransporterComponent::FinishMovement()
{
	// 关闭 Tick 是静止机关最直接的性能收益；无需每帧反复比较已经相等的位置。
	bMoving = false;
	SetComponentTickEnabled(false);
}
