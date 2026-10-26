// Copyright Epic Games, Inc. All Rights Reserved.
// 预览/出手共用的几何查询接口；结果只描述查询时刻，不持有权威结算或资源状态。
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
	// 当前几何／目标是否合法；成本、冷却和施法者状态仍须通过能力准入，不保证未来命中。
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
	// 显式技能射程仍受弹体寿命限制；Range=0 的原生投射物使用完整寿命距离。
	float GetProjectileRange(const AmultiplayerCombatCharacter& Source);
	// 检查敌对存活玩家的近战距离、可选朝向与遮挡。
	bool CanMeleeHit(const AmultiplayerCombatCharacter& Source, const AmultiplayerCombatCharacter& Target, bool bCheckFacing);
	// 检查范围圆柱中的合法敌对目标及中心到目标的视线。
	bool CanAffectArea(const AmultiplayerCombatCharacter& Source, const AmultiplayerCombatCharacter& Target,
		const FVector& Center, float Radius);
	// 宽阶段只收集去重候选，不做遮挡射线；每个候选开始结算前仍须调用 CanAffectArea。
	TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> CollectAreaCandidates(
		const AmultiplayerCombatCharacter& Source, const FVector& Center, float Radius);
	// 预览需要当前完整命中集合，因此在候选上执行一次范围、阵营与遮挡过滤。
	TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> CollectAreaTargets(
		const AmultiplayerCombatCharacter& Source, const FVector& Center, float Radius);
}
