// Copyright Epic Games, Inc. All Rights Reserved.

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
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayCueManager.h"
#include "TimerManager.h"

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

// 预测客户端的消耗、冷却和表现；伤害、发弹及治疗结果仍只在服务器提交。
UmultiplayerCombatAbility::UmultiplayerCombatAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	// 共用状态要求由角色实际 CombatConfig 提供，不在每个能力 CDO 再复制一份。
}

FmultiplayerCombatSkillRule UmultiplayerCombatAbility::GetRule() const
{
	return CastChecked<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo())->GetSkillRule(Skill);
}

bool UmultiplayerCombatAbility::CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	const bool bEnough = Character && Character->GetEnergy() >= Character->GetCombatConfig().GetSkill(Skill).Cost;
	if (!bEnough && OptionalRelevantTags) OptionalRelevantTags->AddTag(CombatTags::Failure_InsufficientEnergy);
	return bEnough;
}

void UmultiplayerCombatAbility::ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo) const
{
	if (UAbilitySystemGlobals::Get().ShouldIgnoreCosts()) return;
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (!Character) return;
	const float Cost = Character->GetCombatConfig().GetSkill(Skill).Cost;
	if (!CostGameplayEffectClass || Cost <= 0) return;
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, CostGameplayEffectClass, 1);
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Cost, -Cost);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
}

void UmultiplayerCombatAbility::ApplyCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo) const
{
	if (UAbilitySystemGlobals::Get().ShouldIgnoreCooldowns()) return;
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (!Character) return;
	const float Cooldown = Character->GetSkillRule(Skill).Cooldown;
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, CooldownGameplayEffectClass, 1);
	Spec.Data->SetDuration(Cooldown, true);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
}

void UmultiplayerCombatAbility::CommitExecute(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo)
{
	const uint32 Revision = ActivationRevision;
	if (!ActorInfo->IsNetAuthority())
	{
		// 服务器拒绝不走本地 CanActivate 失败通知；只补提示，不另写属性回滚。
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
	ApplyCost(Handle, ActorInfo, ActivationInfo);
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Revision == ActivationRevision && IsActive() && Character && Character->IsCombatReady())
		ApplyCooldown(Handle, ActorInfo, ActivationInfo);
}

// 连接接纳与角色存活是所有技能共用的业务边界；资源、冷却与阻断标签继续由 GAS 校验。
bool UmultiplayerCombatAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	const AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	return Character != nullptr && Character->CheckSkillAvailable(Skill, OptionalRelevantTags)
		&& Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

bool UmultiplayerCombatAbility::DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* Reasons) const
{
	const auto* CombatASC = Cast<UmultiplayerCombatAbilitySystem>(&ASC);
	return (!CombatASC || CombatASC->CheckAbilityTagRelationships(GetAssetTags(), Reasons))
		&& Super::DoesAbilitySatisfyTagRequirements(ASC, SourceTags, TargetTags, Reasons);
}

// 只校验事件，不写技能实例；拒绝的新请求不能覆盖上一轮仍在执行的瞄准快照。
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
	CostGameplayEffectClass = UmultiplayerAreaCostEffect::StaticClass();
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
	if (UAnimMontage* Montage = Character->GetActionMontage(EMultiplayerCombatAnimation::Attack))
		// 冷却限制请求频率，不用于压缩动作；按素材原速播放，避免短冷却把整段动画强行加速。
		ActorInfo->AbilitySystemComponent->PlayMontage(this, ActivationInfo, Montage, 1.0f);
	if (Revision != ActivationRevision || !IsActive()) return;
	// 有动作时用同一份规则等待出手；无动作配置的原生角色仍保留立即发弹。
	if (Character->GetActionMontage(EMultiplayerCombatAnimation::Attack) && GetRule().Windup > 0)
	{
		auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Windup);
		Delay->OnFinish.AddDynamic(this, &ThisClass::ReleaseProjectile);
		Delay->ReadyForActivation();
	}
	else ReleaseProjectile();
}

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
		const bool bFired = Character->FireProjectile(GetRule().Amount, Character->GetCombatConfig().ProjectileSpeed, &AimRotation);
		if (bFired && Revision == ActivationRevision && IsActive() && IsValid(Character))
		{
			FGameplayCueParameters Feedback(CurrentActorInfo->AbilitySystemComponent->MakeEffectContext());
			Feedback.Location = Launch.Origin;
			Feedback.Normal = (Launch.End - Launch.Origin).GetSafeNormal();
			ExecuteAuthorityResultCue(Character->GetAbilitySystemComponent(), CombatTags::Cue_Attack, Feedback);
		}
	}
	if (Revision != ActivationRevision || !IsActive()) return;
	if (Character->GetActionMontage(EMultiplayerCombatAnimation::Attack) && GetRule().Recovery > 0)
	{
		auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Recovery);
		Delay->OnFinish.AddDynamic(this, &ThisClass::FinishAttack);
		Delay->ReadyForActivation();
	}
	else FinishAttack();
}

