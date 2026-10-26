// Copyright Epic Games, Inc. All Rights Reserved.

// 战斗角色协调层：初始化 ASC、接收操作意图、监听生命与控制状态，并保存短时恢复快照。
// 技能准入见 CharacterSkillStatus，瞄准查询见 Targeting；执行步骤与属性结算不在角色中重复实现。

#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatAbilitySystem.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Core/multiplayerGameMode.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// [职责] ASC 与属性集属于 Pawn 生命周期；短时重连由服务器快照重建，不跨回合保存。
// 取舍：玩家和敌人共用角色战斗层；换 Pawn 要重建稳定状态，而不是让 ASC 跨 Pawn 常驻。
AmultiplayerCombatCharacter::AmultiplayerCombatCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AbilitySystem = CreateDefaultSubobject<UmultiplayerCombatAbilitySystem>(TEXT("AbilitySystem"));
	AbilitySystem->SetIsReplicated(true);
	AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CombatAttributes = CreateDefaultSubobject<UmultiplayerCombatAttributes>(TEXT("CombatAttributes"));
	Progression = CreateDefaultSubobject<UmultiplayerCombatProgressionComponent>(TEXT("Progression"));
	Coop = CreateDefaultSubobject<UmultiplayerCombatCoopComponent>(TEXT("CoopLife"));
	AttackAbilityClass = UmultiplayerAttackAbility::StaticClass();
	// DS 只推进 Montage 时钟供 GAS 同步，不要求刷新纯表现骨骼或依赖动画通知结算。
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
}

// 服务器初始化数值并授予技能；两端监听同一组属性变化，供本地界面读取。
void AmultiplayerCombatCharacter::BeginPlay()
{
	Super::BeginPlay();
	// 近身玩家和敌人不能把第三人称镜头挤进身体；只忽略相机探针，墙体遮挡与玩法碰撞保持不变。
	// 在蓝图构造完成后统一设置，避免旧蓝图的 Pawn/CharacterMesh 配置覆盖这条表现规则。
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	GetMesh()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	AbilitySystem->InitAbilityActorInfo(this, this);
	AbilitySystem->RegisterGameplayTagEvent(CombatTags::State_Defending, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &ThisClass::HandleDefenseTagChanged);
	for (FGameplayTag Tag : TArray<FGameplayTag>{CombatTags::State_Casting, CombatTags::State_Stunned,
		CombatTags::State_Rooted, CombatTags::State_MovementBlocked, CombatTags::State_Burning, CombatTags::State_Attacking,
		CombatTags::State_Dashing, CombatTags::State_Parrying, CombatTags::State_Rescuing, CombatTags::Cooldown_Dash})
		AbilitySystem->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
			.AddUObject(this, &ThisClass::HandleControlTagChanged);
	AbilitySystem->AbilityFailedCallbacks.AddUObject(this, &ThisClass::HandleAbilityFailed);
	// 进入角色时异步预载纯表现资源；施法与伤害路径不做同步磁盘加载，DS完全跳过。
	if (GetNetMode() != NM_DedicatedServer)
	{
		const auto& Config = GetCombatConfig();
		TArray<FSoftObjectPath> Paths;
		const FSoftObjectPath ProjectilePath = !Config.ProjectileCascadeEffect.IsNull()
			? Config.ProjectileCascadeEffect.ToSoftObjectPath() : Config.ProjectileEffect.ToSoftObjectPath();
		if (!ProjectilePath.IsNull()) Paths.Add(ProjectilePath);
		if (!Config.AreaMarkerMaterial.IsNull()) Paths.AddUnique(Config.AreaMarkerMaterial.ToSoftObjectPath());
		for (const auto* Feedback : {&Config.AttackFeedback, &Config.ImpactFeedback, &Config.DefenseFeedback,
			&Config.HealFeedback, &Config.CastingFeedback, &Config.AreaFeedback, &Config.StunFeedback, &Config.FailureFeedback})
		{
			if (Feedback != &Config.CastingFeedback)
			{
				const FSoftObjectPath EffectPath = !Feedback->CascadeEffect.IsNull()
					? Feedback->CascadeEffect.ToSoftObjectPath() : Feedback->Effect.ToSoftObjectPath();
				if (!EffectPath.IsNull()) Paths.AddUnique(EffectPath);
			}
			if (!Feedback->Sound.IsNull()) Paths.AddUnique(Feedback->Sound.ToSoftObjectPath());
		}
		if (!Paths.IsEmpty()) PresentationAssets = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths);
	}
	for (const FGameplayAttribute& Attribute : {
		UmultiplayerCombatAttributes::GetHealthAttribute(), UmultiplayerCombatAttributes::GetMaxHealthAttribute(),
		UmultiplayerCombatAttributes::GetShieldAttribute(), UmultiplayerCombatAttributes::GetMaxShieldAttribute(),
		UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetMaxEnergyAttribute()})
	{
		AbilitySystem->GetGameplayAttributeValueChangeDelegate(Attribute).AddUObject(this, &ThisClass::HandleAttributeChanged);
	}
	if (HasAuthority())
	{
		CombatAttributes->SetMaxHealth(FMath::Max(1.0f, InitialMaxHealth));
		CombatAttributes->SetHealth(GetMaxHealth());
		CombatAttributes->SetMaxEnergy(FMath::Max(0.0f, InitialMaxEnergy));
		CombatAttributes->SetEnergy(GetMaxEnergy());
		if (bGrantSupportAbilities)
		{
			Progression->InitializeLoadout();
			AbilitySystem->GiveAbility(FGameplayAbilitySpec(UmultiplayerRescueAbility::StaticClass(), 1));
		}
		else if (AttackAbilityClass) AbilitySystem->GiveAbility(FGameplayAbilitySpec(AttackAbilityClass, 1));
		DeathRewardId = FGuid::NewGuid();
	}
	OnCombatStatsChanged.Broadcast(GetHealth(), GetMaxHealth(), GetEnergy(), GetMaxEnergy());
}

// 控制器可能晚于 BeginPlay 就绪；刷新拥有连接，不重复授予技能或重置血量。
void AmultiplayerCombatCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();
	AbilitySystem->InitAbilityActorInfo(this, this);
}

// 新 Pawn 在 Possess 前仍可能是 MOVE_None；不能把初始化前的禁行模式当成接纳后的恢复目标。
void AmultiplayerCombatCharacter::Restart()
{
	Super::Restart();
	if (bMovementLocked)
	{
		// Restart 已按地面/水体确定当前模式；更新旧锁快照，但不提前解除接纳、施法或眩晕限制。
		const UCharacterMovementComponent* Movement = GetCharacterMovement();
		PreviousMovementMode = Movement->MovementMode;
		PreviousCustomMode = Movement->CustomMovementMode;
	}
	RefreshMovementLock();
}

// 释放角色持有的技能任务与监听，避免退出或重开后旧回调继续执行。
void AmultiplayerCombatCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	AbilitySystem->RegisterGameplayTagEvent(CombatTags::State_Defending, EGameplayTagEventType::NewOrRemoved).RemoveAll(this);
	for (FGameplayTag Tag : TArray<FGameplayTag>{CombatTags::State_Casting, CombatTags::State_Stunned,
		CombatTags::State_Rooted, CombatTags::State_MovementBlocked, CombatTags::State_Burning, CombatTags::State_Attacking,
		CombatTags::State_Dashing, CombatTags::State_Parrying, CombatTags::State_Rescuing, CombatTags::Cooldown_Dash})
		AbilitySystem->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved).RemoveAll(this);
	AbilitySystem->AbilityFailedCallbacks.RemoveAll(this);
	bCombatSessionReady = false;
	AbilitySystem->CancelAllAbilities();
	for (const FGameplayAttribute& Attribute : {
		UmultiplayerCombatAttributes::GetHealthAttribute(), UmultiplayerCombatAttributes::GetMaxHealthAttribute(),
		UmultiplayerCombatAttributes::GetShieldAttribute(), UmultiplayerCombatAttributes::GetMaxShieldAttribute(),
		UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetMaxEnergyAttribute()})
	{
		AbilitySystem->GetGameplayAttributeValueChangeDelegate(Attribute).RemoveAll(this);
	}
	AbilitySystem->ClearActorInfo();
	PresentationAssets.Reset();
	ClearAreaFeedback();
	Super::EndPlay(EndPlayReason);
	// Super 会结束合作组件并广播最后一次状态清理；在它之后撤销唯一表现任务。
	GetWorldTimerManager().ClearTimer(LifePresentationRefreshTimer);
}

