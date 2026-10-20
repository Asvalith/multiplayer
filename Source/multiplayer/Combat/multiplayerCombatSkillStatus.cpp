// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatCharacter.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatTags.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	FGameplayTag SkillCooldownTag(EMultiplayerCombatSkill Skill)
	{
		return CombatTags::GetSkillTags(Skill).Cooldown;
	}

	FGameplayTag SpecificFailure(const FGameplayTagContainer& Reasons)
	{
		const FGameplayTag FailureRoot = FGameplayTag::RequestGameplayTag(TEXT("Failure.Combat"));
		for (const FGameplayTag& Reason : Reasons)
			if (Reason.MatchesTag(FailureRoot)) return Reason;
		return {};
	}
}

// 会话、生命和学习状态属于角色业务；此处不重复实现 GAS 的标签、资源或冷却校验。
bool AmultiplayerCombatCharacter::CheckSkillBusinessRequirements(EMultiplayerCombatSkill Skill, FGameplayTagContainer* OutReasons) const
{
	const auto Reject = [OutReasons](FGameplayTag Reason)
	{
		if (OutReasons) OutReasons->AddTag(Reason);
		return false;
	};
	if (!bCombatSessionReady || !AbilitySystem) return Reject(CombatTags::Failure_NotReady);
	if ((Coop && Coop->IsDowned()) || AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Downed))
		return Reject(CombatTags::Failure_Downed);
	if (!IsAlive() || AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Dead)) return Reject(CombatTags::Failure_Dead);
	if ((Coop && Coop->IsRescuing()) || AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Rescuing))
		return Reject(CombatTags::Failure_Rescuing);
	const auto Class = GetSkillAbilityClass(Skill);
	if (!Class || (bGrantSupportAbilities && Progression->GetSkillLevel(Skill) <= 0))
		return Reject(CombatTags::Failure_NotLearned);
	const FGameplayAbilitySpec* Spec = AbilitySystem->FindAbilitySpecFromClass(Class);
	if (!Spec) return Reject(CombatTags::Failure_NotReady);
	if (Skill == EMultiplayerCombatSkill::Dash)
	{
		// MOVE_None 是实际 CMC 状态，不把非标签来源的移动锁遗漏在关系查询之外。
		if (GetCharacterMovement()->MovementMode == MOVE_None)
			return Reject(CombatTags::Failure_MovementBlocked);
	}
	if (Spec->IsActive())
		return Reject(CombatTags::Failure_AlreadyActive);
	// 不检查其他技能的 Active 标记：治疗/范围技能仍可按原规则取消普通攻击。
	return true;
}

// 预览只读查询真实能力，包括派生技能的特殊资格；正式激活和提交仍会重新校验当时状态。
bool AmultiplayerCombatCharacter::CheckSkillAvailable(EMultiplayerCombatSkill Skill, FGameplayTagContainer* OutReasons) const
{
	if (!AbilitySystem || !AbilitySystem->AbilityActorInfo.IsValid())
	{
		if (OutReasons) OutReasons->AddTag(CombatTags::Failure_NotReady);
		return false;
	}
	const auto Class = GetSkillAbilityClass(Skill);
	const FGameplayAbilitySpec* Spec = Class ? AbilitySystem->FindAbilitySpecFromClass(Class) : nullptr;
	if (!Spec || !Spec->Ability)
	{
		// 缺少 Spec 时仍区分未学习和连接未就绪，不把不可调用的能力当作普通冷却失败。
		if (CheckSkillBusinessRequirements(Skill, OutReasons) && OutReasons)
			OutReasons->AddTag(CombatTags::Failure_NotReady);
		return false;
	}
	// 与 InternalTryActivateAbility 一样优先使用已有实例，避免遗漏实例上的技能特有资格。
	const UGameplayAbility* Instance = Spec->GetPrimaryInstance();
	const UGameplayAbility* Ability = Instance ? Instance : Spec->Ability.Get();
	return Ability->CanActivateAbility(Spec->Handle, AbilitySystem->AbilityActorInfo.Get(), nullptr, nullptr, OutReasons);
}

