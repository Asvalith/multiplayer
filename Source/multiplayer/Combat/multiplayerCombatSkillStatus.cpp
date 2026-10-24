// Copyright Epic Games, Inc. All Rights Reserved.

// 技能资格与失败说明：只读查询实际能力，角色仅补充接纳、生命和学习前提。
// 正式激活仍走 GAS；失败提示使用本次真实原因，不再重复推断成本或冷却。

#include "Combat/multiplayerCombatCharacter.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatTags.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	// 将技能种类映射到集中定义的冷却标签，供提示读取剩余时间。
	FGameplayTag SkillCooldownTag(EMultiplayerCombatSkill Skill)
	{
		return CombatTags::GetSkillTags(Skill).Cooldown;
	}

	// 优先取项目可读的拒绝标签；原生未知原因由调用者使用通用说明兜底。
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
	// PreActivate 先标记实例活动并广播 owned tag，最后才增加 Spec.ActiveCount；两者均须拒绝复入。
	// 同线程标签通知可能立即再次请求技能；只防同能力重复，不把本可替换的其他技能一并拒绝。
	const UGameplayAbility* Instance = Spec->GetPrimaryInstance();
	if (Spec->IsActive() || (Instance && Instance->IsActive()))
		return Reject(CombatTags::Failure_AlreadyActive);
	// 不检查其他技能的 Active 标记：治疗/范围技能仍可按原规则取消普通攻击。
	return true;
}

// 预览与正式激活共用真实 CanActivate；提交只重查成本/冷却，目标及角色资格在出手时另验。
// 查询没有副作用，只说明本地此刻状态；服务器仍独立检查，预检成功不等于已接纳或已命中。
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

// 供 HUD 主动查询不可用原因；可用时返回空文本，不产生提示或效果。
FText AmultiplayerCombatCharacter::GetSkillFailureReason(EMultiplayerCombatSkill Skill) const
{
	FGameplayTagContainer Reasons;
	if (CheckSkillAvailable(Skill, &Reasons)) return FText::GetEmpty();
	const FGameplayTag Reason = SpecificFailure(Reasons);
	// 原生输入限制或派生条件未提供项目标签时，也必须提示“不满足条件”，不能显示成可用。
	return GetSkillFailureText(Skill, Reason.IsValid() ? Reason : CombatTags::Failure_Requirements);
}

// 本地显示已有拒绝结果并播放失败 Cue；只有没有结果时才主动查询资格。
void AmultiplayerCombatCharacter::PresentSkillFailure(const FText& Message)
{
	if (!IsLocallyControlled() || GetNetMode() == NM_DedicatedServer) return;
	OnCombatMessage.Broadcast(Message);
	AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
}

// 原因选择留在原入口；统一出口不重查资格或覆盖服务器已经裁决的失败原因。
void AmultiplayerCombatCharacter::ReportSkillFailure(EMultiplayerCombatSkill Skill, const FGameplayTagContainer* Reasons)
{
	if (!IsLocallyControlled() || GetNetMode() == NM_DedicatedServer) return;
	const FGameplayTag Reason = Reasons ? SpecificFailure(*Reasons) : FGameplayTag();
	FText Message = Reasons ? GetSkillFailureText(Skill, Reason) : GetSkillFailureReason(Skill);
	if (Message.IsEmpty()) Message = GetSkillFailureText(Skill, CombatTags::Failure_Requirements);
	PresentSkillFailure(Message);
}

// 服务器指定的原因直接显示，不能在此重新预检尚未纠正的客户端预测状态而覆盖真正原因。
// 当前提示按技能反馈，未携带请求编号；动作/效果的 Key 隔离不等于旧提示也能逐次精确过滤。
void AmultiplayerCombatCharacter::ClientSkillRejected_Implementation(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue)
{
	PresentSkillFailure(GetSkillFailureText(Skill, Reason, AuthorityValue));
}

// 接收 GAS 正式激活失败，区分战斗技能与救援提示，不再次激活或修改资源。
void AmultiplayerCombatCharacter::HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons)
{
	if (const auto* CombatAbility = Cast<UmultiplayerCombatAbility>(Ability))
	{
		ReportSkillFailure(CombatAbility->GetCombatSkill(), &Reasons);
	}
	else
	{
		PresentSkillFailure(FText::FromString(TEXT("无法救援：需要靠近可见的倒地队友，并保持可行动状态")));
	}
}
