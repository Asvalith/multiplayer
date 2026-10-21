// Copyright Epic Games, Inc. All Rights Reserved.

// 实现项目 ASC 的标签关系扩展、正式失败反馈、动画预测及重连所需窄入口。
// 不代管输入或技能步骤；生命周期、复制和效果账本仍以 GAS 为主。

#include "Combat/multiplayerCombatAbility.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "TimerManager.h"

// 标签只描述资格；目标、距离、遮挡等实时业务仍由各能力在接纳/生效阶段分别复核。
// 此处集中拥有者的能力关系，不是“全局规则、攻击方、受击方”任意组合的通用规则解析器。
bool UmultiplayerCombatAbilitySystem::CheckAbilityTagRelationships(const FGameplayTagContainer& AbilityTags,
	FGameplayTagContainer* Reasons) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActor());
	if (!Character) return true;
	FGameplayTagContainer Required, Blocked, Owned;
	Character->GetCombatConfig().GetAbilityActivationTagRequirements(AbilityTags, Required, Blocked);
	GetOwnedGameplayTags(Owned);
	// 资格查询只累积稳定失败原因，不在预检中取消技能或修改任何标签。
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

// 拦住缺失瞄准事件的远程定向激活，再让原生流程校验能力、预测键及正式激活资格。
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

// 保留 UE5.5 PlayMontage 的播放、复制和预测流程，只在认领动作前封住同步通知窗口。
// 不能先调用 Super 再恢复记录：旧调用已绑定的预测拒绝回调仍可能误停后来的同资产动作。
// 这是针对 5.5 Started 顺序的窄适配，升级引擎需重核；不代表所有动画回调或任意动作都可回滚。
float UmultiplayerCombatAbilitySystem::PlayMontage(UGameplayAbility* AnimatingAbility,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* Montage, float InPlayRate,
	FName StartSectionName, float StartTimeSeconds)
{
	UAnimInstance* Animation = AbilityActorInfo.IsValid() ? AbilityActorInfo->GetAnimInstance() : nullptr;
	if (!Animation || !Montage) return -1.f;
	const uint32 PreviousRevision = MontagePlayRevision;
	const uint32 Revision = ++MontagePlayRevision;
	const float Duration = Animation->Montage_Play(Montage, InPlayRate, EMontagePlayReturnType::MontageLength, StartTimeSeconds);
	if (Duration <= 0.f)
	{
		// 没有播放成功的嵌套请求不能抢走外层动作。
		if (MontagePlayRevision == Revision) MontagePlayRevision = PreviousRevision;
		return Duration;
	}
	// Started 内可同步取消并启动另一技能；较新的播放已经负责自己的归属，旧栈不得再覆盖。
	if (MontagePlayRevision != Revision) return -1.f;
	if (AnimatingAbility && (!AnimatingAbility->IsActive()
		|| AnimatingAbility->GetCurrentActivationInfo().GetActivationPredictionKey() != ActivationInfo.GetActivationPredictionKey()))
	{
		Animation->Montage_Stop(.15f, Montage);
		return -1.f;
	}
	LocalAnimMontageInfo.AnimMontage = Montage;
	LocalAnimMontageInfo.AnimatingAbility = AnimatingAbility;
	LocalAnimMontageInfo.PlayInstanceId = LocalAnimMontageInfo.PlayInstanceId < UINT8_MAX ? LocalAnimMontageInfo.PlayInstanceId + 1 : 0;
	if (AnimatingAbility) AnimatingAbility->SetCurrentMontage(Montage);
	if (StartSectionName != NAME_None) Animation->Montage_JumpToSection(StartSectionName, Montage);
	if (ShouldRecordMontageReplication())
	{
		auto& Replicated = GetRepAnimMontageInfo_Mutable();
		Replicated.Animation = Montage->IsDynamicMontage() ? Montage->GetFirstAnimReference() : Montage;
		Replicated.PlayInstanceId = Replicated.PlayInstanceId < UINT8_MAX ? Replicated.PlayInstanceId + 1 : 0;
		Replicated.SectionIdToPlay = StartSectionName != NAME_None ? Montage->GetSectionIndex(StartSectionName) + 1 : 0;
		if (Montage->IsDynamicMontage())
		{
			check(!Montage->SlotAnimTracks.IsEmpty());
			Replicated.SlotName = Montage->SlotAnimTracks[0].SlotName;
			Replicated.BlendOutTime = Montage->GetDefaultBlendInTime();
		}
		AnimMontage_UpdateReplicatedData();
	}
	if (IsOwnerActorAuthoritative())
	{
		if (AActor* Avatar = GetAvatarActor()) Avatar->ForceNetUpdate();
	}
	else
	{
		FPredictionKey Key = GetPredictionKeyForNewAction();
		if (Key.IsValidKey())
			Key.NewRejectedDelegate().BindWeakLambda(this, [this, Revision]
			{
				// 资产可能被下一轮复用，旧预测拒绝只能结束自己的播放请求。
				if (MontagePlayRevision == Revision) CurrentMontageStop(.25f);
			});
	}
	return Duration;
}

// 权威端按句柄移除；拥有者仅移除匹配本次预测键的施法 GE，不借此开放任意客户端删效果。
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
	// 弱回调只推进该效果句柄，效果移除后的清理仍使用 GAS 持有的同一计时器。
	const FTimerDelegate Callback = FTimerDelegate::CreateWeakLambda(this, [this, Handle] { ExecutePeriodicEffect(Handle); });
	GetWorld()->GetTimerManager().SetTimer(Effect->PeriodHandle, Callback, Effect->Spec.GetPeriod(), true,
		FMath::Max(FirstDelay, UE_SMALL_NUMBER));
}
