// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Core/multiplayerGameplayConfig.h"

/*
 * 固定轨道运动组件只生成服务器位置：激活选终点、失活可选返回起点，到位后停 Tick。
 * 网络发送由所属 Actor 负责；此处没有自行复制属性，也没有处理障碍绕行、挤压或客户端预测。
 */

// 默认静止；收到有效目标且尚未到达时才开启 PrePhysics 移动。
UmultiplayerTransporterComponent::UmultiplayerTransporterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	// (*) CMC 在 PrePhysics 更新。基座移动不能晚于乘客，否则其自动 Tick 依赖会跳过本组件。
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

// 服务器沿固定轨道匀速移动并回填基座速度，客户端只消费所属 Actor 的运动复制。
// 设计：平台是 CMC 的移动基座，不能像门一样只复制开关；实际位移/DeltaTime 才是继承速度。
// 轨迹和位置复制交给服务器与原生 Actor/CMC，不另建客户端平台预测或历史位置回滚。
void UmultiplayerTransporterComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 客户端不能再生成第二条轨迹，与服务器复制位置互相覆盖。
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		SetComponentTickEnabled(false);
		return;
	}

	// 固定世界速度不随剩余距离改变，关卡可据此预估到达时间。
	const FVector TargetLocation = GetTargetLocation();
	const FVector NewLocation = FMath::VInterpConstantTo(
		Owner->GetActorLocation(),
		TargetLocation,
		DeltaTime,
		MoveSpeed);

	const FVector OldLocation = Owner->GetActorLocation();
	Owner->SetActorLocation(NewLocation, false);
	USceneComponent* Root = Owner->GetRootComponent();
	if (Root != nullptr)
	{
		// 非物理根组件需手动回填真实位移速度，供乘客 CMC 正确继承。
		Root->ComponentVelocity = DeltaTime > SMALL_NUMBER? (Owner->GetActorLocation() - OldLocation) / DeltaTime : FVector::ZeroVector;
	}
	// 用实际移动后的坐标判断到位，再贴合端点并清零速度；不能只检查计划位置。
	if (Owner->GetActorLocation().Equals(TargetLocation, 0.5f))
	{
		Owner->SetActorLocation(TargetLocation, false);
		FinishMovement();
	}
}

// 服务器提交期望端点：过滤重复在途目标，遵守单程配置，未对齐时恢复运动。
void UmultiplayerTransporterComponent::SetTransportActive(bool bNewActive)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	// 同目标在途通知不重启 Tick；静止时仍需复核实际位置。
	if (bTransportActive == bNewActive && bMoving)
	{
		return;
	}

	// 单程模式忽略失活，包括途中取消和到达后的释放。
	if (!bReturnWhenInactive && !bNewActive)
	{
		return;
	}

	// 先提交新端点选择，再复核位置；即使同值，外力移开后也能恢复对齐。
	bTransportActive = bNewActive;
	if (Owner->GetActorLocation().Equals(GetTargetLocation(), 0.5f))
	{
		FinishMovement();
		return;
	}

	// 未到位才开启移动，并请求尽快发送当前权威位置。
	bMoving = true;
	SetComponentTickEnabled(true);
	Owner->ForceNetUpdate();
}

// 服务器在首次运动前缓存固定世界端点与配置速度，不在 Tick 中重复查配置。
void UmultiplayerTransporterComponent::ConfigureWorldTargets(const FVector& InStartLocation,const FVector& InActiveLocation)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}
	// 固定世界坐标不能继续追读附着 Arrow，否则终点会随 Owner 移动。
	StartLocation = InStartLocation;
	ActiveLocation = InActiveLocation;
	MoveSpeed = FmultiplayerGameplayConfig::Get(this).PlatformMoveSpeed;
}

// 结束运动时先提交零基座速度，再关闭运动标记与 Tick；最终位置由调用者贴合。
void UmultiplayerTransporterComponent::FinishMovement()
{
	AActor* Owner = GetOwner();
	USceneComponent* Root = Owner != nullptr ? Owner->GetRootComponent() : nullptr;
	// 先清零并请求最后一帧复制，乘客不能在平台停止后继续继承旧速度。
	if (Root != nullptr)
	{
		Root->ComponentVelocity = FVector::ZeroVector;
		if (bMoving) Owner->ForceNetUpdate();
	}
	bMoving = false;
	SetComponentTickEnabled(false);
}
