// Copyright Epic Games, Inc. All Rights Reserved.

// 实现技能公共准入／提交和攻击、防御、治疗、范围技能的阶段流程。
// GAS 管生命周期与预测，服务器管目标和结果；这里不处理输入、HUD 或机关规则。

#include "Combat/multiplayerCombatAbility.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "AIController.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayCueManager.h"

namespace
{
	// 这些结果 Cue 没有在客户端预测播放，不能继承激活键后被拥有者当作重复预测而丢弃。
	void ExecuteAuthorityResultCue(UAbilitySystemComponent* TargetASC, FGameplayTag Tag, const FGameplayCueParameters& Parameters)
	{
		UAbilitySystemGlobals::Get().GetGameplayCueManager()->InvokeGameplayCueExecuted_WithParams(
			TargetASC, Tag, FPredictionKey(), Parameters);
	}

	// 使用 UE5.5 效果组件授予限时标签，避免继续写入已弃用的旧标签容器。
	void ConfigureTimedTagEffect(UGameplayEffect& Effect, UTargetTagsGameplayEffectComponent& TagComponent,
		float Duration, FGameplayTag GrantedTag)
	{
		Effect.DurationPolicy = EGameplayEffectDurationType::HasDuration;
		Effect.DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
		FInheritedTagContainer Tags;
		Tags.AddTag(GrantedTag);
		TagComponent.SetAndApplyTargetTagChanges(Tags);
	}
	// 目标属于每个 AI 的黑板；施法锁会清空黑板，因此起手前只取一次目标快照。
	AmultiplayerCombatCharacter* GetMeleeTarget(const AmultiplayerCombatEnemy* Enemy)
	{
		const AAIController* AI = Enemy ? Cast<AAIController>(Enemy->GetController()) : nullptr;
		const UBlackboardComponent* Blackboard = AI ? AI->GetBlackboardComponent() : nullptr;
		return Blackboard ? Cast<AmultiplayerCombatCharacter>(Blackboard->GetValueAsObject(CombatAIKeys::TargetActor)) : nullptr;
	}
	// 接纳时检查距离与遮挡，出手时再检查正前方 120 度；目标可以在起手期间躲开。
	bool CanReachMeleeTarget(const AmultiplayerCombatEnemy* Enemy, const AmultiplayerCombatCharacter* Target, bool bCheckFacing)
	{
		const AAIController* AI = Enemy ? Cast<AAIController>(Enemy->GetController()) : nullptr;
		return AI && IsValid(Target) && CombatTargeting::CanMeleeHit(*Enemy, *Target, bCheckFacing);
	}
}

// 预测客户端的消耗、冷却和表现；伤害、发弹及治疗结果仍只在服务器提交。
// 取舍：复用 GAS 预测账本，不另写资源回滚；LocalPredicted 不代表任意 C++ 副作用都能自动撤销。
// InstancedPerActor 会复用对象，后续必须区分“同一个实例”和“同一轮激活”。
UmultiplayerCombatAbility::UmultiplayerCombatAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	// 共用状态要求由角色实际 CombatConfig 提供，不在每个能力 CDO 再复制一份。
}

// 从角色读取结合等级后的有效数值，调用点决定是即时读取还是固定本次局部规则。
FmultiplayerCombatSkillRule UmultiplayerCombatAbility::GetRule() const
{
	return CastChecked<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo())->GetSkillRule(Skill);
}

// 准入与提交复核都读取当前能量；只产生失败标签，不提前扣费。
bool UmultiplayerCombatAbility::CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	// 原生 CanActivateAbility/CommitCheck 负责 IgnoreCosts；这里仅校验当前有效规则。
	const bool bEnough = Character && Character->GetEnergy() >= Character->GetSkillRule(Skill).Cost;
	if (!bEnough && OptionalRelevantTags) OptionalRelevantTags->AddTag(CombatTags::Failure_InsufficientEnergy);
	return bEnough;
}

// 沿用原生冷却标签查询，将拒绝映射成项目的冷却提示。
bool UmultiplayerCombatAbility::CheckCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const bool bReady = Super::CheckCooldown(Handle, ActorInfo, OptionalRelevantTags);
	// 保留原生冷却标签查询，只把真实失败映射为项目提示，不在角色里再维护一套判断。
	if (!bReady && OptionalRelevantTags) OptionalRelevantTags->AddTag(CombatTags::Failure_Cooldown);
	return bReady;
}

// 支持 GAS 单独提交成本的入口；完整 Commit 的成本／冷却快照由 CommitExecute 统一处理。
void UmultiplayerCombatAbility::ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character) ApplyRuleCost(Handle, ActorInfo, ActivationInfo, Character->GetSkillRule(Skill));
}

// 把本次成本作为负的 SetByCaller 值交给 GE，客户端预测和权威确认不另写扣费账本。
void UmultiplayerCombatAbility::ApplyRuleCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const
{
	if (UAbilitySystemGlobals::Get().ShouldIgnoreCosts() || !CostGameplayEffectClass || Rule.Cost <= 0) return;
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, CostGameplayEffectClass, 1);
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Cost, -Rule.Cost);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
}

