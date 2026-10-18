// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatMobility.h"

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/Tasks/AbilityTask_ApplyRootMotionConstantForce.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UmultiplayerDashCostEffect::UmultiplayerDashCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	FSetByCallerFloat Cost;
	Cost.DataTag = CombatTags::Data_Cost;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
}

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

UmultiplayerDashAbility::UmultiplayerDashAbility()
{
	Skill = EMultiplayerCombatSkill::Dash;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Dash));
	ActivationOwnedTags.AddTag(CombatTags::State_Dashing);
	ActivationBlockedTags.AddTag(CombatTags::State_Dashing);
	ActivationBlockedTags.AddTag(CombatTags::State_MovementBlocked);
	ActivationBlockedTags.AddTag(CombatTags::State_Rooted);
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Dash;
	CostGameplayEffectClass = UmultiplayerDashCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDashCooldownEffect::StaticClass();
}

bool UmultiplayerDashAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	return Character && Character->GetCharacterMovement()->MovementMode != MOVE_None
		&& Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

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
	const float Distance = FVector::Dist2D(Preview.Origin, Preview.End);
	auto* Motion = UAbilityTask_ApplyRootMotionConstantForce::ApplyRootMotionConstantForce(this, TEXT("CombatDash"),
		Direction, Distance / Rule.Duration, Rule.Duration, false, nullptr,
		ERootMotionFinishVelocityMode::ClampVelocity, FVector::ZeroVector, 0.f, true);
	Motion->OnFinish.AddDynamic(this, &ThisClass::FinishDash);
	Motion->ReadyForActivation();
}

void UmultiplayerDashAbility::FinishDash()
{
	// 结束与取消都沿用 GAS 销毁本轮任务；原生任务按自己的 source ID 清理，不影响其他位移来源。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}