// 时间与消耗实时读取 GAS/配置；不另存一套可能与预测撤销脱节的失败状态或倒计时。
FText AmultiplayerCombatCharacter::GetSkillFailureText(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue) const
{
	if (Reason == CombatTags::Failure_NotReady) return FText::FromString(TEXT("连接或技能尚未就绪，请稍候"));
	if (Reason == CombatTags::Failure_Dead) return FText::FromString(TEXT("死亡后无法释放技能"));
	if (Reason == CombatTags::Failure_Downed) return FText::FromString(TEXT("倒地中，等待队友救援"));
	if (Reason == CombatTags::Failure_Rescuing) return FText::FromString(TEXT("正在救援，松开 F 后再释放技能"));
	if (Reason == CombatTags::Failure_Stunned) return FText::FromString(TEXT("眩晕期间无法释放技能"));
	if (Reason == CombatTags::Failure_Casting) return FText::FromString(TEXT("本次未能释放：请求时正在施法（可右键取消）"));
	if (Reason == CombatTags::Failure_Dashing) return FText::FromString(TEXT("本次未能释放：请求时正在冲刺（可右键取消）"));
	if (Reason == CombatTags::Failure_AlreadyActive) return FText::FromString(TEXT("本次未能释放：请求时该技能已在生效，不能重复释放"));
	if (Reason == CombatTags::Failure_NotLearned) return FText::FromString(TEXT("技能尚未学习，按 B 打开技能菜单"));
	if (Reason == CombatTags::Failure_InsufficientEnergy)
		return FText::FromString(FString::Printf(TEXT("能量不足：需要 %.0f，当前 %.0f"), GetSkillRule(Skill).Cost,
			AuthorityValue >= 0 ? AuthorityValue : GetEnergy()));
	if (Reason == CombatTags::Failure_Cooldown)
	{
		const float Remaining = AuthorityValue >= 0 ? AuthorityValue : GetEffectRemaining(SkillCooldownTag(Skill));
		return FText::FromString(Remaining > 0.f ? FString::Printf(TEXT("技能冷却中：剩余 %.1f 秒"), Remaining) : TEXT("技能冷却中，等待服务器同步"));
	}
	if (Reason == CombatTags::Failure_NoHealTarget)
		return FText::FromString(GetHealth() >= GetMaxHealth()
			? TEXT("生命已满；附近没有可治疗的受伤队友")
			: TEXT("没有可治疗的受伤目标；倒地队友请按 F 救援"));
	if (Reason == CombatTags::Failure_InvalidGround) return FText::FromString(TEXT("没有有效地面落点，请调整瞄准位置"));
	if (Reason == CombatTags::Failure_DashBlocked) return FText::FromString(TEXT("前方空间不足，无法冲刺"));
	if (Reason == CombatTags::Failure_MovementBlocked) return FText::FromString(TEXT("当前移动受限，无法冲刺"));
	if (Reason == CombatTags::Failure_InvalidAim) return FText::FromString(TEXT("瞄准请求无效，请重新瞄准后释放"));
	if (Reason == CombatTags::Failure_MuzzleBlocked) return FText::FromString(TEXT("发射点被遮挡，请离开墙体"));
	if (Reason == CombatTags::Failure_Requirements) return FText::FromString(TEXT("当前状态不满足该技能的使用条件"));
	if (Reason == CombatTags::Failure_Conflict) return FText::FromString(TEXT("当前动作与该技能冲突，请结束当前动作后重试"));
	if (Reason == CombatTags::Failure_Rejected) return FText::FromString(TEXT("服务器未接受本次技能，预测状态已撤销"));
	return FText::GetEmpty();
}

FText AmultiplayerCombatCharacter::GetSkillFailureReason(EMultiplayerCombatSkill Skill) const
{
	FGameplayTagContainer Reasons;
	if (CheckSkillAvailable(Skill, &Reasons)) return FText::GetEmpty();
	return GetSkillFailureText(Skill, SpecificFailure(Reasons));
}

void AmultiplayerCombatCharacter::ReportSkillFailure(EMultiplayerCombatSkill Skill)
{
	if (!IsLocallyControlled() || GetNetMode() == NM_DedicatedServer) return;
	FText Message = GetSkillFailureReason(Skill);
	if (Message.IsEmpty()) Message = GetSkillFailureText(Skill, CombatTags::Failure_Rejected);
	OnCombatMessage.Broadcast(Message);
	AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
}

// 服务器指定的原因直接显示，不能在此重新预检尚未纠正的客户端预测状态而覆盖真正原因。
void AmultiplayerCombatCharacter::ClientSkillRejected_Implementation(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue)
{
	if (!IsLocallyControlled() || GetNetMode() == NM_DedicatedServer) return;
	OnCombatMessage.Broadcast(GetSkillFailureText(Skill, Reason, AuthorityValue));
	AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
}

void AmultiplayerCombatCharacter::HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons)
{
	if (!IsLocallyControlled() || GetNetMode() == NM_DedicatedServer) return;
	if (const auto* CombatAbility = Cast<UmultiplayerCombatAbility>(Ability))
	{
		const FGameplayTag Reason = SpecificFailure(Reasons);
		if (!Reason.IsValid()) { ReportSkillFailure(CombatAbility->GetCombatSkill()); return; }
		OnCombatMessage.Broadcast(GetSkillFailureText(CombatAbility->GetCombatSkill(), Reason));
		AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
	}
	else
	{
		OnCombatMessage.Broadcast(FText::FromString(TEXT("无法救援：需要靠近可见的倒地队友，并保持可行动状态")));
		AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
	}
}