// 支持 GAS 单独提交冷却的入口，使用当前规则和原生效果生命周期。
void UmultiplayerCombatAbility::ApplyCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character) ApplyRuleCooldown(Handle, ActorInfo, ActivationInfo, Character->GetSkillRule(Skill));
}

// 使用调用者固定的冷却时长，避免扣费回调改变等级后混用另一份提交数值。
void UmultiplayerCombatAbility::ApplyRuleCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const
{
	if (UAbilitySystemGlobals::Get().ShouldIgnoreCooldowns()) return;
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, CooldownGameplayEffectClass, 1);
	Spec.Data->SetDuration(Rule.Cooldown, true);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
}

// CommitCheck 通过后先扣费再挂冷却；每次外部通知返回时，旧激活栈必须仍属于当前版本。
// Can 决定能否开始，CommitCheck 只复核此刻的成本/CD；它不重跑完整 Can，也不代替出手时目标检查。
// 这不是原子事务：扣费通知可以取消技能，不自动退款；具体能力在 Commit 返回后仍须复核本轮资格。
void UmultiplayerCombatAbility::CommitExecute(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo)
{
	const uint32 Revision = ActivationRevision;
	const auto* CommitCharacter = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (!CommitCharacter) return;
	// 仅锁定本次提交的成本/CD；扣费通知同步修改等级时，冷却不能改读成另一份规则。
	// 伤害、目标和出手合法性仍由具体技能在生效阶段决定，不宣称全量技能快照。
	const FmultiplayerCombatSkillRule Rule = CommitCharacter->GetSkillRule(Skill);
	if (!ActorInfo->IsNetAuthority())
	{
		// 预测拒绝只清理仍归属于本次预测键的动作；资源／冷却撤销仍交 GAS，原因另走失败通知。
		FPredictionKey Key = ActivationInfo.GetActivationPredictionKey();
		if (Key.IsLocalClientKey())
			Key.NewRejectedDelegate().BindWeakLambda(ActorInfo->AvatarActor.Get(), [Weak = TWeakObjectPtr<AmultiplayerCombatCharacter>(Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get())),
				WeakAbility = TWeakObjectPtr<UmultiplayerCombatAbility>(this), RejectedKey = Key.Current]
			{
				if (auto* Character = Weak.Get())
				{
					UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
					if (auto* Ability = WeakAbility.Get(); Ability && ASC->GetAnimatingAbility() == Ability
						&& Ability->GetCurrentActivationInfo().GetActivationPredictionKey().Current == RejectedKey)
						ASC->CurrentMontageStop(.15f);
					// 精确业务原因由服务器拒绝通知提供；此处仅清理预测动作，不覆盖较早到达的原因。
				}
			});
	}
	// 先扣资源再通知冷却变化，阻止冷却监听回调用同一份旧余额嵌套施放另一技能。
	ApplyRuleCost(Handle, ActorInfo, ActivationInfo, Rule);
	if (Revision == ActivationRevision && IsActive() && IsValid(CommitCharacter) && CommitCharacter->IsCombatReady())
		ApplyRuleCooldown(Handle, ActorInfo, ActivationInfo, Rule);
}

// 连接接纳与角色存活是所有技能共用的业务边界；资源、冷却与阻断标签继续由 GAS 校验。
bool UmultiplayerCombatAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	const AmultiplayerCombatCharacter* Character = ActorInfo ? Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	return Character != nullptr && Character->CheckSkillBusinessRequirements(Skill, OptionalRelevantTags)
		&& Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

// 将集中关系补入原生标签检查，预检与正式激活都经同一虚函数，而非再复制一遍规则。
bool UmultiplayerCombatAbility::DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* Reasons) const
{
	const auto* CombatASC = Cast<UmultiplayerCombatAbilitySystem>(&ASC);
	return (!CombatASC || CombatASC->CheckAbilityTagRelationships(GetAssetTags(), Reasons))
		&& Super::DoesAbilitySatisfyTagRequirements(ASC, SourceTags, TargetTags, Reasons);
}

// 只校验事件，不写技能实例；拒绝的新请求不能覆盖上一轮仍在执行的瞄准快照。
// 合法方向只是输入意图：发射点、落点和命中仍由服务器查询，不以载荷校验替代权威结算。
bool UmultiplayerCombatAbility::ShouldAbilityRespondToEvent(const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayEventData* Payload) const
{
	if (!Super::ShouldAbilityRespondToEvent(ActorInfo, Payload)) return false;
	const FGameplayTag ExpectedEvent = CombatTags::GetSkillTags(Skill).Event;
	if (!ExpectedEvent.IsValid()) return true;
	if (!Payload || Payload->EventTag != ExpectedEvent || Payload->TargetData.Num() != 1) return false;
	const FGameplayAbilityTargetData* Data = Payload->TargetData.Get(0);
	if (!Data || Data->GetScriptStruct() != FGameplayAbilityTargetData_LocationInfo::StaticStruct()) return false;
	const auto* Aim = static_cast<const FGameplayAbilityTargetData_LocationInfo*>(Data);
	// 在 GetRotation 的诊断清洗之前拒绝非法网络数据，不能把 NaN 变成合法的零朝向。
	if (Aim->TargetLocation.LiteralTransform.ContainsNaN()) return false;
	const FQuat Rotation = Aim->TargetLocation.LiteralTransform.GetRotation();
	// 客户端仅提供方向意图；位置、伤害和命中对象均不从 Payload 读取。
	return Aim->TargetLocation.LocationType == EGameplayAbilityTargetingLocationType::LiteralTransform
		&& !Rotation.ContainsNaN() && Rotation.IsNormalized();
}

