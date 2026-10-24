// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "Combat/multiplayerCombatProgression.h"

// 服务器同局短时恢复的数据合同，不拥有角色，不复制到客户端，也不是通用存档或序列化协议。
/** 白名单持续效果的服务器快照；Spec 保留原来源，周期额外保存距下一跳的真实时间。 */
struct FmultiplayerCombatResumeEffect
{
	FGameplayEffectSpecHandle Spec;
	float Remaining = 0.f;
	float NextTickDelay = 0.f;
	float RawDamagePerTick = 0.f;
};

/** 仅服务器保存的短时重连快照，不从客户端接收，也不复制整份历史状态。 */
struct FmultiplayerCombatResumeState
{
	float Health = 100.0f;
	float Energy = 100.0f;
	float Shield = 0.0f;
	float AttackCooldown = 0.0f;
	float DefenseCooldown = 0.0f;
	float HealCooldown = 0.0f;
	float DefenseRemaining = 0.0f;
	float AreaCooldown = 0.0f;
	float StunRemaining = 0.0f;
	float DashCooldown = 0.f;
	bool bDowned = false;
	float DownedRemaining = 0.f;
	FmultiplayerCombatProgressionState Progression;
	// 只保存灼烧与定身，不恢复正在执行的能力、施法锁或任意第三方效果。
	TArray<FmultiplayerCombatResumeEffect> PersistentEffects;
	bool bDead = false;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};
