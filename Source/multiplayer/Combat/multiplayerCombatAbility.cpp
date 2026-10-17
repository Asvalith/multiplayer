// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatAbility.h"

#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

namespace
{
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
	// 客户端不提交治疗目标；服务器在接纳与实际提交时共用同一套选目标规则。
	AmultiplayerCombatCharacter* FindHealTarget(AmultiplayerCombatCharacter* Character)
	{
		// 优先最低血量比例；比例相同时选更近的目标，自身也属于合法候选。
		const float HealRangeSquared = FMath::Square(Character->GetCombatConfig().Heal.Range);
		TWeakObjectPtr<AmultiplayerCombatCharacter> SelectedTarget;
		float LowestHealthRatio = 1.0f;
		float ClosestDistanceSquared = HealRangeSquared;
		FVector EyeLocation;
		FRotator EyeRotation;
		Character->GetActorEyesViewPoint(EyeLocation, EyeRotation);
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CoopHealLineOfSight), false, Character);
		UWorld* World = Character->GetWorld();

		for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
		{
			AmultiplayerCombatCharacter* Candidate = *It;
			if (!Candidate->IsCombatReady() || Character->IsHostileTo(Candidate)
				|| Candidate->GetHealth() >= Candidate->GetMaxHealth())
			{
				continue;
			}

			const float DistanceSquared = FVector::DistSquared(Character->GetActorLocation(), Candidate->GetActorLocation());
			if (DistanceSquared > HealRangeSquared)
			{
				continue;
			}

			// 对自己无需射线；对队友允许射线命中目标自身，但不能穿过墙或其他遮挡。
			FHitResult VisibilityHit;
			if (Candidate != Character
				&& World->LineTraceSingleByChannel(VisibilityHit, EyeLocation, Candidate->GetActorLocation(), ECC_Visibility, QueryParams)
				&& VisibilityHit.GetActor() != Candidate)
			{
				continue;
			}

			const float HealthRatio = Candidate->GetHealth() / Candidate->GetMaxHealth();
			if (HealthRatio < LowestHealthRatio
				|| (FMath::IsNearlyEqual(HealthRatio, LowestHealthRatio) && DistanceSquared < ClosestDistanceSquared))
			{
				SelectedTarget = Candidate;
				LowestHealthRatio = HealthRatio;
				ClosestDistanceSquared = DistanceSquared;
			}
		}

		return SelectedTarget.Get();
	}
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

// 预测客户端的消耗、冷却和表现；伤害、发弹及治疗结果仍只在服务器提交。
UmultiplayerCombatAbility::UmultiplayerCombatAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationBlockedTags.AddTag(CombatTags::State_Dead);
	ActivationBlockedTags.AddTag(CombatTags::State_Stunned);
	ActivationBlockedTags.AddTag(CombatTags::State_Casting);
}

const FmultiplayerCombatSkillRule& UmultiplayerCombatAbility::GetRule() const
{
	return CastChecked<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo())->GetCombatConfig().GetSkill(Skill);
}

bool UmultiplayerCombatAbility::CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	return Character && Character->GetEnergy() >= Character->GetCombatConfig().GetSkill(Skill).Cost;
}

void UmultiplayerCombatAbility::ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo) const
{
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
	const auto* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (!Character) return;
	const float Cooldown = Character->GetCombatConfig().GetSkill(Skill).Cooldown;
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
					Character->OnCombatMessage.Broadcast(FText::FromString(TEXT("服务器未接受技能，已撤销预测消耗与状态")));
					Character->GetAbilitySystemComponent()->ExecuteGameplayCue(CombatTags::Cue_Failure);
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
	return Character != nullptr && Character->IsCombatReady()
		&& Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

// 攻击：伤害效果、冷却和技能放在一起。
// 每次应用单独填写伤害数值；效果只写入待结算的伤害属性。
UmultiplayerDamageEffect::UmultiplayerDamageEffect()
{
	Executions.AddDefaulted_GetRef().CalculationClass = UmultiplayerCombatDamageExecution::StaticClass();
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
	const bool bCommitted = Character && CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted
		// 提交冷却可能同步通知外部，旧调用栈不能在技能已被取消或角色死亡后继续发弹。
		|| !IsActive() || !IsValid(Character) || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
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
		Character->FireProjectile(GetRule().Amount, Character->GetCombatConfig().ProjectileSpeed);
		if (Revision == ActivationRevision && IsActive() && IsValid(Character))
			Character->GetAbilitySystemComponent()->ExecuteGameplayCue(CombatTags::Cue_Attack);
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

// 防御：消耗、冷却、限时效果和技能。
// GAS 在提交技能前检查能否承担这次消耗，提交后扣除能量。
UmultiplayerDefendCostEffect::UmultiplayerDefendCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	FSetByCallerFloat Cost; Cost.DataTag = CombatTags::Data_Cost;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Cost);
}

