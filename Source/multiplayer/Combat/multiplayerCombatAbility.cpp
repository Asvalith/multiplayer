// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatAbility.h"

#include "AbilitySystemComponent.h"
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
}

// 执行权在服务器；不要把 ServerOnly 的执行策略误写为禁止客户端请求的安全策略。
UmultiplayerCombatAbility::UmultiplayerCombatAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	ActivationBlockedTags.AddTag(CombatTags::State_Dead);
}

// 每次应用单独填写伤害数值；效果只写入待结算的伤害属性。
UmultiplayerDamageEffect::UmultiplayerDamageEffect()
{
	FSetByCallerFloat Damage;
	Damage.DataTag = CombatTags::Data_Damage;
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetIncomingDamageAttribute();
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Damage);
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
	CooldownGameplayEffectClass = UmultiplayerAttackCooldownEffect::StaticClass();
}

// GAS 检查并提交冷却，服务器创建真正能够造成伤害的投射物。
void UmultiplayerAttackAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character == nullptr || !Character->IsAlive()
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo)
		// 提交冷却可能同步通知外部，旧调用栈不能在技能已被取消或角色死亡后继续发弹。
		|| !IsActive() || !IsValid(Character) || !Character->IsAlive())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	Character->FireProjectile(25.0f, 1600.0f);
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

// GAS 在提交技能前检查能否承担这次消耗，提交后扣除能量。
UmultiplayerDefendCostEffect::UmultiplayerDefendCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-20.0f));
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
}

// 防御期间不能再施放防御，资源消耗和冷却仍交给 GAS 统一检查。
UmultiplayerDefendAbility::UmultiplayerDefendAbility()
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
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
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character == nullptr || !Character->IsAlive()
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo)
		// 能量变化的监听者可能取消技能；返回旧调用栈后不能重新挂上防御效果。
		|| !IsActive() || !IsValid(Character) || !Character->IsAlive())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	ApplyGameplayEffectToOwner(Handle, ActorInfo, ActivationInfo,
		GetDefault<UmultiplayerDefendingEffect>(), 1.0f);
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

// 瞬时治疗直接修改血量，最终合法范围由属性集统一处理。
UmultiplayerHealEffect::UmultiplayerHealEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetHealthAttribute();
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(30.0f));
}

// 治疗采用独立消耗效果，避免技能代码中自行扣能量。
UmultiplayerHealCostEffect::UmultiplayerHealCostEffect()
{
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetEnergyAttribute();
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-25.0f));
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
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Heal));
	CostGameplayEffectClass = UmultiplayerHealCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerHealCooldownEffect::StaticClass();
}

// 服务器选目标和检查遮挡；客户端不能提交任意治疗对象或回血数值。
void UmultiplayerHealAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(ActorInfo->AvatarActor.Get());
	if (Character == nullptr || !Character->IsAlive())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 优先最低血量比例；比例相同时选更近的目标，自身也属于合法候选。
	constexpr float HealRange = 600.0f;
	constexpr float HealRangeSquared = FMath::Square(HealRange);
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
		if (!Candidate->IsAlive() || Character->IsHostileTo(Candidate)
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

	// 无目标直接结束；只有确认目标合法后才提交资源与冷却。
	if (!SelectedTarget.IsValid() || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 消耗属性的通知可能同步取消技能或改变角色状态，提交后再复核施法者和目标。
	AmultiplayerCombatCharacter* Target = SelectedTarget.Get();
	if (IsActive() && IsValid(Character) && Character->IsAlive() && IsValid(Target) && Target->IsAlive())
	{
		FGameplayEffectSpecHandle HealSpec = MakeOutgoingGameplayEffectSpec(UmultiplayerHealEffect::StaticClass());
		if (HealSpec.IsValid())
		{
			ActorInfo->AbilitySystemComponent->ApplyGameplayEffectSpecToTarget(*HealSpec.Data.Get(), Target->GetAbilitySystemComponent());
		}
	}
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
