// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatAbility.h"

#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "TimerManager.h"

// 项目 ASC 只扩展 GAS 集成：中央标签关系、失败反馈、预测清理和恢复所需的窄入口。
// 角色业务资格、具体技能步骤及属性结算分别保留在对应模块。
// 标签只描述资格；目标、距离、遮挡等实时业务仍由各能力在接纳/生效阶段分别复核。
bool UmultiplayerCombatAbilitySystem::CheckAbilityTagRelationships(const FGameplayTagContainer& AbilityTags,
	FGameplayTagContainer* Reasons) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActor());
	if (!Character) return true;
	FGameplayTagContainer Required, Blocked, Owned;
	Character->GetCombatConfig().GetAbilityActivationTagRequirements(AbilityTags, Required, Blocked);
	GetOwnedGameplayTags(Owned);
	const auto Reject = [Reasons](FGameplayTag Reason)
	{
		if (Reasons) Reasons->AddTag(Reason);
		return false;
	};
	const FGameplayTagContainer BlockingStates = Owned.Filter(Blocked);
	if (!BlockingStates.IsEmpty())
	{
		// 稳定优先级让本地预览与服务器拒绝说明一致；不另存一份状态 bool。
		const TPair<FGameplayTag, FGameplayTag> Failures[] = {
			{CombatTags::State_Downed, CombatTags::Failure_Downed}, {CombatTags::State_Dead, CombatTags::Failure_Dead},
			{CombatTags::State_Rescuing, CombatTags::Failure_Rescuing}, {CombatTags::State_Stunned, CombatTags::Failure_Stunned},
			{CombatTags::State_Casting, CombatTags::Failure_Casting}, {CombatTags::State_Dashing, CombatTags::Failure_Dashing},
			{CombatTags::State_Defending, CombatTags::Failure_AlreadyActive},
			{CombatTags::State_MovementBlocked, CombatTags::Failure_MovementBlocked}, {CombatTags::State_Rooted, CombatTags::Failure_MovementBlocked}};
		for (const auto& Failure : Failures)
			if (BlockingStates.HasTag(Failure.Key)) return Reject(Failure.Value);
		return Reject(CombatTags::Failure_Conflict);
	}
	if (!Owned.HasAll(Required)) return Reject(CombatTags::Failure_Requirements);
	return !AreAbilityTagsBlocked(AbilityTags) || Reject(CombatTags::Failure_Conflict);
}

// 借鉴 Lyra：把中央关系叠加到原生/蓝图参数，而非覆盖它们或自行维护活动能力列表。
void UmultiplayerCombatAbilitySystem::ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags,
	UGameplayAbility* RequestingAbility, bool bEnableBlockTags, const FGameplayTagContainer& BlockTags,
	bool bExecuteCancelTags, const FGameplayTagContainer& CancelTags)
{
	FGameplayTagContainer ExpandedBlock = BlockTags, ExpandedCancel = CancelTags;
	if (const auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActor()))
		Character->GetCombatConfig().GetAbilityBlockAndCancelTags(AbilityTags, ExpandedBlock, ExpandedCancel);
	Super::ApplyAbilityBlockAndCancelTags(AbilityTags, RequestingAbility, bEnableBlockTags, ExpandedBlock,
		bExecuteCancelTags, ExpandedCancel);
}

