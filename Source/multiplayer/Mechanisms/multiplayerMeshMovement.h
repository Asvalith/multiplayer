// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"

namespace MultiplayerMeshMovement
{

// 门使用固定世界端点，压力板使用相对按压偏移；只决定网格变换坐标系。
enum class ESpace : uint8 { World, Relative };

// 状态机关的本地网格过渡；速度未同步时等待，到位后返回 true 供调用者停止 Tick。
// 本工具不计算激活资格、不复制状态，也不替代承载玩家的平台运动。
inline bool MoveTo(USceneComponent& Mesh, const FVector& TargetLocation,
	float DeltaSeconds, float Speed, float Tolerance, ESpace Space)
{
	if (Speed <= 0.0f)
	{
		return false;
	}

	const bool bWorldSpace = Space == ESpace::World;
	const FVector CurrentLocation = bWorldSpace ? Mesh.GetComponentLocation() : Mesh.GetRelativeLocation();
	const FVector NewLocation = FMath::VInterpConstantTo(CurrentLocation, TargetLocation, DeltaSeconds, Speed);
	const bool bReachedTarget = NewLocation.Equals(TargetLocation, Tolerance);
	// 先确定最终位置，再提交一次变换，避免到位帧先移动、又贴合端点。
	const FVector FinalLocation = bReachedTarget ? TargetLocation : NewLocation;
	if (bWorldSpace)
	{
		Mesh.SetWorldLocation(FinalLocation);
	}
	else
	{
		Mesh.SetRelativeLocation(FinalLocation);
	}
	return bReachedTarget;
}

}