// 阵营与死亡状态由服务器复制；属性集另行通过 GAS 的 RepNotify 同步。
void AmultiplayerCombatCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AmultiplayerCombatCharacter, CombatTeam, COND_InitialOnly);
	DOREPLIFETIME(AmultiplayerCombatCharacter, bDead);
	DOREPLIFETIME(AmultiplayerCombatCharacter, bCombatSessionReady);
	DOREPLIFETIME(AmultiplayerCombatCharacter, CastEndTime);
	DOREPLIFETIME(AmultiplayerCombatCharacter, CastDuration);
	DOREPLIFETIME(AmultiplayerCombatCharacter, ConfirmedAreaTarget);
	DOREPLIFETIME(AmultiplayerCombatCharacter, ConfirmedAreaNormal);
	DOREPLIFETIME(AmultiplayerCombatCharacter, bHasAreaTarget);
}

// 只读状态查询。
// 接口与数值查询都读取现存组件，不创建临时状态。
UAbilitySystemComponent* AmultiplayerCombatCharacter::GetAbilitySystemComponent() const { return AbilitySystem; }
// 读取 GAS 当前生命，不在角色中保存第二份余额。
float AmultiplayerCombatCharacter::GetHealth() const { return CombatAttributes->GetHealth(); }
// 读取包含成长和被动加成的当前生命上限。
float AmultiplayerCombatCharacter::GetMaxHealth() const { return CombatAttributes->GetMaxHealth(); }
// 读取本端已有能量；完整资源仅保证服务器与拥有者可见。
float AmultiplayerCombatCharacter::GetEnergy() const { return CombatAttributes->GetEnergy(); }
// 读取本端能量上限，旁观者不能据此判断对方能否付费。
float AmultiplayerCombatCharacter::GetMaxEnergy() const { return CombatAttributes->GetMaxEnergy(); }
// 读取可被伤害消费的当前护盾。
float AmultiplayerCombatCharacter::GetShield() const { return CombatAttributes->GetShield(); }
// 读取护盾容量，不把它与生命上限合并。
float AmultiplayerCombatCharacter::GetMaxShield() const { return CombatAttributes->GetMaxShield(); }
// 同时检查持久死亡标记与生命，避免复制到达顺序造成暂时可行动。
bool AmultiplayerCombatCharacter::IsAlive() const { return !bDead && GetHealth() > 0.0f; }
// 战斗入口要求已接纳、存活且未倒地；不等于任意技能都满足激活条件。
bool AmultiplayerCombatCharacter::IsCombatReady() const { return bCombatSessionReady && IsAlive() && !Coop->IsDowned(); }
// 防御状态来自限时 GE；能力结束不代表防御效果已经到期。
bool AmultiplayerCombatCharacter::IsDefending() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Defending); }
// 查询施法锁标签，供移动权限与读条共同读取。
bool AmultiplayerCombatCharacter::IsCasting() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Casting); }
// 查询当前是否眩晕，不自行维护控制结束计时。
bool AmultiplayerCombatCharacter::IsStunned() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Stunned); }
// 查询禁锢状态；禁锢仅限制移动，不等于禁止全部技能。
bool AmultiplayerCombatCharacter::IsRooted() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Rooted); }
// 查询是否仍有任一来源的灼烧效果。
bool AmultiplayerCombatCharacter::IsBurning() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Burning); }
// 查询短弹反窗口，与较长的防御持续时间分开。
bool AmultiplayerCombatCharacter::IsParrying() const { return IsCombatReady() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Parrying); }
// 兼容业务直接授予的眩晕标签；正式 GE 同时持有统一禁行权限。
bool AmultiplayerCombatCharacter::IsMovementBlocked() const
{
	return IsStunned() || IsRooted() || (IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_MovementBlocked));
}
// 多来源禁锢取当前最长剩余时间，仅用于显示，不决定何时解锁。
float AmultiplayerCombatCharacter::GetRootRemaining() const { return GetEffectRemaining(CombatTags::State_Rooted); }
// 读取已有灼烧效果的最长剩余时间。
float AmultiplayerCombatCharacter::GetBurnRemaining() const { return GetEffectRemaining(CombatTags::State_Burning); }
// 读取眩晕 GE 的剩余时间。
float AmultiplayerCombatCharacter::GetStunRemaining() const { return GetEffectRemaining(CombatTags::State_Stunned); }
// 从 R 冷却 GE 读取剩余时间。
float AmultiplayerCombatCharacter::GetAreaCooldownRemaining() const { return GetSkillCooldownRemaining(EMultiplayerCombatSkill::Area); }
// 从普攻冷却 GE 读取剩余时间。
float AmultiplayerCombatCharacter::GetAttackCooldownRemaining() const { return GetSkillCooldownRemaining(EMultiplayerCombatSkill::Attack); }
// 从治疗冷却 GE 读取剩余时间。
float AmultiplayerCombatCharacter::GetHealCooldownRemaining() const { return GetSkillCooldownRemaining(EMultiplayerCombatSkill::Heal); }
// 从冲刺冷却 GE 读取剩余时间。
float AmultiplayerCombatCharacter::GetDashCooldownRemaining() const { return GetSkillCooldownRemaining(EMultiplayerCombatSkill::Dash); }

// HUD、失败提示和权威拒绝使用同一映射，无效枚举不能退化为空查询并读到其他效果。
float AmultiplayerCombatCharacter::GetSkillCooldownRemaining(EMultiplayerCombatSkill Skill) const
{
	const FGameplayTag Tag = CombatTags::GetSkillTags(Skill).Cooldown;
	return Tag.IsValid() ? GetEffectRemaining(Tag) : 0.f;
}
// 配置资产缺省时使用原生默认配置；调用者只读，不改写共享资产。
const UmultiplayerCombatConfig& AmultiplayerCombatCharacter::GetCombatConfig() const
{
	return CombatConfig ? *CombatConfig : *GetDefault<UmultiplayerCombatConfig>();
}
// 为蓝图暴露同一份只读有效配置，而不是另一条配置来源。
const UmultiplayerCombatConfig* AmultiplayerCombatCharacter::GetCombatConfigAsset() const { return &GetCombatConfig(); }

// 将技能种类映射到实际授予类；小兵可用自己的攻击类替换玩家普攻。
TSubclassOf<UGameplayAbility> AmultiplayerCombatCharacter::GetSkillAbilityClass(EMultiplayerCombatSkill Skill) const
{
	switch (Skill)
	{
	case EMultiplayerCombatSkill::Attack: return AttackAbilityClass;
	case EMultiplayerCombatSkill::Defend: return UmultiplayerDefendAbility::StaticClass();
	case EMultiplayerCombatSkill::Heal: return UmultiplayerHealAbility::StaticClass();
	case EMultiplayerCombatSkill::Area: return UmultiplayerAreaAbility::StaticClass();
	case EMultiplayerCombatSkill::Dash: return UmultiplayerDashAbility::StaticClass();
	default: return nullptr;
	}
}
// 配置不改写；技能升级通过真实 Spec.Level 影响数值，客户端和服务器使用同一公式。
FmultiplayerCombatSkillRule AmultiplayerCombatCharacter::GetSkillRule(EMultiplayerCombatSkill Skill) const
{
	auto Rule = GetCombatConfig().GetSkill(Skill);
	const auto* Spec = AbilitySystem->FindAbilitySpecFromClass(GetSkillAbilityClass(Skill));
	const float ExtraLevels = Spec ? FMath::Max(0, Spec->Level - 1) : 0.f;
	Rule.Amount *= 1.f + .2f * ExtraLevels;
	Rule.Cooldown /= 1.f + .05f * ExtraLevels;
	return Rule;
}