// 攻击：伤害效果、冷却和技能放在一起。
// 每次应用单独填写伤害数值；效果只写入待结算的伤害属性。
UmultiplayerDamageEffect::UmultiplayerDamageEffect()
{
	Executions.AddDefaulted_GetRef().CalculationClass = UmultiplayerCombatDamageExecution::StaticClass();
}

// 瞬时增加可被消耗的护盾，不把剩余护盾藏在一个无法随伤害递减的属性修饰器里。
UmultiplayerShieldEffect::UmultiplayerShieldEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetShieldAttribute();
	FSetByCallerFloat Amount; Amount.DataTag = CombatTags::Data_Shield;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Amount);
}

// GAS 在服务器推进周期；客户端仅接收属性和标签，不用本地 Timer 再扣一次血。
UmultiplayerBurningEffect::UmultiplayerBurningEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("BurningTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 4.f, CombatTags::State_Burning);
	Period = FScalableFloat(1.f);
	bExecutePeriodicEffectOnApplication = false;
	StackingType = EGameplayEffectStackingType::AggregateBySource;
	StackLimitCount = 1;
	StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
	Executions.AddDefaulted_GetRef().CalculationClass = UmultiplayerCombatDamageExecution::StaticClass();
}

// 每个来源独立授予同一个权限标签；最后一层消失才触发移动恢复。
UmultiplayerRootEffect::UmultiplayerRootEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("RootTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 2.f, CombatTags::State_Rooted);
	FInheritedTagContainer Restrictions;
	Restrictions.AddTag(CombatTags::State_Rooted);
	Restrictions.AddTag(CombatTags::State_MovementBlocked);
	Tags->SetAndApplyTargetTagChanges(Restrictions);
}

// 冷却效果存在期间阻止新的普通攻击。
UmultiplayerAttackCooldownEffect::UmultiplayerAttackCooldownEffect()
{
	// 默认构造必须创建有名子对象，不能使用内部通过 NewObject 创建组件的 FindOrAddComponent。
	UTargetTagsGameplayEffectComponent* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("AttackCooldownTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 0.5f, CombatTags::Cooldown_Attack);
}

// 攻击只设置冷却，不额外引入弹药或自动连发状态。
UmultiplayerAttackAbility::UmultiplayerAttackAbility()
{
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Attack));
	// 标签随本次能力自动结束；取消、死亡或预测拒绝均不会留下独立的转向锁。
	ActivationOwnedTags.AddTag(CombatTags::State_Attacking);
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Attack;
	CostGameplayEffectClass = UmultiplayerEnergyCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerAttackCooldownEffect::StaticClass();
}

// GAS 检查并提交冷却，服务器创建真正能够造成伤害的投射物。
void UmultiplayerAttackAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	bProjectileReleased = false;
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character)
	{
		// 事件已在接纳前校验；按键时的方向固定到本轮，起手后转身不会改变这一发。
		AimRotation = TriggerEventData
			? TriggerEventData->TargetData.Get(0)->GetEndPointTransform().Rotator()
			: Character->GetControlRotation();
		Character->SetAttackAimSnapshot(AimRotation);
		if (!Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &AimRotation).bValid)
		{
			if (ActorInfo->IsNetAuthority()) Character->ClientSkillRejected(Skill, CombatTags::Failure_MuzzleBlocked);
			else Character->OnCombatMessage.Broadcast(Character->GetSkillFailureText(Skill, CombatTags::Failure_MuzzleBlocked));
			EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
			return;
		}
	}
	const bool bCommitted = Character && CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted
		// 提交冷却可能同步通知外部，旧调用栈不能在技能已被取消或角色死亡后继续发弹。
		|| !IsActive() || !IsValid(Character) || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	// 接纳攻击时只朝本次瞄准方向转身；观察输入不再持续驱动身体，不改变镜头俯仰。
	Character->SetActorRotation(FRotator(0.f, AimRotation.Yaw, 0.f));
	if (Revision != ActivationRevision || !IsActive()) return;
	if (UAnimMontage* Montage = Character->GetActionMontage(EMultiplayerCombatAnimation::Attack))
		// 冷却限制请求频率，不用于压缩动作；按素材原速播放，避免短冷却把整段动画强行加速。
		ActorInfo->AbilitySystemComponent->PlayMontage(this, ActivationInfo, Montage, 1.0f);
	if (Revision != ActivationRevision || !IsActive()) return;
	// 出手时序属于技能规则，缺少纯表现 Montage 不会跳过起手。
	if (GetRule().Windup > 0)
	{
		auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Windup);
		Delay->OnFinish.AddDynamic(this, &ThisClass::ReleaseProjectile);
		Delay->ReadyForActivation();
	}
	else ReleaseProjectile();
}