// 原生拒绝 RPC 只携带预测键；业务原因另发给同一拥有者，不把提示广播给旁观客户端。
void UmultiplayerCombatAbilitySystem::NotifyAbilityFailed(FGameplayAbilitySpecHandle Handle,
	UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	Super::NotifyAbilityFailed(Handle, Ability, FailureReason);
	auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActor());
	const auto* CombatAbility = Cast<UmultiplayerCombatAbility>(Ability);
	if (!Character || !CombatAbility || !IsOwnerActorAuthoritative() || Character->IsLocallyControlled()
		|| !Character->GetNetConnection()) return;
	FGameplayTag Reason = CombatTags::Failure_Rejected;
	for (const FGameplayTag& Tag : FailureReason)
	{
		if (Tag.MatchesTag(FGameplayTag::RequestGameplayTag(TEXT("Failure.Combat"))))
		{
			Reason = Tag;
			break;
		}
	}
	float AuthorityValue = -1.f;
	if (Reason == CombatTags::Failure_InsufficientEnergy) AuthorityValue = Character->GetEnergy();
	else if (Reason == CombatTags::Failure_Cooldown)
	{
		switch (CombatAbility->GetCombatSkill())
		{
		case EMultiplayerCombatSkill::Attack: AuthorityValue = Character->GetAttackCooldownRemaining(); break;
		case EMultiplayerCombatSkill::Defend: AuthorityValue = Character->GetDefenseCooldownRemaining(); break;
		case EMultiplayerCombatSkill::Heal: AuthorityValue = Character->GetHealCooldownRemaining(); break;
		case EMultiplayerCombatSkill::Area: AuthorityValue = Character->GetAreaCooldownRemaining(); break;
		case EMultiplayerCombatSkill::Dash: AuthorityValue = Character->GetDashCooldownRemaining(); break;
		}
	}
	Character->ClientSkillRejected(CombatAbility->GetCombatSkill(), Reason, AuthorityValue);
}

void UmultiplayerCombatAbilitySystem::InternalServerTryActivateAbility(FGameplayAbilitySpecHandle Handle,
	bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData)
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	const auto* CombatAbility = Spec ? Cast<UmultiplayerCombatAbility>(Spec->Ability) : nullptr;
	// 阻止远程调用普通激活 RPC 绕过瞄准数据检查；不改变其他技能的原生激活流程。
	if (!TriggerEventData && CombatAbility && CombatTags::GetSkillTags(CombatAbility->GetCombatSkill()).Event.IsValid())
	{
		if (auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActor()))
			Character->ClientSkillRejected(CombatAbility->GetCombatSkill(), CombatTags::Failure_InvalidAim);
		ClientActivateAbilityFailed(Handle, PredictionKey.Current);
		return;
	}
	Super::InternalServerTryActivateAbility(Handle, InputPressed, PredictionKey, TriggerEventData);
}

void UmultiplayerCombatAbilitySystem::EndCasting(FActiveGameplayEffectHandle ServerHandle, const FPredictionKey& ActivationKey)
{
	if (IsOwnerActorAuthoritative())
	{
		if (GetActiveGameplayEffect(ServerHandle)) RemoveActiveGameplayEffect(ServerHandle);
		return;
	}
	if (!AbilityActorInfo.IsValid() || !AbilityActorInfo->IsLocallyControlled() || !ActivationKey.IsLocalClientKey()) return;
	const auto Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::State_Casting));
	for (auto Handle : GetActiveEffects(Query))
	{
		const FActiveGameplayEffect* Effect = GetActiveGameplayEffect(Handle);
		// 确认接管后句柄会变化，但同一次施法的预测键不变。不得删除别次施法或任何冷却。
		if (Effect && Effect->PredictionKey == ActivationKey
			&& Effect->Spec.Def->GetClass() == UmultiplayerCastingEffect::StaticClass())
			RemoveActiveGameplayEffect_AllowClientRemoval(Handle);
	}
}

// 仅权威重连使用；修改真正持有的周期句柄，移除 GE 时仍能取消同一个定时器。
void UmultiplayerCombatAbilitySystem::SetPeriodicEffectDelay(FActiveGameplayEffectHandle Handle, float FirstDelay)
{
	if (!IsOwnerActorAuthoritative()) return;
	FActiveGameplayEffect* Effect = ActiveGameplayEffects.GetActiveGameplayEffect(Handle);
	if (!Effect || Effect->Spec.GetPeriod() <= 0.f) return;
	const FTimerDelegate Callback = FTimerDelegate::CreateWeakLambda(this, [this, Handle] { ExecutePeriodicEffect(Handle); });
	GetWorld()->GetTimerManager().SetTimer(Effect->PeriodHandle, Callback, Effect->Spec.GetPeriod(), true,
		FMath::Max(FirstDelay, UE_SMALL_NUMBER));
}