// 同步服务器结束时刻；晚加入从当前状态恢复进度，拥有者则立即使用预测起手时刻。
void AmultiplayerCombatCharacter::SetCastTiming(float Duration)
{
	const AGameStateBase* State = GetWorld()->GetGameState();
	CastDuration = Duration;
	CastEndTime = (State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds()) + Duration;
	OnCombatStateChanged.Broadcast();
}

// 施法时间与标签可能分批到达；每次到达都用当前快照补齐读条，不假定复制顺序。
void AmultiplayerCombatCharacter::OnRep_CastTiming()
{
	OnCombatStateChanged.Broadcast();
}
// 按当前服务器时间估算读条比例；标签消失后不继续显示旧进度。
float AmultiplayerCombatCharacter::GetCastProgress() const
{
	if (!IsCasting() || CastDuration <= 0) return 0;
	const AGameStateBase* State = GetWorld()->GetGameState();
	const double Now = State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
	return FMath::Clamp(1.f - float(CastEndTime - Now) / CastDuration, 0.f, 1.f);
}

// 预测和权威效果短暂并存时取最长剩余时间；拒绝或到期移除效果后自然回到零。
float AmultiplayerCombatCharacter::GetDefenseRemaining() const { return GetEffectRemaining(CombatTags::State_Defending); }

// 冷却使用实际 GameplayEffect 时间，不额外维护 Timer；拒绝预测后不残留本地冷却。
float AmultiplayerCombatCharacter::GetDefenseCooldownRemaining() const { return GetSkillCooldownRemaining(EMultiplayerCombatSkill::Defend); }

// 状态和冷却查询共用实际 GE 计时；死亡时返回零，不在此移除效果。
float AmultiplayerCombatCharacter::GetEffectRemaining(const FGameplayTag& Tag) const
{
	if (!IsAlive()) return 0.0f;
	const FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(
		FGameplayTagContainer(Tag));
	float Remaining = 0.0f;
	for (float EffectRemaining : AbilitySystem->GetActiveEffectsTimeRemaining(Query))
	{
		Remaining = FMath::Max(Remaining, EffectRemaining);
	}
	return Remaining;
}

// 阵营只控制合法攻击目标，不依赖 Actor 的网络 Owner 判断敌我关系。
bool AmultiplayerCombatCharacter::IsHostileTo(const AmultiplayerCombatCharacter* Other) const
{
	return Other != nullptr && CombatTeam != Other->CombatTeam;
}

// 接纳玩家前关闭玩法入口；接纳成功仅恢复存活角色，不能用握手把死亡角色重新激活。
void AmultiplayerCombatCharacter::SetCombatSessionReady(bool bReady)
{
	if (!HasAuthority()) return;
	bCombatSessionReady = bReady;
	if (!bReady) AbilitySystem->CancelAllAbilities();
	SetActorEnableCollision(IsCombatReady());
	RefreshMovementLock();
	OnCombatStateChanged.Broadcast();
}

// [职责] GameMode 断线保存时，从服务器真实属性与 GE 捕获个人快照，不接收客户端数值。
// 只保存稳定数值及白名单效果；旧任务、回调和 ActiveGEHandle 不带入新 Pawn，避免重放半次技能。
FmultiplayerCombatResumeState AmultiplayerCombatCharacter::CaptureResumeState() const
{
	FmultiplayerCombatResumeState State;
	State.Health = GetHealth();
	State.Energy = GetEnergy();
	State.Shield = GetShield();
	State.AttackCooldown = GetEffectRemaining(CombatTags::Cooldown_Attack);
	State.DefenseCooldown = GetDefenseCooldownRemaining();
	State.HealCooldown = GetEffectRemaining(CombatTags::Cooldown_Heal);
	State.DefenseRemaining = GetDefenseRemaining();
	State.AreaCooldown = GetAreaCooldownRemaining();
	State.StunRemaining = GetStunRemaining();
	State.DashCooldown = GetDashCooldownRemaining();
	State.Progression = Progression->CaptureState();
	State.bDowned = Coop->IsDowned();
	State.DownedRemaining = Coop->GetDownedRemaining();
	State.bDead = bDead;
	State.Location = GetActorLocation();
	State.Rotation = GetActorRotation();
	// 按真实 ActiveGE 保存每一份：同源灼烧已由 GAS 合并，不同源灼烧和各层定身仍各自独立。
	for (FActiveGameplayEffectHandle Handle : AbilitySystem->GetActiveEffects(FGameplayEffectQuery()))
	{
		const FActiveGameplayEffect* Effect = AbilitySystem->GetActiveGameplayEffect(Handle);
		if (!Effect || !Effect->Spec.Def) continue;
		// Duration 与 Period 同属 TimerManager；首帧 Pending 会一起延后，不能把 World 时钟的寿命和 Timer 相位混用。
		const float TimerRemaining = GetWorld()->GetTimerManager().GetTimerRemaining(Effect->DurationHandle);
		const float Remaining = TimerRemaining >= 0.f ? TimerRemaining : Effect->GetTimeRemaining(GetWorld()->GetTimeSeconds());
		if (Effect->Spec.Def->IsA<UmultiplayerDefendingEffect>()) State.DefenseRemaining = FMath::Max(0.f, Remaining);
		const bool bBurning = Effect->Spec.Def->IsA<UmultiplayerBurningEffect>();
		if (!bBurning && !Effect->Spec.Def->IsA<UmultiplayerRootEffect>()) continue;
		const float NextTick = bBurning ? GetWorld()->GetTimerManager().GetTimerRemaining(Effect->PeriodHandle) : 0.f;
		// Logout/服务器 Tick 可恰好落在两个计时器同时为零的截止帧，此时最终跳仍待执行。
		// 不支持在 Duration 回调的属性广播栈内同步保存原子快照；普通 Period 回调仍按下一周期保存。
		const bool bPendingFinalTick = bBurning && Remaining == 0.f && NextTick == 0.f;
		if (Remaining <= 0.f && !bPendingFinalTick) continue;
		FmultiplayerCombatResumeEffect& Saved = State.PersistentEffects.AddDefaulted_GetRef();
		Saved.Spec = FGameplayEffectSpecHandle(new FGameplayEffectSpec(Effect->Spec));
		Saved.Remaining = Remaining;
		if (!bBurning) continue;
		// 当前周期回调内 Period 剩余为零，本跳已经体现在属性中；下一跳应在一个周期以后。
		Saved.NextTickDelay = bPendingFinalTick ? 0.f : (NextTick > 0.f ? NextTick : Effect->Spec.GetPeriod());
		FAggregatorEvaluateParameters Evaluation;
		Evaluation.SourceTags = Effect->Spec.CapturedSourceTags.GetAggregatedTags();
		Evaluation.TargetTags = Effect->Spec.CapturedTargetTags.GetAggregatedTags();
		const FGameplayEffectAttributeCaptureDefinition AttackCapture(UmultiplayerCombatAttributes::GetAttackPowerAttribute(), EGameplayEffectAttributeCaptureSource::Source, true);
		float Attack = 0.f;
		if (const auto* Capture = Effect->Spec.CapturedRelevantAttributes.FindCaptureSpecByDefinition(AttackCapture, true))
			Capture->AttemptCalculateAttributeMagnitude(Evaluation, Attack);
		// 按离线期间没有其他属性变化的假设，固定来源快照和目标护甲后的每跳伤害。
		// 这是白名单效果的补算，不是离线世界重演；动态护甲等新规则需要另定离线语义。
		Saved.RawDamagePerTick = FMath::Max(0.f, Effect->Spec.GetSetByCallerMagnitude(CombatTags::Data_Damage, false)
			+ Attack - FMath::Max(0.f, CombatAttributes->GetArmor()));
	}
	return State;
}