// 起手结束后先认领出手，服务器创建一次弹体；嵌套取消后不得追加旧收招任务。
void UmultiplayerAttackAbility::ReleaseProjectile()
{
	if (!IsActive() || bProjectileReleased) return;
	const uint32 Revision = ActivationRevision;
	auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	if (!IsValid(Character) || !Character->IsCombatReady() || Character->IsStunned())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	bProjectileReleased = true;
	if (CurrentActorInfo->IsNetAuthority())
	{
		const auto Launch = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &AimRotation);
		const bool bFired = AmultiplayerCombatProjectile::FireFromSkill(*Character, GetRule().Amount,
			Character->GetCombatConfig().ProjectileSpeed, &AimRotation);
		if (bFired && Revision == ActivationRevision && IsActive() && IsValid(Character))
		{
			FGameplayCueParameters Feedback(CurrentActorInfo->AbilitySystemComponent->MakeEffectContext());
			Feedback.Location = Launch.Origin;
			Feedback.Normal = (Launch.End - Launch.Origin).GetSafeNormal();
			ExecuteAuthorityResultCue(Character->GetAbilitySystemComponent(), CombatTags::Cue_Attack, Feedback);
		}
	}
	if (Revision != ActivationRevision || !IsActive()) return;
	if (GetRule().Recovery > 0)
	{
		auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Recovery);
		Delay->OnFinish.AddDynamic(this, &ThisClass::FinishAttack);
		Delay->ReadyForActivation();
	}
	else FinishAttack();
}

// 收招完成；客户端正常结束只影响自身能力，不能提前结束服务器尚未完成的阶段。
void UmultiplayerAttackAbility::FinishAttack()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}

// 预测键隔离旧轮结束，版本号隔离停动画时触发的新激活；只停当前能力拥有的动作。
void UmultiplayerAttackAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo)
		|| ActivationInfo.GetActivationPredictionKey() != CurrentActivationInfo.GetActivationPredictionKey()) return;
	const uint32 Revision = ActivationRevision;
	if (bWasCancelled && ActorInfo->AbilitySystemComponent->GetAnimatingAbility() == this)
		ActorInfo->AbilitySystemComponent->CurrentMontageStop(.15f);
	if (Revision == ActivationRevision && IsActive())
		Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// 小兵只在服务器释放自己的近战；冷却与资源沿用角色配置，不生成玩家的投射物。
UmultiplayerEnemyMeleeAbility::UmultiplayerEnemyMeleeAbility()
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Attack));
	CostGameplayEffectClass = UmultiplayerEnergyCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerAttackCooldownEffect::StaticClass();
}

// 无合法黑板目标时不启动技能，也不扣资源或进入冷却；身体可在接纳后再朝目标转身。
bool UmultiplayerEnemyMeleeAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Enemy = Cast<AmultiplayerCombatEnemy>(ActorInfo->AvatarActor.Get());
	return ActorInfo->IsNetAuthority() && !bEndingMelee
		&& Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)
		&& CanReachMeleeTarget(Enemy, GetMeleeTarget(Enemy), false);
}

// 固定本轮目标与朝向；移动和决策通过现有 Casting 标签暂停，不另写一套 AI 攻击状态。
void UmultiplayerEnemyMeleeAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	auto* Enemy = Cast<AmultiplayerCombatEnemy>(ActorInfo->AvatarActor.Get());
	AttackTarget = GetMeleeTarget(Enemy);
	bReleased = false;
	const bool bCommitted = ActorInfo->IsNetAuthority() && CanReachMeleeTarget(Enemy, AttackTarget.Get(), false)
		&& CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted || !IsActive() || !IsValid(Enemy) || !Enemy->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	if (!CanReachMeleeTarget(Enemy, AttackTarget.Get(), false))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	Enemy->SetActorRotation(FRotator(0, (AttackTarget->GetActorLocation() - Enemy->GetActorLocation()).Rotation().Yaw, 0));
	if (Revision != ActivationRevision || !IsActive()) return;
	const FActiveGameplayEffectHandle AppliedCasting = ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo,
		MakeOutgoingGameplayEffectSpec(UmultiplayerCastingEffect::StaticClass()));
	// 标签监听可能同步取消技能；尚未写入成员的句柄仍只清理本次效果。
	if (Revision != ActivationRevision || !IsActive())
	{
		ActorInfo->AbilitySystemComponent->RemoveActiveGameplayEffect(AppliedCasting);
		return;
	}
	CastingEffect = AppliedCasting;
	Enemy->SetCastTiming(GetRule().Windup + GetRule().Recovery);
	if (Revision != ActivationRevision || !IsActive()) return;
	if (UAnimMontage* Montage = Enemy->GetActionMontage(EMultiplayerCombatAnimation::Attack))
		ActorInfo->AbilitySystemComponent->PlayMontage(this, ActivationInfo, Montage, 1.0f);
	if (Revision != ActivationRevision || !IsActive()) return;
	ActorInfo->AbilitySystemComponent->ExecuteGameplayCue(CombatTags::Cue_Attack);
	if (Revision != ActivationRevision || !IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Windup);
	Delay->OnFinish.AddDynamic(this, &ThisClass::ReleaseMelee);
	Delay->ReadyForActivation();
}

