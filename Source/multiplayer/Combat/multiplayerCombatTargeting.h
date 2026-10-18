// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Combat/multiplayerCombatConfig.h"

class AmultiplayerCombatCharacter;

// 本地预览与服务器检测共用的查询结果，不复制预测目标，也不把客户端目标当命中结果。
struct FmultiplayerCombatTargetPreview
{
	EMultiplayerCombatSkill Skill = EMultiplayerCombatSkill::Attack;
	FVector Origin = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	float Radius = 0.f;
	bool bValid = false;
	TWeakObjectPtr<AmultiplayerCombatCharacter> Target;
	TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> AffectedTargets;
};

namespace CombatTargeting
{
	// 参数与弹体/CMC使用同一份几何，不用特效尺寸反推伤害。
	constexpr float ProjectileRadius = 12.f;
	constexpr float ProjectileLifetime = 3.f;
	constexpr float AreaHalfHeight = 200.f;
	constexpr float MeleeFrontDot = .5f;
	bool CanMeleeHit(const AmultiplayerCombatCharacter& Source, const AmultiplayerCombatCharacter& Target, bool bCheckFacing);
	bool CanAffectArea(const AmultiplayerCombatCharacter& Source, const AmultiplayerCombatCharacter& Target,
		const FVector& Center, float Radius);
	TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> CollectAreaTargets(
		const AmultiplayerCombatCharacter& Source, const FVector& Center, float Radius);
}