namespace
{
	// 只计算既有周期的到期次数，不执行技能、不发送事件，也不把重连视为一次新的效果应用。
	int32 CountOfflineBurnTicks(const FmultiplayerCombatResumeEffect& Effect, float Elapsed)
	{
		const double Period = Effect.Spec.IsValid() ? Effect.Spec.Data->GetPeriod() : 0.f;
		double Window = FMath::Min(FMath::Max(0.0, double(Elapsed)), double(Effect.Remaining));
		// 只在真正到期时沿用 GAS CheckDuration 的最终跳容差，普通周期不能因容差提前扣血。
		if (double(Elapsed) >= double(Effect.Remaining)) Window += KINDA_SMALL_NUMBER;
		if (Period <= 0.0 || Window < double(Effect.NextTickDelay)) return 0;
		return 1 + FMath::FloorToInt((Window - double(Effect.NextTickDelay)) / Period);
	}
}

// [职责] 新 Pawn 仍未接纳时，依次恢复成长上限、位置、资源和白名单效果，完成后由 GameMode 提交接纳状态。
// [边界] 扣除离线耗时并补算白名单灼烧；不重演离线世界、恢复旧能力或回滚恢复失败的临时 Pawn。
bool AmultiplayerCombatCharacter::RestoreResumeState(const FmultiplayerCombatResumeState& State, float OfflineSeconds, FText* OutError)
{
	if (OutError) *OutError = FText::GetEmpty();
	const auto Reject = [OutError](const FText& Error)
	{
		if (OutError) *OutError = Error;
		return false;
	};
	if (!HasAuthority() || bCombatSessionReady)
		return Reject(NSLOCTEXT("Multiplayer", "CombatRestoreContext", "角色不处于待恢复阶段。"));
	AbilitySystem->CancelAllAbilities();
	ClearTransientEffects();
	// 先恢复长期上限和装配，再钳本局资源；不把成长加载当成一次治疗或免费回能。
	if (!Progression->RestoreState(State.Progression))
		return Reject(NSLOCTEXT("Multiplayer", "CombatRestoreLoadout", "成长与配装恢复失败，请重新连接。"));
	// 沿用原生传送规则；失败就拒绝本次接纳，不静默改用出生点，也不额外搜索安全落点。
	if (IsActorBeingDestroyed() || !TeleportTo(State.Location, State.Rotation))
		return Reject(NSLOCTEXT("Multiplayer", "CombatRestoreLocation", "原位置暂时无法恢复，请重新连接。"));
	const float Elapsed = FMath::Max(0.f, OfflineSeconds);
	float OfflineDamage = 0.f;
	// 白名单中只有灼烧造成离线数值变化；无方向灼烧不经过正面格挡，再统一先盾后血。
	for (const FmultiplayerCombatResumeEffect& Effect : State.PersistentEffects)
	{
		if (!Effect.Spec.IsValid() || !Effect.Spec.Data->Def || !Effect.Spec.Data->Def->IsA<UmultiplayerBurningEffect>()) continue;
		const int32 TotalTicks = CountOfflineBurnTicks(Effect, Elapsed);
		OfflineDamage += Effect.RawDamagePerTick * TotalTicks;
	}
	const float SavedShield = FMath::Clamp(State.Shield, 0.f, GetMaxShield());
	const float RestoredShield = FMath::Max(0.f, SavedShield - OfflineDamage);
	const float RestoredHealth = FMath::Clamp(State.Health - FMath::Max(0.f, OfflineDamage - SavedShield), 0.f, GetMaxHealth());
	const bool bExpiredDowned = State.bDowned && Elapsed >= State.DownedRemaining;
	const bool bNotifyRestoredDeath = !bDead && (State.bDead || RestoredHealth <= 0.f || bExpiredDowned);
	bDead = bDead || State.bDead || RestoredHealth <= 0.f || bExpiredDowned;
	CombatAttributes->SetEnergy(FMath::Clamp(State.Energy, 0.0f, GetMaxEnergy()));
	CombatAttributes->SetShield(bDead ? 0.f : RestoredShield);
	CombatAttributes->SetHealth(bDead ? 0.f : RestoredHealth);
	if (bNotifyRestoredDeath)
	{
		OnRep_Dead();
		HandleDeath();
	}
	else if (!bDead)
	{
		const auto RestoreEffect = [this, OfflineSeconds](TSubclassOf<UGameplayEffect> Class, float PreviousRemaining)
		{
			const float Remaining = PreviousRemaining - FMath::Max(0.0f, OfflineSeconds);
			if (Remaining <= 0.0f) return;
			FGameplayEffectSpecHandle Spec = AbilitySystem->MakeOutgoingSpec(Class, 1.0f, AbilitySystem->MakeEffectContext());
			Spec.Data->SetDuration(Remaining, true);
			AbilitySystem->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		};
		RestoreEffect(UmultiplayerAttackCooldownEffect::StaticClass(), State.AttackCooldown);
		RestoreEffect(UmultiplayerDefendCooldownEffect::StaticClass(), State.DefenseCooldown);
		RestoreEffect(UmultiplayerHealCooldownEffect::StaticClass(), State.HealCooldown);
		RestoreEffect(UmultiplayerDefendingEffect::StaticClass(), State.DefenseRemaining);
		RestoreEffect(UmultiplayerAreaCooldownEffect::StaticClass(), State.AreaCooldown);
		RestoreEffect(UmultiplayerStunEffect::StaticClass(), State.StunRemaining);
		RestoreEffect(UmultiplayerDashCooldownEffect::StaticClass(), State.DashCooldown);
		// 恢复剩余效果时保留原 Context、SetByCaller 和来源捕获；应用到新 ASC 会重新捕获目标属性。
		for (const FmultiplayerCombatResumeEffect& Effect : State.PersistentEffects)
		{
			if (!IsAlive() || IsActorBeingDestroyed()) break;
			const float Remaining = Effect.Remaining - Elapsed;
			if (Remaining <= 0.f || !Effect.Spec.IsValid() || !Effect.Spec.Data->Def) continue;
			const bool bBurning = Effect.Spec.Data->Def->IsA<UmultiplayerBurningEffect>();
			if (!bBurning && !Effect.Spec.Data->Def->IsA<UmultiplayerRootEffect>()) continue;
			FGameplayEffectSpec Spec(*Effect.Spec.Data);
			Spec.bDurationLocked = false;
			Spec.SetDuration(Remaining, true);
			const FActiveGameplayEffectHandle Handle = AbilitySystem->ApplyGameplayEffectSpecToSelf(Spec);
			if (bBurning && Handle.IsValid())
			{
				const float NextTick = Effect.NextTickDelay + CountOfflineBurnTicks(Effect, Elapsed) * Spec.GetPeriod() - Elapsed;
				CastChecked<UmultiplayerCombatAbilitySystem>(AbilitySystem)->SetPeriodicEffectDelay(Handle, NextTick);
			}
		}
		if (State.bDowned) Coop->RestoreDowned(State.DownedRemaining - Elapsed);
	}
	ForceNetUpdate();
	return true;
}