// 出手只结算一次，且使用此刻的服务器距离、朝向和遮挡；动画通知只负责表现。
void UmultiplayerEnemyMeleeAbility::ReleaseMelee()
{
	if (!IsActive() || bReleased) return;
	const uint32 Revision = ActivationRevision;
	auto* Enemy = Cast<AmultiplayerCombatEnemy>(GetAvatarActorFromActorInfo());
	if (!IsValid(Enemy) || !Enemy->IsCombatReady() || Enemy->IsStunned())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	bReleased = true;
	const float Recovery = GetRule().Recovery;
	if (AmultiplayerCombatCharacter* Target = AttackTarget.Get(); CanReachMeleeTarget(Enemy, Target, true))
		Enemy->ApplyCombatDamage(Target, GetRule().Amount);
	// 受伤和死亡通知允许同步取消，旧出手回调不能替新一轮添加收招任务。
	if (Revision != ActivationRevision || !IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, Recovery);
	Delay->OnFinish.AddDynamic(this, &ThisClass::FinishMelee);
	Delay->ReadyForActivation();
}

// 即使挥空也完成收招，避免追击过程反复立即重试同一次攻击。
void UmultiplayerEnemyMeleeAbility::FinishMelee()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

// 先结束本轮任务，最后解开自己的移动锁；解锁可同步重启行为树，不得再清新一轮成员。
void UmultiplayerEnemyMeleeAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo) || bEndingMelee) return;
	if (ScopeLockCount > 0)
	{
		WaitingToExecute.Add(FPostLockDelegate::CreateUObject(this, &ThisClass::EndAbility,
			Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled));
		return;
	}
	const FActiveGameplayEffectHandle FinishedCasting = CastingEffect;
	{
		TGuardValue<bool> EndingGuard(bEndingMelee, true);
		CastingEffect.Invalidate();
		AttackTarget.Reset();
		if (bWasCancelled && ActorInfo->AbilitySystemComponent->GetAnimatingAbility() == this)
			ActorInfo->AbilitySystemComponent->CurrentMontageStop(.15f);
		Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	}
	if (ActorInfo->AbilitySystemComponent.IsValid())
		ActorInfo->AbilitySystemComponent->RemoveActiveGameplayEffect(FinishedCasting);
}

// 防御：消耗、冷却、限时效果和技能。
// 所有技能只定义这一份能量修改规则；金额仍由本次预测／权威提交写入。
UmultiplayerEnergyCostEffect::UmultiplayerEnergyCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	FSetByCallerFloat Cost; Cost.DataTag = CombatTags::Data_Cost;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
}

// 防御冷却长于防御持续时间，不能连续施放维持永久格挡。
UmultiplayerDefendCooldownEffect::UmultiplayerDefendCooldownEffect()
{
	UTargetTagsGameplayEffectComponent* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("DefendCooldownTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 8.0f, CombatTags::Cooldown_Defend);
}

// 防御到期后 GAS 自动移除标签，不需要额外 Timer 手动恢复状态。
UmultiplayerDefendingEffect::UmultiplayerDefendingEffect()
{
	UTargetTagsGameplayEffectComponent* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("DefendingTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 3.0f, CombatTags::State_Defending);
	GameplayCues.Add(FGameplayEffectCue(CombatTags::Cue_Defense, 0, 1));
}

// 短弹反单独持有标签，不随防御能力立即结束，也不覆盖较长的防御状态。
UmultiplayerParryEffect::UmultiplayerParryEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("ParryTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, .3f, CombatTags::State_Parrying);
}

// 防御期间不能再施放防御，资源消耗和冷却仍交给 GAS 统一检查。
UmultiplayerDefendAbility::UmultiplayerDefendAbility()
{
	Skill = EMultiplayerCombatSkill::Defend;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Defend));
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Defend;
	CostGameplayEffectClass = UmultiplayerEnergyCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDefendCooldownEffect::StaticClass();
}

