// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"

namespace MultiplayerMeshMovement
{

enum class ESpace : uint8 { World, Relative };

// 状态机关的本地网格过渡；速度未同步时等待，到位后返回 true 供调用者停止 Tick。
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
	const auto SetLocation = [&Mesh, bWorldSpace](const FVector& Location)
	{
		if (bWorldSpace)
		{
			Mesh.SetWorldLocation(Location);
		}
		else
		{
			Mesh.SetRelativeLocation(Location);
		}
	};

	SetLocation(NewLocation);
	if (!NewLocation.Equals(TargetLocation, Tolerance))
	{
		return false;
	}

	SetLocation(TargetLocation);
	return true;
}

}