// 玩家输入与 GAS 请求；这里不创建弹体或直接结算技能结果。
// 输入使用 GAS 的拥有者请求通道，服务器仍会重新检查技能条件。
void AmultiplayerCombatCharacter::Attack()
{
	if (IsTargetingSkill()) ConfirmSkillTargeting();
	else if (Progression->GetSlotSkill(0) == EMultiplayerCombatSkill::Area) BeginSlotTargeting(0);
	else Progression->ActivateSlot(0);
}
// Q 请求当前第二槽技能；实际技能种类由配装决定。
void AmultiplayerCombatCharacter::Defend() { Progression->ActivateSlot(1); }
// E 请求当前第三槽技能，不在输入入口直接回血。
void AmultiplayerCombatCharacter::Heal() { Progression->ActivateSlot(2); }
// R 请求当前第四槽技能，目标与付费仍由对应能力处理。
void AmultiplayerCombatCharacter::CastArea() { Progression->ActivateSlot(3); }
// Shift 请求当前第五槽技能，不绕过 GAS 修改移动位置。
void AmultiplayerCombatCharacter::Dash() { Progression->ActivateSlot(4); }
// 验证能力已授予后选择事件或普通 Try 入口；正式资格检查仍由 GAS 执行。
bool AmultiplayerCombatCharacter::ActivateSkill(EMultiplayerCombatSkill Skill)
{
	// 所有入口最终由 GAS 的 CanActivate 共用资格查询；不要在相邻分发层重复扫描治疗目标。
	const auto Class = GetSkillAbilityClass(Skill);
	// 尚未授予 Spec 时 GAS 不会进入 CanActivate 或发送失败回调，入口必须明确提示。
	if (!Class || !AbilitySystem->FindAbilitySpecFromClass(Class)) { ReportSkillFailure(Skill); return false; }
	const FGameplayTag Event = CombatTags::GetSkillTags(Skill).Event;
	if (Event.IsValid()) return ActivateAimedSkill(Class, Event);
	return AbilitySystem->TryActivateAbilityByClass(Class);
}
// 请求服务器救援能力，距离、目标占用和引导条件由能力验证。
void AmultiplayerCombatCharacter::Rescue() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerRescueAbility::StaticClass()); }
// 拥有者将松键意图交给服务器；权威端只取消本角色的救援能力。
void AmultiplayerCombatCharacter::CancelRescue()
{
	if (!HasAuthority()) { ServerCancelRescue(); return; }
	if (const auto* Spec = AbilitySystem->FindAbilitySpecFromClass(UmultiplayerRescueAbility::StaticClass())) AbilitySystem->CancelAbilityHandle(Spec->Handle);
}
// 接收所属连接的取消请求，复用同一权威清理入口。
void AmultiplayerCombatCharacter::ServerCancelRescue_Implementation() { CancelRescue(); }
// 生命或合作状态变化后重新计算移动权限，再通知表现层读取当前快照。
void AmultiplayerCombatCharacter::RefreshCombatPermissions() { RefreshMovementLock(); OnCombatStateChanged.Broadcast(); }

// 将同帧倒地/死亡表现变化合并到下一帧，不延迟真正的行动权限更新。
void AmultiplayerCombatCharacter::RequestLifePresentationRefresh()
{
	if (GetNetMode()==NM_DedicatedServer || LifePresentationRefreshTimer.IsValid()) return;
	// BleedOut 同一调用栈先清倒地、再提交死亡；等本帧通知结束，避免先站起再重播死亡。
	LifePresentationRefreshTimer=GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,[this]
	{
		LifePresentationRefreshTimer.Invalidate();
		RefreshDownedPresentation();
	}));
}

// 本地播放或退出倒地姿态；不负责救援结算，也不覆盖已经提交的死亡表现。
void AmultiplayerCombatCharacter::RefreshDownedPresentation()
{
	if (bDead || !DeathMontage) return;
	auto* Animation=GetMesh()->GetAnimInstance();
	if (!Animation) return;
	if (Coop->IsDowned() && !bLifePoseStarted)
	{
		Animation->Montage_Stop(.1f);
		bLifePoseStarted=Animation->Montage_Play(DeathMontage)>0.f;
	}
	else if (!Coop->IsDowned() && bLifePoseStarted)
	{
		bLifePoseStarted=false;
		// 救起只撤销本次倒地接管的动作，不停止其他技能或修改移动/骨骼物理。
		Animation->Montage_Stop(.15f,DeathMontage);
	}
}
// 请求服务器保存长期成长与配装，不保存当前血量或技能执行栈。
void AmultiplayerCombatCharacter::SaveProgress() { ServerPersistProgress(false); }
// 请求服务器读取长期档案，不借读档回滚本局资源和冷却。
void AmultiplayerCombatCharacter::LoadProgress() { ServerPersistProgress(true); }
// 路径与档案由所属服务器决定；限制磁盘操作频率，不接受客户端成长数据。
void AmultiplayerCombatCharacter::ServerPersistProgress_Implementation(bool bLoad)
{
	const double Now = GetWorld()->GetRealTimeSeconds();
	if (!IsCombatReady() || Now - LastPersistenceRequest < 1.0) return;
	LastPersistenceRequest = Now;
	ClientProgressResult(bLoad ? Progression->LoadProgress() : Progression->SaveProgress(), bLoad);
}
// 所属客户端显示权威存取结果，不自行重放成长奖励。
void AmultiplayerCombatCharacter::ClientProgressResult_Implementation(bool bSuccess, bool bLoad)
{
	OnCombatMessage.Broadcast(FText::FromString(bSuccess ? (bLoad ? TEXT("已读取成长与配装；本局生命、能量和冷却不回退") : TEXT("成长与配装已保存"))
		: TEXT("操作未完成：存档不存在、不兼容或当前动作尚未结束")));
}

// [亮点] 远端方向意图随 GAS 激活事件提交；服务器独立求起点、落点和命中，不另收目标裁决。
// [边界] 沿用原生预测窗口及 GE 账本，不为任意 C++ 副作用或半次技能提供通用回滚。
bool AmultiplayerCombatCharacter::ActivateAimedSkill(TSubclassOf<UGameplayAbility> AbilityClass, const FGameplayTag& EventTag)
{
	// AI、独立运行和服务器自己的调用直接取本地快照；远端玩家只发送方向，不发送命中结果。
	if (HasAuthority())
	{
		return AbilitySystem->TryActivateAbilityByClass(AbilityClass);
	}
	auto* Aim = new FGameplayAbilityTargetData_LocationInfo();
	Aim->TargetLocation.LocationType = EGameplayAbilityTargetingLocationType::LiteralTransform;
	Aim->TargetLocation.LiteralTransform = FTransform(GetControlRotation());
	FGameplayEventData Event;
	Event.EventTag = EventTag;
	Event.Instigator = this;
	Event.TargetData.Add(Aim);
	return AbilitySystem->HandleGameplayEvent(EventTag, &Event) > 0;
}
// 能力取消与持续状态分开处理：Q 的 GA 已结束，仍需服务器移除其防御/弹反 GE。
// R 出手后粒子独立于 GA；右键只停当前表现，不能抹去已经结算的伤害与控制。
void AmultiplayerCombatCharacter::CancelCast()
{
	FPredictionKey DefenseKey;
	const bool bCancelDefense = CastChecked<UmultiplayerCombatAbilitySystem>(AbilitySystem)->GetDefensePredictionKey(DefenseKey);
	const FmultiplayerCombatActivationIdentity AreaIdentity = GetAreaActivationIdentity();
	// 先关闭旧预览，清理完成后再通知；这条预览通知中启动的新技能不属于本次取消。
	// 这里只调整预览通知顺序，不把 GAS 效果/任务内部的状态通知改成延后事务。
	const bool bHadTargeting = bSkillTargeting;
	bSkillTargeting = false;
	CancelRescue();
	FGameplayTagContainer AbilityTags;
	AbilityTags.AddTag(CombatTags::Ability_Defend);
	AbilityTags.AddTag(CombatTags::Ability_Area);
	AbilityTags.AddTag(CombatTags::Ability_Dash);
	AbilitySystem->CancelAbilities(&AbilityTags);
	if (!HasAuthority()) StopAreaFeedback(AreaIdentity);
	ServerCancelDefenseAndAreaFeedback(DefenseKey, bCancelDefense, AreaIdentity);
	if (bHadTargeting) OnCombatStateChanged.Broadcast();
}