// 技能当次执行结束后，格挡效果仍保留到持续时间结束或死亡清理。
// 同步 ActivateAbility 调用栈已有 GAS 预测窗口；Cost、Cooldown 和防御效果共用其预测键。
// 不手动修改能量，也不在确认时再次添加效果，接受接管与拒绝撤销都由 GAS 完成。
void UmultiplayerDefendAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	const bool bCommitted = Character && CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted
		// 能量变化的监听者可能取消技能；返回旧调用栈后不能重新挂上防御效果。
		|| !IsActive() || !IsValid(Character) || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	// 防御保持当前身体朝向；前方弹反与预览都读取角色正面，不把自由镜头朝向写回身体。
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(UmultiplayerDefendingEffect::StaticClass());
	Spec.Data->SetDuration(GetRule().Duration, true);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
	if (Revision == ActivationRevision && IsActive() && Character->IsCombatReady())
	{
		auto Parry = MakeOutgoingGameplayEffectSpec(UmultiplayerParryEffect::StaticClass());
		Parry.Data->SetDuration(Character->GetCombatConfig().ParryWindow, true);
		ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Parry);
	}
	// 可选护盾只由服务器授予，避免把可被远端攻击消耗的护盾另做一套本地扣减账本。
	if (Revision == ActivationRevision && IsActive() && ActorInfo->IsNetAuthority() && Character->IsCombatReady())
		Character->GrantShield(Character->GetCombatConfig().DefenseShieldAmount);
	if (Revision == ActivationRevision) EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

// 治疗：治疗效果、消耗、冷却和技能。
// 瞬时治疗直接修改血量，最终合法范围由属性集统一处理。
UmultiplayerHealEffect::UmultiplayerHealEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetHealthAttribute();
	FSetByCallerFloat Heal; Heal.DataTag = CombatTags::Data_Heal;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Heal);
}

// 治疗冷却限制施放频率。
UmultiplayerHealCooldownEffect::UmultiplayerHealCooldownEffect()
{
	UTargetTagsGameplayEffectComponent* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("HealCooldownTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 6.0f, CombatTags::Cooldown_Heal);
}

// 治疗的消耗与冷却和防御独立，不需要另写计时器。
UmultiplayerHealAbility::UmultiplayerHealAbility()
{
	Skill = EMultiplayerCombatSkill::Heal;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Heal));
	CostGameplayEffectClass = UmultiplayerEnergyCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerHealCooldownEffect::StaticClass();
}

// 治疗特有的目标资格留在治疗能力；HUD 和正式激活都通过同一个虚函数查询。
// 生效前仍需重新选取/验证目标，不能把无副作用准入查询当作目标锁定。
bool UmultiplayerHealAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)) return false;
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character->GetHealTarget()) return true;
	if (OptionalRelevantTags) OptionalRelevantTags->AddTag(CombatTags::Failure_NoHealTarget);
	return false;
}

// 服务器选目标和检查遮挡；客户端不能提交任意治疗对象或回血数值。
void UmultiplayerHealAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character == nullptr || !Character->IsAlive())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 无目标直接结束；只有确认目标合法后才提交资源与冷却。
	const bool bCommitted = (!ActorInfo->IsNetAuthority() || Character->GetHealTarget() != nullptr)
		&& CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 消耗属性的通知可能同步取消技能或改变角色状态，提交后再复核施法者和目标。
	if (!IsActive() || !IsValid(Character) || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	// 提交后仅服务器重新选目标，不跨扣费回调保留旧目标。
	if (auto* Target = ActorInfo->IsNetAuthority() ? Character->GetHealTarget() : nullptr;
		IsValid(Target) && Target->IsCombatReady())
	{
		FGameplayEffectSpecHandle HealSpec = MakeOutgoingGameplayEffectSpec(UmultiplayerHealEffect::StaticClass());
		if (HealSpec.IsValid())
		{
			HealSpec.Data->SetSetByCallerMagnitude(CombatTags::Data_Heal, GetRule().Amount);
			ActorInfo->AbilitySystemComponent->ApplyGameplayEffectSpecToTarget(*HealSpec.Data.Get(), Target->GetAbilitySystemComponent());
			if (IsValid(Target))
			{
				FGameplayCueParameters Feedback(HealSpec.Data->GetEffectContext());
				Feedback.Location = Target->GetActorLocation();
				ExecuteAuthorityResultCue(Target->GetAbilitySystemComponent(), CombatTags::Cue_Heal, Feedback);
			}
		}
	}
	if (Revision == ActivationRevision) EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

// 定义范围冷却标签和默认时长，实际提交由当前有效规则覆盖时长。
UmultiplayerAreaCooldownEffect::UmultiplayerAreaCooldownEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("AreaCooldownTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 5, CombatTags::Cooldown_Area);
}

// 施法锁跟随能力保存的效果句柄，生命周期由 EndAbility 解除，不使用独立到期解锁。
UmultiplayerCastingEffect::UmultiplayerCastingEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("CastingTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 1.2f, CombatTags::State_Casting);
	// 异步阶段可能因卡顿延后，锁只能跟随能力结束，不能被一个独立倒计时提前解除。
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	GameplayCues.Add(FGameplayEffectCue(CombatTags::Cue_Casting, 0, 1));
}

// 同目标再次眩晕刷新时长；共享禁行标签的计数仍允许其他禁锢效果独立存在。
UmultiplayerStunEffect::UmultiplayerStunEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("StunTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 1.5f, CombatTags::State_Stunned);
	// 保留原来的眩晕刷新规则；禁行权限还可能由其他独立控制效果持有。
	FInheritedTagContainer Restrictions;
	Restrictions.AddTag(CombatTags::State_Stunned);
	Restrictions.AddTag(CombatTags::State_MovementBlocked);
	Tags->SetAndApplyTargetTagChanges(Restrictions);
	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	StackLimitCount = 1;
	StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	GameplayCues.Add(FGameplayEffectCue(CombatTags::Cue_Stun, 0, 1));
}

// 范围技能采用本地预测和集中互斥，拥有者的主动取消可通知服务器。
UmultiplayerAreaAbility::UmultiplayerAreaAbility()
{
	Skill = EMultiplayerCombatSkill::Area;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Area));
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Area;
	CostGameplayEffectClass = UmultiplayerEnergyCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerAreaCooldownEffect::StaticClass();
	// 主动取消沿用 GAS 的拥有者能力取消通道；资源/CD已在起手提交，取消不退款。
	bServerRespectsRemoteAbilityCancellation = true;
}

