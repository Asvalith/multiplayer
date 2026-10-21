// Copyright Epic Games, Inc. All Rights Reserved.
// 冲刺沿用技能的预测、扣费和冷却流程，移动实现交给原生能力任务。
// 预览可达距离与本次位移共用一次查询；取消后不再向 CMC 添加推进源。

#include "Combat/multiplayerCombatMobility.h"

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/Tasks/AbilityTask_ApplyRootMotionConstantForce.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

// 只定义能量修改通道，负成本由通用提交入口通过 SetByCaller 写入。
UmultiplayerDashCostEffect::UmultiplayerDashCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	FSetByCallerFloat Cost;
	Cost.DataTag = CombatTags::Data_Cost;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
}

// 用持续 GE 持有冲刺冷却标签，实际时长由本次技能规则覆盖。
UmultiplayerDashCooldownEffect::UmultiplayerDashCooldownEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(3.f));
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("DashCooldownTags"));
	GEComponents.Add(Tags);
	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(CombatTags::Cooldown_Dash);
	Tags->SetAndApplyTargetTagChanges(GrantedTags);
}

// 只配置冲刺特有事件和效果，预测策略及准入规则继续由战斗基类提供。
UmultiplayerDashAbility::UmultiplayerDashAbility()
{
	Skill = EMultiplayerCombatSkill::Dash;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Dash));
	ActivationOwnedTags.AddTag(CombatTags::State_Dashing);
	// 只拥有本次冲刺标签；禁行/重复激活关系读取角色配置，结束仍由原生任务清位移源。
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Dash;
	CostGameplayEffectClass = UmultiplayerDashCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDashCooldownEffect::StaticClass();
}

// 本轮输入、胶囊预览和提交均有效后创建原生位移任务；版本号隔离同步取消后的旧调用栈。
void UmultiplayerDashAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	const FRotator Aim = TriggerEventData
		? TriggerEventData->TargetData.Get(0)->GetEndPointTransform().Rotator()
		: Character->GetControlRotation();
	// 抬头、低头只影响镜头；冲刺只采用接纳本次请求时的水平朝向，不采用载荷位置。
	const FVector Direction = FRotator(0.f, Aim.Yaw, 0.f).Vector();
	const auto Preview = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Dash, &Aim);
	if (!Preview.bValid)
	{
		if (ActorInfo->IsNetAuthority()) Character->ClientSkillRejected(Skill, CombatTags::Failure_DashBlocked);
		else Character->OnCombatMessage.Broadcast(Character->GetSkillFailureText(Skill, CombatTags::Failure_DashBlocked));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	// 扣费与冷却通知可同步导致控制状态变化；旧激活栈不能在取消之后重新施加位移。
	if (!bCommitted || !IsActive() || !IsValid(Character) || !Character->IsCombatReady() || Character->IsMovementBlocked())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const FmultiplayerCombatSkillRule& Rule = GetRule();
	Character->SetActorRotation(FRotator(0, Aim.Yaw, 0));
	if (Revision != ActivationRevision || !IsActive()) return;
	// 沿预览同一次胶囊扫掠的可达距离推进，静态斜墙前也不持续推入再滑向另一终点。
	// 位移交给原生 RootMotion/CMC，取消由任务清掉自己的推进源；不另做坐标快照回滚。
	const float Distance = FVector::Dist2D(Preview.Origin, Preview.End);
	auto* Motion = UAbilityTask_ApplyRootMotionConstantForce::ApplyRootMotionConstantForce(this, TEXT("CombatDash"),
		Direction, Distance / Rule.Duration, Rule.Duration, false, nullptr,
		ERootMotionFinishVelocityMode::ClampVelocity, FVector::ZeroVector, 0.f, true);
	Motion->OnFinish.AddDynamic(this, &ThisClass::FinishDash);
	Motion->ReadyForActivation();
}

// 自然结束交还 GAS 生命周期；客户端不自行纠正坐标或返还能量。
void UmultiplayerDashAbility::FinishDash()
{
	// 结束与取消都沿用 GAS 销毁本轮任务；原生任务按自己的 source ID 清理，不影响其他位移来源。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}