// 该 RPC 只接纳所属角色的取消意图；客户端不能指定效果类型、伤害或任意效果句柄。
void AmultiplayerCombatCharacter::ServerCancelDefenseAndAreaFeedback_Implementation(FPredictionKey DefenseKey, bool bCancelDefense,
	FmultiplayerCombatActivationIdentity AreaIdentity)
{
	if (bCancelDefense) CastChecked<UmultiplayerCombatAbilitySystem>(AbilitySystem)->EndDefense(DefenseKey);
	// 只解析本角色已授予的 R；未知预测请求不推断成当前新轮次。
	if (!AreaIdentity.IsValid()) return;
	const FmultiplayerCombatActivationIdentity Requested = AreaIdentity;
	AreaIdentity.AuthorityRevision = 0;
	const auto* Spec = AbilitySystem->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	const auto* Ability = Spec ? Cast<UmultiplayerCombatAbility>(Spec->GetPrimaryInstance()) : nullptr;
	const auto Current = Ability ? Ability->GetActivationIdentity() : FmultiplayerCombatActivationIdentity();
	if (Current.IsValid() && Requested.SpecHandle == Current.SpecHandle)
	{
		// 客户端不能通过未来轮次提前屏蔽服务器后续释放。
		if (Requested.AuthorityRevision != 0
			&& Requested.AuthorityRevision <= Current.AuthorityRevision) AreaIdentity = Requested;
		else if (Requested.AuthorityRevision == 0)
		{
			if (Current.Matches(Requested)) AreaIdentity = Current;
			else if (const auto* State = AreaFeedbackStates.Find(Requested.SpecHandle);
				State && State->Identity.Matches(Requested)) AreaIdentity = State->Identity;
		}
	}
	// 未解析请求只确认并释放拥有者的暂存关联，不修改服务器/旁观者的当前状态。
	MulticastStopAreaFeedback(AreaIdentity);
}

// 旧回执只操作自己的激活；无法解析的旧预测请求只结束等待，不取消当前新表现。
void AmultiplayerCombatCharacter::MulticastStopAreaFeedback_Implementation(FmultiplayerCombatActivationIdentity Identity)
{
	if (Identity.AuthorityRevision != 0) StopAreaFeedback(Identity);
	PendingAreaCancellations.RemoveAll([&](const auto& Pending) { return Pending.Matches(Identity); });
}

// 预测轮次只在本端绑定；拒绝造成客户端/服务器轮次分叉时仍通过原预测键关联。
FmultiplayerCombatActivationIdentity AmultiplayerCombatCharacter::GetAreaActivationIdentity() const
{
	const auto* Spec = AbilitySystem->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	const auto* Ability = Spec ? Cast<UmultiplayerCombatAbility>(Spec->GetPrimaryInstance()) : nullptr;
	if (!Ability) return LatestAreaFeedback;
	const auto Identity = Ability->GetActivationIdentity();
	if (Identity.AuthorityRevision == 0)
		if (const auto* State = AreaFeedbackStates.Find(Identity.SpecHandle); State
			&& State->OwnerActivationRevision == Ability->GetLocalActivationRevision() && State->Identity.Matches(Identity))
			return State->Identity;
	return Identity;
}

// 每个能力句柄只保存最新权威轮次；接收旧 Cue、旧取消或旧异步结果不退回旧状态。
bool AmultiplayerCombatCharacter::AcceptAreaFeedbackIdentity(const FmultiplayerCombatActivationIdentity& Identity)
{
	if (!Identity.IsValid() || Identity.AuthorityRevision == 0 || !IsAlive() || IsActorBeingDestroyed()) return false;
	auto& State = AreaFeedbackStates.FindOrAdd(Identity.SpecHandle);
	if (State.Identity.AuthorityRevision != 0
		&& static_cast<int32>(Identity.AuthorityRevision - State.Identity.AuthorityRevision) < 0) return false;
	if (Identity.AuthorityRevision != State.Identity.AuthorityRevision)
	{
		State.Identity = Identity;
		State.bCancelled = false;
		State.OwnerActivationRevision = 0;
	}
	if (IsLocallyControlled() && !State.bCancelled && State.OwnerActivationRevision == 0)
	{
		const auto* Spec = AbilitySystem->FindAbilitySpecFromHandle(Identity.SpecHandle);
		const auto* Ability = Spec ? Cast<UmultiplayerCombatAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (Ability && Ability->GetActivationIdentity().Matches(Identity))
			State.OwnerActivationRevision = Ability->GetLocalActivationRevision();
	}
	if (PendingAreaCancellations.ContainsByPredicate([&](const auto& Pending) { return Pending.Matches(Identity); }))
		State.bCancelled = true;
	LatestAreaFeedback = Identity;
	// 新 R 接替同能力旧结果；先摘除集合，再销毁，回调创建的新组件不在旧清理范围内。
	TArray<TWeakObjectPtr<USceneComponent>> Replaced;
	for (int32 Index = AreaFeedback.Num() - 1; Index >= 0; --Index)
		if (!AreaFeedback[Index].Component.IsValid() || (AreaFeedback[Index].Identity.SpecHandle == Identity.SpecHandle
			&& !AreaFeedback[Index].Identity.Matches(Identity)))
		{
			Replaced.Add(AreaFeedback[Index].Component);
			AreaFeedback.RemoveAtSwap(Index);
		}
	for (const auto& Entry : Replaced) if (auto* Component = Entry.Get()) Component->DestroyComponent();
	return !IsAreaFeedbackCancelled(Identity);
}

// 权威身份按服务器轮次比较；预测键只用于尚未收到权威回执的短暂关联。
bool AmultiplayerCombatCharacter::IsAreaFeedbackCancelled(const FmultiplayerCombatActivationIdentity& Identity) const
{
	if (!Identity.IsValid()) return true;
	if (PendingAreaCancellations.ContainsByPredicate([&](const auto& Pending) { return Pending.Matches(Identity); })) return true;
	if (const auto* State = AreaFeedbackStates.Find(Identity.SpecHandle))
	{
		if (Identity.AuthorityRevision != 0 && State->Identity.AuthorityRevision != 0
			&& static_cast<int32>(Identity.AuthorityRevision - State->Identity.AuthorityRevision) < 0) return true;
		if (!State->bCancelled || !State->Identity.Matches(Identity)) return false;
		if (Identity.AuthorityRevision != 0) return true;
		const auto* Spec = AbilitySystem->FindAbilitySpecFromHandle(Identity.SpecHandle);
		const auto* Ability = Spec ? Cast<UmultiplayerCombatAbility>(Spec->GetPrimaryInstance()) : nullptr;
		return Ability && State->OwnerActivationRevision != 0
			&& State->OwnerActivationRevision == Ability->GetLocalActivationRevision();
	}
	return false;
}

// 组件、补载和取消都使用 Cue 内同一份身份；登记时再复核一次，不复活已取消的粒子。
void AmultiplayerCombatCharacter::TrackAreaFeedback(USceneComponent* Component, const FmultiplayerCombatActivationIdentity& Identity)
{
	if (!IsValid(Component)) return;
	if (IsAreaFeedbackCancelled(Identity) || IsActorBeingDestroyed() || !IsAlive())
	{
		Component->DestroyComponent();
		return;
	}
	AreaFeedback.RemoveAll([](const auto& Entry) { return !Entry.Component.IsValid(); });
	AreaFeedback.Add({Component, Identity});
}

FmultiplayerCombatActivationIdentity AmultiplayerCombatCharacter::GetAreaFeedbackIdentity(const USceneComponent* Component) const
{
	if (!Component) return {};
	for (const auto& Entry : AreaFeedback) if (Entry.Component.Get() == Component) return Entry.Identity;
	return {};
}

// 先记录取消，再摘除同轮组件；迟到的 A 不能清 B，也不能让 A 再次显示。
void AmultiplayerCombatCharacter::StopAreaFeedback(const FmultiplayerCombatActivationIdentity& Identity)
{
	if (!Identity.IsValid()) return;
	if (Identity.AuthorityRevision != 0)
	{
		AcceptAreaFeedbackIdentity(Identity);
		if (auto* State = AreaFeedbackStates.Find(Identity.SpecHandle); State && State->Identity.Matches(Identity))
			State->bCancelled = true;
	}
	else if (!PendingAreaCancellations.ContainsByPredicate([&](const auto& Pending) { return Pending.Matches(Identity); }))
		PendingAreaCancellations.Add(Identity);
	TArray<TWeakObjectPtr<USceneComponent>> Stopped;
	for (int32 Index = AreaFeedback.Num() - 1; Index >= 0; --Index)
		if (AreaFeedback[Index].Identity.Matches(Identity))
		{
			Stopped.Add(AreaFeedback[Index].Component);
			AreaFeedback.RemoveAtSwap(Index);
		}
	for (const auto& Entry : Stopped) if (auto* Component = Entry.Get()) Component->DestroyComponent();
	if (LatestAreaFeedback.Matches(Identity))
		if (auto* Player = Cast<APlayerController>(GetController()); Player && IsLocallyControlled())
			if (auto* HUD = Cast<AmultiplayerCombatHUD>(Player->GetHUD())) HUD->ClearResolvedArea();
}

