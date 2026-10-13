// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Core/multiplayerGameplayConfig.h"

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
	// (*) CMC 在 PrePhysics 更新。基座移动不能晚于乘客，否则其自动 Tick 依赖会跳过本组件。
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

//tick组件在服务器上执行匀速插值移动，客户端位置由 ReplicateMovement 复制
void UmultiplayerTransporterComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	//移动规则只在服务器执行。客户端位置来自所属 Actor 的 ReplicateMovement，不能再计算第二份轨迹。
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		SetComponentTickEnabled(false);
		return;
	}

	//VInterpConstantTo 使用固定世界速度，不会因距离不同改变速度，便于关卡设计者预估到达时间。
	const FVector TargetLocation = GetTargetLocation();
	//****注意这个匀速插值函数！！！
	const FVector NewLocation = FMath::VInterpConstantTo(
		Owner->GetActorLocation(),
		TargetLocation,
		DeltaTime,
		MoveSpeed);

	//更新owner的位置，手动计算速度
	const FVector OldLocation = Owner->GetActorLocation();
	Owner->SetActorLocation(NewLocation, false);
	USceneComponent* Root = Owner->GetRootComponent();
	if (Root != nullptr)
	{

		//手动回填owner速度，避免乘客在 Tick 里读取到错误的零速度。
		Root->ComponentVelocity = DeltaTime > SMALL_NUMBER? (Owner->GetActorLocation() - OldLocation) / DeltaTime : FVector::ZeroVector;
	}
	// 到达容差内后贴合端点并结束本次运动；Tick 关闭后不再计算速度，避免乘客在 Tick 里读取到错误的零速度。
	if (Owner->GetActorLocation().Equals(TargetLocation, 0.5f))
	{
		Owner->SetActorLocation(TargetLocation, false);
		FinishMovement();
	}
}

//移动控制逻辑
void UmultiplayerTransporterComponent::SetTransportActive(bool bNewActive)
{
	//玩家及服务器校验
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	//防重入校验
	if (bTransportActive == bNewActive && bMoving)
	{
		//已在前往同一端点时不重复重启 Tick；同值但未移动的情况仍在下面检查实际位置。
		return;
	}

	//失活不自动返回
	if (!bReturnWhenInactive && !bNewActive)
	{
		//单程平台关闭返回功能后，失活事件不应把已经到达终点的平台拉回起点。
		return;
	}

	//记录新状态；如果已经位于目标点，FinishMovement 会直接关闭 Tick。
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
	Owner->ForceNetUpdate();
}

//初始化配置固定目标点
void UmultiplayerTransporterComponent::ConfigureWorldTargets(const FVector& InStartLocation,const FVector& InActiveLocation)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}
	//调用方在平台开始移动前传入固定世界坐标，避免附着的 Arrow 端点随 Owner 一起移动。
	StartLocation = InStartLocation;
	ActiveLocation = InActiveLocation;
	MoveSpeed = FmultiplayerGameplayConfig::Get(this).PlatformMoveSpeed;
}

//停止移动：最后更新速度为零，关闭 Tick，关闭状态，避免每帧重复比较位置
void UmultiplayerTransporterComponent::FinishMovement()
{
	AActor* Owner = GetOwner();
	USceneComponent* Root = Owner != nullptr ? Owner->GetRootComponent() : nullptr;
	//同步最终速度，更新状态，并关闭 Tick
	if (Root != nullptr)
	{
		Root->ComponentVelocity = FVector::ZeroVector;
		if (bMoving) Owner->ForceNetUpdate();
	}
	// 关闭 Tick 是静止机关最直接的性能收益；无需每帧反复比较已经相等的位置。
	bMoving = false;
	SetComponentTickEnabled(false);
}