// 一次释放只遍历查询到的唯一角色；友军、死亡、越界或隔墙目标都不参与结算。
void UmultiplayerAreaAbility::ResolveTargets(AmultiplayerCombatCharacter& Character, const FVector& Center)
{
	if (!Character.HasAuthority() || !Character.IsCombatReady() || Character.IsStunned()) return;
	const auto& Config = Character.GetCombatConfig();
	// 固定本批配置与等级数值；各次伤害 GE 的来源属性捕获仍在各自创建时发生，并非全量战斗快照。
	const auto Rule = Character.GetSkillRule(EMultiplayerCombatSkill::Area);
	const float Radius = Config.AreaRadius;
	const float RootDuration = Config.AreaRootDuration;
	const float BurningDamage = Config.BurningDamagePerTick;
	const float BurningDuration = Config.BurningDuration;
	const auto Targets = CombatTargeting::CollectAreaCandidates(Character, Center, Radius);
	const FVector NoDirection = FVector::ZeroVector;
	// 已出手的范围伤害按一次同步结算处理：普通取消不撤回本批命中；施法者死亡或眩晕则停止后续候选。
	// 快照与去重先完成，结算回调中即使销毁其他目标也不会访问悬空指针。
	for (auto Weak : Targets)
	{
		auto* Target = Weak.Get();
		// 候选收集不做射线；应用前检查一次完整合法性，前一目标的回调改动也能及时生效。
		if (!Target || !CombatTargeting::CanAffectArea(Character, *Target, Center, Radius)) continue;
		Character.ApplyCombatDamage(Target, Rule.Amount, nullptr, &NoDirection);
		if (IsValid(Target) && Target->IsCombatReady()) Target->ApplyStun(Rule.Duration);
		// 持续效果仍由本次服务器技能施加；只扩展目标效果，不让 Cue 或客户端再执行伤害。
		if (IsValid(Target) && Target->IsCombatReady())
		{
			if (RootDuration > 0) Target->ApplyRoot(RootDuration);
			Character.ApplyBurning(Target, BurningDamage, BurningDuration);
		}
		if (!Character.IsCombatReady() || Character.IsStunned()) break;
	}
	FGameplayCueParameters Feedback;
	Feedback.Location = Center;
	Feedback.RawMagnitude = Radius;
	Character.GetAbilitySystemComponent()->ExecuteGameplayCue(CombatTags::Cue_Area, Feedback);
}