// 退出/死亡清理所有本地观察状态；这不是另一条服务器取消或伤害回滚链路。
void AmultiplayerCombatCharacter::ClearAreaFeedback()
{
	const auto PreviousFeedback = MoveTemp(AreaFeedback);
	AreaFeedbackStates.Reset();
	PendingAreaCancellations.Reset();
	LatestAreaFeedback = {};
	for (const auto& Entry : PreviousFeedback) if (auto* Component = Entry.Component.Get()) Component->DestroyComponent();
	if (auto* Player = Cast<APlayerController>(GetController()); Player && IsLocallyControlled())
		if (auto* HUD = Cast<AmultiplayerCombatHUD>(Player->GetHUD())) HUD->ClearResolvedArea();
}

// 死亡玩家不能通过原有跳跃输入重新进入移动状态。
void AmultiplayerCombatCharacter::Jump()
{
	if (IsCombatReady() && !IsMovementBlocked() && !IsCasting()) Super::Jump();
}

// 根据本次效果保存的来向判断正面直接伤害；这是只读裁决，不消费盾或血。
bool AmultiplayerCombatCharacter::ShouldBlockDirectDamage(const FGameplayEffectSpec& Spec) const
{
	const FGameplayEffectContextHandle Context = Spec.GetContext();
	if (!IsCombatReady() || !AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Defending)
		|| !Spec.Def || !Spec.Def->IsA<UmultiplayerDamageEffect>() || !Context.HasOrigin()) return false;
	// Q 只完全挡住身体正面范围内的直接攻击；无方向与持续伤害照常先盾后血。
	const FVector ToAttack = (Context.GetOrigin() - GetActorLocation()).GetSafeNormal();
	const FVector Facing = GetActorForwardVector().GetSafeNormal2D();
	return !ToAttack.ContainsNaN() && !ToAttack.IsNearlyZero()
		&& FVector::DotProduct(Facing, ToAttack) >= GetCombatConfig().ParryFrontDot;
}

// [职责] 统一直接伤害入口：服务器建 GE 后交 Execution/AttributeSet 结算，Cue 只反馈结果。
// 灼烧由周期 GE 直接进入同一属性结算器，不反向调用本入口，也不接收客户端伤害值。
bool AmultiplayerCombatCharacter::ApplyCombatDamage(AmultiplayerCombatCharacter* Target, float Amount, const FHitResult* Hit,
	const FVector* IncomingDirection)
{
	if (!HasAuthority() || !IsValid(Target) || !Target->IsCombatReady() || !IsHostileTo(Target)
		|| !FMath::IsFinite(Amount) || Amount <= 0) return false;
	FGameplayEffectContextHandle Context = AbilitySystem->MakeEffectContext();
	if (Hit) Context.AddHitResult(*Hit);
	// Context 固定本次攻击来向，命中结算不再读取发射者后来移动到的位置。
	if (IncomingDirection)
	{
		if (!IncomingDirection->ContainsNaN() && !IncomingDirection->IsNearlyZero())
			Context.AddOrigin(Target->GetActorLocation() - IncomingDirection->GetSafeNormal());
	}
	else Context.AddOrigin(GetActorLocation());
	FGameplayEffectSpecHandle Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, Context);
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
	// 先固定本次格挡结果；属性通知可同步改变朝向/防御状态，不能让它改写已经结算的命中反馈。
	const bool bBlockedDirectDamage = Target->ShouldBlockDirectDamage(*Spec.Data.Get());
	AbilitySystem->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->AbilitySystem);
	if (!bBlockedDirectDamage && IsValid(Target))
	{
		FGameplayCueParameters Feedback(Context);
		Feedback.Location = Hit ? FVector(Hit->ImpactPoint) : Target->GetActorLocation();
		Target->AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Impact, Feedback);
	}
	return true;
}

// 控制状态只来自权威效果；AggregateByTarget 将重复施加合为刷新，不先解控再重加。
void AmultiplayerCombatCharacter::ApplyStun(float Duration)
{
	if (!HasAuthority() || !IsCombatReady() || !FMath::IsFinite(Duration) || Duration <= 0) return;
	FGameplayEffectSpecHandle Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerStunEffect::StaticClass(), 1, AbilitySystem->MakeEffectContext());
	Spec.Data->SetDuration(FMath::Min(Duration, 120.f), true);
	AbilitySystem->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}

// 禁锢只封锁普通移动，保留原地攻击和支援能力；每份效果由自己的句柄结束。
FActiveGameplayEffectHandle AmultiplayerCombatCharacter::ApplyRoot(float Duration)
{
	if (!HasAuthority() || !IsCombatReady() || !FMath::IsFinite(Duration) || Duration <= 0) return {};
	auto Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerRootEffect::StaticClass(), 1, AbilitySystem->MakeEffectContext());
	Spec.Data->SetDuration(Duration, true);
	return AbilitySystem->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}

// 来源 ASC 留在上下文中供 GAS 按来源聚合；目标与每跳数值均由服务器技能确定。
FActiveGameplayEffectHandle AmultiplayerCombatCharacter::ApplyBurning(AmultiplayerCombatCharacter* Target,
	float DamagePerTick, float Duration)
{
	if (!HasAuthority() || !IsValid(Target) || !Target->IsCombatReady() || !IsHostileTo(Target)
		|| !FMath::IsFinite(DamagePerTick) || DamagePerTick <= 0 || !FMath::IsFinite(Duration) || Duration <= 0) return {};
	auto Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerBurningEffect::StaticClass(), 1, AbilitySystem->MakeEffectContext());
	Spec.Data->SetDuration(Duration, true);
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, DamagePerTick);
	return AbilitySystem->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->AbilitySystem);
}

// 护盾的获得也经 GE 修改基础值，避免客户端直接写属性或反复叠加不可消耗的修饰器。
void AmultiplayerCombatCharacter::GrantShield(float Amount)
{
	if (!HasAuthority() || !IsCombatReady() || !FMath::IsFinite(Amount) || Amount <= 0) return;
	auto Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerShieldEffect::StaticClass(), 1, AbilitySystem->MakeEffectContext());
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Shield, Amount);
	AbilitySystem->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}

// 固定本次瞄准快照的水平射程，先防穿墙，再投影到地面；不接收客户端伪造落点。
FVector AmultiplayerCombatCharacter::GetAreaTarget(FVector* OutGroundNormal, const FRotator* AimSnapshot) const
{
	// 既过滤角色，又尊重实体的 Visibility 响应；仅 Pawn 重叠的机关触发盒不再被当作墙。
	const auto Preview = GetSkillTargetPreview(EMultiplayerCombatSkill::Area, AimSnapshot);
	if (OutGroundNormal) *OutGroundNormal = Preview.Normal;
	return Preview.End;
}

// 仅在没有更高优先级动作时播放本地受击，不用受击动画触发伤害或打断技能。
void AmultiplayerCombatCharacter::PlayImpactReaction()
{
	if (!IsAlive() || IsCasting() || IsStunned() || GetNetMode() == NM_DedicatedServer || !HitMontage) return;
	if (UAnimInstance* Animation = GetMesh()->GetAnimInstance(); Animation && !Animation->IsAnyMontagePlaying())
		Animation->Montage_Play(HitMontage);
}