void UmultiplayerAttackAbility::FinishAttack()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}

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
	CostGameplayEffectClass = UmultiplayerAreaCostEffect::StaticClass();
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
	{
		UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
		ASC->PlayMontage(this, ActivationInfo, Montage, 1.0f);
		// OnMontageStarted 可在 ASC 写入动画归属前取消本轮；返回后补停旧动作，不碰回调中新激活的技能。
		if (Revision == ActivationRevision && !IsActive() && IsValid(ASC)
			&& ASC->GetAnimatingAbility() == this && ASC->GetCurrentMontage() == Montage)
			ASC->CurrentMontageStop(.15f);
	}
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
// GAS 在提交技能前检查能否承担这次消耗，提交后扣除能量。
UmultiplayerDefendCostEffect::UmultiplayerDefendCostEffect()
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
	CostGameplayEffectClass = UmultiplayerDefendCostEffect::StaticClass();
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

// 治疗采用独立消耗效果，避免技能代码中自行扣能量。
UmultiplayerHealCostEffect::UmultiplayerHealCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	FSetByCallerFloat Cost; Cost.DataTag = CombatTags::Data_Cost;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
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
	CostGameplayEffectClass = UmultiplayerHealCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerHealCooldownEffect::StaticClass();
}

// 无合法目标时在服务器接纳之前拒绝，让 GAS 对同一 PredictionKey 撤销本地消耗与冷却。
// 基类共用资格查询已检查治疗目标，不在派生 CanActivate 中再扫描整场角色和遮挡。

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

	TWeakObjectPtr<AmultiplayerCombatCharacter> SelectedTarget;
	if (ActorInfo->IsNetAuthority()) SelectedTarget = Character->GetHealTarget();
	// 无目标直接结束；只有确认目标合法后才提交资源与冷却。
	const bool bCommitted = (!ActorInfo->IsNetAuthority() || SelectedTarget.IsValid())
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
	if (Revision != ActivationRevision || !IsActive()) return;
	AmultiplayerCombatCharacter* Target = SelectedTarget.Get();
	if (ActorInfo->IsNetAuthority()) Target = Character->GetHealTarget();
	if (ActorInfo->IsNetAuthority() && IsValid(Target) && Target->IsCombatReady())
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

UmultiplayerAreaCostEffect::UmultiplayerAreaCostEffect()
{
	FSetByCallerFloat Cost; Cost.DataTag = CombatTags::Data_Cost;
	auto& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
}

UmultiplayerAreaCooldownEffect::UmultiplayerAreaCooldownEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("AreaCooldownTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 5, CombatTags::Cooldown_Area);
}

UmultiplayerCastingEffect::UmultiplayerCastingEffect()
{
	auto* Tags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("CastingTags"));
	GEComponents.Add(Tags);
	ConfigureTimedTagEffect(*this, *Tags, 1.2f, CombatTags::State_Casting);
	// 异步阶段可能因卡顿延后，锁只能跟随能力结束，不能被一个独立倒计时提前解除。
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	GameplayCues.Add(FGameplayEffectCue(CombatTags::Cue_Casting, 0, 1));
}

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

UmultiplayerAreaAbility::UmultiplayerAreaAbility()
{
	Skill = EMultiplayerCombatSkill::Area;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Area));
	AbilityTriggers.AddDefaulted_GetRef().TriggerTag = CombatTags::Event_Area;
	CostGameplayEffectClass = UmultiplayerAreaCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerAreaCooldownEffect::StaticClass();
	// 主动取消沿用 GAS 的拥有者能力取消通道；资源/CD已在起手提交，取消不退款。
	bServerRespectsRemoteAbilityCancellation = true;
}

void UmultiplayerAreaAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const uint32 Revision = ++ActivationRevision;
	auto* Character = CastChecked<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	bReleased = false;
	// 两端都计算表现落点，服务器只使用自己的控制朝向、遮挡和地面查询。
	// 本次朝向改为随激活传递的快照；起点、射程、遮挡和地面仍由各自世界查询，伤害仅由服务器结算。
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
	if (CurrentActorInfo->IsNetAuthority()) Character->ResolveArea(ReleaseLocation);
	if (Revision != ActivationRevision || !IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, GetRule().Recovery);
	Delay->OnFinish.AddDynamic(this, &ThisClass::FinishRecovery);
	Delay->ReadyForActivation();
}

void UmultiplayerAreaAbility::FinishRecovery()
{
	// 客户端只结束本地表现；正常收招由服务器裁决，不能用客户端提前到达的 End RPC 跳过起手。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, CurrentActorInfo->IsNetAuthority(), false);
}

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