// 先验证并锁定落点，再提交资源／冷却、挂施法锁和起手任务；通知中的取消必须及时截断旧栈。
void UmultiplayerAreaAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	auto* Character = CastChecked<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	bReleased = false;
	// 两端使用请求中的方向快照计算落点；起点、射程、遮挡和地面各自查询，服务器独立裁决最终结果。
	const FRotator AimRotation = TriggerEventData
		? TriggerEventData->TargetData.Get(0)->GetEndPointTransform().Rotator()
		: Character->GetControlRotation();
	const FmultiplayerCombatTargetPreview Preview = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &AimRotation);
	if (!Preview.bValid)
	{
		if (ActorInfo->IsNetAuthority()) Character->ClientSkillRejected(Skill, CombatTags::Failure_InvalidGround);
		else Character->OnCombatMessage.Broadcast(Character->GetSkillFailureText(Skill, CombatTags::Failure_InvalidGround));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const FVector GroundNormal = Preview.Normal;
	TargetLocation = Preview.End;
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted || !IsActive() || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	// 范围攻击只锁定本轮落点，不改变身体朝向；起手后转镜头也不追改落点。
	Character->SetAreaCastTarget(Preview.End, Preview.Normal);
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(UmultiplayerCastingEffect::StaticClass());
	FHitResult TargetHit; TargetHit.Location = TargetHit.ImpactPoint = TargetLocation;
	TargetHit.Normal = TargetHit.ImpactNormal = GroundNormal;
	Spec.Data->GetContext().AddHitResult(TargetHit);
	const FActiveGameplayEffectHandle AppliedCasting = ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
	// 授予标签会同步通知外部，监听者可能立刻眩晕/取消，不能创建一条新的旧任务。
	if (Revision != ActivationRevision || !IsActive())
	{
		CastChecked<UmultiplayerCombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get())->EndCasting(AppliedCasting, ActivationInfo.GetActivationPredictionKey());
		return;
	}
	CastingEffect = AppliedCasting;
	Character->SetCastTiming(GetRule().Windup + GetRule().Recovery);
	// 读条通知也可能触发取消；旧调用不能继续播放已被打断的动作。
	if (Revision != ActivationRevision || !IsActive()) return;
	if (UAnimMontage* Montage = Character->GetActionMontage(EMultiplayerCombatAnimation::Area))
	{
		const int32 Windup = Montage->GetSectionIndex(TEXT("Windup"));
		ActorInfo->AbilitySystemComponent->PlayMontage(this, ActivationInfo, Montage,
			Windup != INDEX_NONE ? Montage->GetSectionLength(Windup) / GetRule().Windup
			: Montage->GetPlayLength() / (GetRule().Windup + GetRule().Recovery));
	}
	if (Revision != ActivationRevision || !IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Windup);
	Delay->OnFinish.AddDynamic(this, &ThisClass::ReleaseArea);
	Delay->ReadyForActivation();
}

// 起手完成先提交出手阶段；服务器按锁定落点结算一次，再建立本轮收招任务。
void UmultiplayerAreaAbility::ReleaseArea()
{
	if (bReleased || !IsActive()) return;
	const uint32 Revision = ActivationRevision;
	auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	if (!IsValid(Character) || !Character->IsCombatReady() || Character->IsStunned())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	bReleased = true; // 先提交阶段，再执行可能同步触发死亡/取消的伤害和表现通知。
	UAbilitySystemComponent* ASC = CurrentActorInfo->AbilitySystemComponent.Get();
	if (UAnimMontage* Montage = Character->GetActionMontage(EMultiplayerCombatAnimation::Area);
		Montage && ASC->GetAnimatingAbility() == this)
	{
		const int32 Release = Montage->GetSectionIndex(TEXT("Release"));
		if (Release != INDEX_NONE)
		{
			float StartTime = 0, EndTime = 0;
			Montage->GetSectionStartAndEndTime(Release, StartTime, EndTime);
			ASC->CurrentMontageJumpToSection(TEXT("Release"));
			ASC->CurrentMontageSetPlayRate((Montage->GetPlayLength() - StartTime) / GetRule().Recovery);
		}
	}
	if (Revision != ActivationRevision || !IsActive()) return;
	// 结算回调可能复用技能实例，本次已经释放的落点不能随成员变量一起改变。
	const FVector ReleaseLocation = TargetLocation;
	if (CurrentActorInfo->IsNetAuthority()) ResolveTargets(*Character, ReleaseLocation);
	if (Revision != ActivationRevision || !IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Recovery);
	Delay->OnFinish.AddDynamic(this, &ThisClass::FinishRecovery);
	Delay->ReadyForActivation();
}

// 完成正常收招；客户端只结束本地阶段，服务器保有权威时间线。
void UmultiplayerAreaAbility::FinishRecovery()
{
	// 客户端只结束本地表现；正常收招由服务器裁决，不能用客户端提前到达的 End RPC 跳过起手。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}

// 先摘下本轮锁句柄再结束任务，最后精确解锁；旧预测键和同步新激活不能清掉下一轮状态。
// Key 对应预测请求，Revision 区分复用实例的轮次，GEHandle 只认领自己施加的效果，三者不能互相替代。
// 普通取消不退款，也不撤销已结算伤害；独立冷却和目标持续效果不随本次施法锁一起删除。
void UmultiplayerAreaAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo)
		|| ActivationInfo.GetActivationPredictionKey() != CurrentActivationInfo.GetActivationPredictionKey()) return;
	const uint32 Revision = ActivationRevision;
	// 任务随 Super::EndAbility 销毁；先解除效果句柄，避免取消回调重复移除同一状态。
	const auto Effect = CastingEffect;
	CastingEffect.Invalidate();
	if (bWasCancelled && ActorInfo && ActorInfo->AbilitySystemComponent.IsValid()
		&& ActorInfo->AbilitySystemComponent->GetAnimatingAbility() == this)
		ActorInfo->AbilitySystemComponent->CurrentMontageStop(.15f);
	if (Revision == ActivationRevision && IsActive())
		Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	if (ActorInfo && ActorInfo->AbilitySystemComponent.IsValid())
	{
		CastChecked<UmultiplayerCombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get())->EndCasting(Effect, ActivationInfo.GetActivationPredictionKey());
	}
}