// 状态变化和死亡收尾。
// 血量归零只在服务器提交一次死亡；先锁住状态，再取消技能和通知外部表现。
void AmultiplayerCombatCharacter::HandleAttributeChanged(const FOnAttributeChangeData& Change)
{
	if (HasAuthority() && Change.Attribute == UmultiplayerCombatAttributes::GetHealthAttribute() && Change.NewValue < Change.OldValue)
		CancelRescue();
	if (HasAuthority() && Change.Attribute == UmultiplayerCombatAttributes::GetHealthAttribute() && Change.NewValue <= 0.0f && !bDead)
	{
		if (Coop->TryEnterDowned()) return;
		bDead = true;
		OnRep_Dead();
		HandleDeath();
		ForceNetUpdate();
	}
	OnCombatStatsChanged.Broadcast(GetHealth(), GetMaxHealth(), GetEnergy(), GetMaxEnergy());
}

// 只响应从无到有或从有到无；预测效果被权威效果接替时不会把计数变化当作第二次施放。
void AmultiplayerCombatCharacter::HandleDefenseTagChanged(FGameplayTag Tag, int32 NewCount)
{
	OnDefenseStateChanged.Broadcast(NewCount > 0);
	OnCombatStateChanged.Broadcast();
}

// 只在控制标签跨越零时处理，不每帧扫描权限；取消任务也会自动移除施法锁。
void AmultiplayerCombatCharacter::HandleControlTagChanged(FGameplayTag Tag, int32 NewCount)
{
	if (Tag == CombatTags::State_Attacking || Tag == CombatTags::State_Parrying || Tag == CombatTags::State_Dashing)
	{
		// 身体按本次瞄准快照转身；攻击结束前 CMC 不再用横移方向抢回身体朝向。
		// 弹反短窗口和冲刺同样保留确认方向；多种状态重叠时最后一个结束才恢复原策略。
		const bool bFacingLocked = AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Attacking)
			|| AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Parrying)
			|| AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Dashing);
		UCharacterMovementComponent* Movement = GetCharacterMovement();
		if (bFacingLocked && !bAttackFacingLocked)
		{
			bPreviousOrientToMovement = Movement->bOrientRotationToMovement;
			bPreviousControllerDesiredRotation = Movement->bUseControllerDesiredRotation;
			bAttackFacingLocked = true;
			Movement->bOrientRotationToMovement = false;
			Movement->bUseControllerDesiredRotation = false;
		}
		else if (!bFacingLocked && bAttackFacingLocked)
		{
			bAttackFacingLocked = false;
			Movement->bOrientRotationToMovement = bPreviousOrientToMovement;
			Movement->bUseControllerDesiredRotation = bPreviousControllerDesiredRotation;
		}
	}
	if (Tag == CombatTags::State_Stunned && NewCount > 0) AbilitySystem->CancelAllAbilities();
	if (NewCount > 0 && (Tag == CombatTags::State_Rooted || Tag == CombatTags::State_MovementBlocked))
	{
		FGameplayTagContainer DashTags(CombatTags::Ability_Dash);
		AbilitySystem->CancelAbilities(&DashTags);
	}
	RefreshMovementLock();
	OnCombatStateChanged.Broadcast();
}
// 接纳状态复制后重算移动锁并刷新界面，不能假定 Controller 已先到达。
void AmultiplayerCombatCharacter::OnRep_SessionReady()
{
	RefreshMovementLock();
	OnCombatStateChanged.Broadcast();
}
// [亮点] 汇总生命、施法、控制和救援限制，最后一项解除才恢复原 CMC 模式，避免各系统互相解锁。
void AmultiplayerCombatCharacter::RefreshMovementLock()
{
	const bool bLock = !IsCombatReady() || IsMovementBlocked() || IsCasting() || Coop->IsRescuing();
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (bLock)
	{
		if (!bMovementLocked)
		{
			PreviousMovementMode = Movement->MovementMode;
			PreviousCustomMode = Movement->CustomMovementMode;
			bMovementLocked = true;
		}
		// Teleport/Restart可能在两次事件间重设移动模式；接纳和控制事件都重新落实现有锁。
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}
	else if (!bLock && bMovementLocked)
	{
		bMovementLocked = false;
		// 空中或其他系统原本禁行时，准确恢复原模式，不擅自强制Walking。
		Movement->SetMovementMode(EMovementMode(PreviousMovementMode), PreviousCustomMode);
	}
}

// 只清本项目的瞬态效果；不把死亡后的查询返回零当成已经清理真实冷却或控制标签。
void AmultiplayerCombatCharacter::ClearTransientEffects()
{
	FGameplayTagContainer EffectTags;
	for (FGameplayTag Tag : TArray<FGameplayTag>{CombatTags::State_Defending, CombatTags::State_Casting, CombatTags::State_Stunned,
		CombatTags::State_Rooted, CombatTags::State_Burning, CombatTags::State_Parrying,
		CombatTags::Cooldown_Attack, CombatTags::Cooldown_Defend, CombatTags::Cooldown_Heal, CombatTags::Cooldown_Area,
		CombatTags::Cooldown_Dash}) EffectTags.AddTag(Tag);
	AbilitySystem->RemoveActiveEffectsWithGrantedTags(EffectTags);
}

// 死亡是持久状态，不仅是一条特效消息；晚加入也能禁止移动并恢复死亡表现。
void AmultiplayerCombatCharacter::OnRep_Dead()
{
	if (!bDead) return;
	CancelSkillTargeting();
	ClearAreaFeedback();
	const bool bKeepDownedPose=bLifePoseStarted;
	Coop->ClearLifeState();
	// 只停用 Pawn 的游戏输入，移动、技能和观察一起结束；Controller 继续处理重开、退出等界面操作。
	// 不依赖控制器是否已复制到达，死亡重连和客户端 Restart 也不会重新开放这具 Pawn 的输入。
	DisableInput(nullptr);
	AbilitySystem->AddLooseGameplayTag(CombatTags::State_Dead);
	AbilitySystem->CancelAllAbilities();
	if (!bKeepDownedPose) AbilitySystem->CurrentMontageStop(0.1f);
	ClearTransientEffects();
	// 死亡清除可消耗护盾，不让复制顺序或后续治疗把尸体显示成仍有防护。
	if (HasAuthority()) CombatAttributes->SetShield(0.f);
	GetCharacterMovement()->DisableMovement();
	SetActorEnableCollision(false);
	if (GetNetMode() != NM_DedicatedServer && !bKeepDownedPose)
	{
		if (UAnimInstance* Animation = GetMesh()->GetAnimInstance())
		{
			Animation->Montage_Stop(0.1f);
			if (DeathMontage != nullptr) bLifePoseStarted=Animation->Montage_Play(DeathMontage)>0.f;
		}
	}
	// 胶囊停止碰撞时同步结束占用重叠，压力板和胜利区域不再把尸体计作有效玩家。
	OnCombatDeath();
	OnCombatStateChanged.Broadcast();
}

// 玩家默认不销毁 Pawn，保留控制器和界面；敌人覆盖此处清理 AI 与尸体。
void AmultiplayerCombatCharacter::HandleDeath()
{
	// 每个实际出生实体只有一个死亡奖励标识；重复死亡通知不会再次发奖。
	if (HasAuthority() && CombatTeam == EMultiplayerCombatTeam::Enemies)
		if (auto* Source = LastDamageSource.Get(); Source && Source->IsPlayerControlled() && IsHostileTo(Source))
		{
			const auto* Enemy = Cast<AmultiplayerCombatEnemy>(this);
			Source->GetProgressionComponent()->AwardExperience(Enemy ? Enemy->GetExperienceReward() : 25, DeathRewardId);
		}
	if (HasAuthority() && CombatTeam == EMultiplayerCombatTeam::Players && IsPlayerControlled())
		if (auto* Mode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>()) Mode->NotifyCombatPlayerDeath();
}

// 服务器记录最近合法敌对来源，供死亡奖励归属使用，不接受客户端指定获奖者。
void AmultiplayerCombatCharacter::RecordDamageSource(AmultiplayerCombatCharacter* Source)
{
	if (HasAuthority() && Source && IsHostileTo(Source)) LastDamageSource = Source;
}
