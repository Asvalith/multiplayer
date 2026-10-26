// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "Combat/multiplayerCombatProgression.h"

// [职责] GameMode 持有的同局个人恢复数据，断线后恢复期限为 120 秒，由服务器 Character 捕获和还原。
// [边界] 不拥有角色、不复制给客户端，也不是长期存档、通用回滚或序列化协议。
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
	// 只为灼烧与定身保留原 Spec；其他支持状态由上方标量重建，离线时间继续扣除。
	// 不保存能力任务、施法锁、位移源、救援占用或半次出手；接纳后从稳定状态继续。
	TArray<FmultiplayerCombatResumeEffect> PersistentEffects;
	bool bDead = false;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};