// 防御冷却长于防御持续时间，不能连续施放维持永久减伤。
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

// 防御期间不能再施放防御，资源消耗和冷却仍交给 GAS 统一检查。
UmultiplayerDefendAbility::UmultiplayerDefendAbility()
{
	Skill = EMultiplayerCombatSkill::Defend;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Defend));
	ActivationBlockedTags.AddTag(CombatTags::State_Defending);
	CostGameplayEffectClass = UmultiplayerDefendCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDefendCooldownEffect::StaticClass();
}

// 技能当次执行结束后，减伤效果仍保留到持续时间结束或死亡清理。
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
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(UmultiplayerDefendingEffect::StaticClass());
	Spec.Data->SetDuration(GetRule().Duration, true);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);
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
	CancelAbilitiesWithTag.AddTag(CombatTags::Ability_Attack);
}

// 无合法目标时在服务器接纳之前拒绝，让 GAS 对同一 PredictionKey 撤销本地消耗与冷却。
bool UmultiplayerHealAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	return Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)
		&& (!ActorInfo->IsNetAuthority() || FindHealTarget(CastChecked<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get())) != nullptr);
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

	TWeakObjectPtr<AmultiplayerCombatCharacter> SelectedTarget;
	if (ActorInfo->IsNetAuthority()) SelectedTarget = FindHealTarget(Character);
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
	if (UAnimMontage* Montage = Character->GetActionMontage(EMultiplayerCombatAnimation::Heal))
		ActorInfo->AbilitySystemComponent->PlayMontage(this, ActivationInfo, Montage, 1.0f);
	if (Revision != ActivationRevision || !IsActive()) return;
	AmultiplayerCombatCharacter* Target = SelectedTarget.Get();
	if (ActorInfo->IsNetAuthority()) Target = FindHealTarget(Character);
	if (ActorInfo->IsNetAuthority() && IsValid(Target) && Target->IsCombatReady())
	{
		FGameplayEffectSpecHandle HealSpec = MakeOutgoingGameplayEffectSpec(UmultiplayerHealEffect::StaticClass());
		if (HealSpec.IsValid())
		{
			HealSpec.Data->SetSetByCallerMagnitude(CombatTags::Data_Heal, GetRule().Amount);
			ActorInfo->AbilitySystemComponent->ApplyGameplayEffectSpecToTarget(*HealSpec.Data.Get(), Target->GetAbilitySystemComponent());
			if (IsValid(Target)) Target->GetAbilitySystemComponent()->ExecuteGameplayCue(CombatTags::Cue_Heal);
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
	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	StackLimitCount = 1;
	StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	GameplayCues.Add(FGameplayEffectCue(CombatTags::Cue_Stun, 0, 1));
}

UmultiplayerAreaAbility::UmultiplayerAreaAbility()
{
	Skill = EMultiplayerCombatSkill::Area;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Area));
	CostGameplayEffectClass = UmultiplayerAreaCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerAreaCooldownEffect::StaticClass();
	CancelAbilitiesWithTag.AddTag(CombatTags::Ability_Attack);
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
	FVector GroundNormal;
	TargetLocation = Character->GetAreaTarget(&GroundNormal);
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (Revision != ActivationRevision) return;
	if (!bCommitted || !IsActive() || !Character->IsCombatReady())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
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
