// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatCharacter.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Core/multiplayerGameMode.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/OverlapResult.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// ASC 与属性集属于角色生命周期；短时重连由服务器快照恢复，不跨回合保存。
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
float AmultiplayerCombatCharacter::GetHealth() const { return CombatAttributes->GetHealth(); }
float AmultiplayerCombatCharacter::GetMaxHealth() const { return CombatAttributes->GetMaxHealth(); }
float AmultiplayerCombatCharacter::GetEnergy() const { return CombatAttributes->GetEnergy(); }
float AmultiplayerCombatCharacter::GetMaxEnergy() const { return CombatAttributes->GetMaxEnergy(); }
float AmultiplayerCombatCharacter::GetShield() const { return CombatAttributes->GetShield(); }
float AmultiplayerCombatCharacter::GetMaxShield() const { return CombatAttributes->GetMaxShield(); }
bool AmultiplayerCombatCharacter::IsAlive() const { return !bDead && GetHealth() > 0.0f; }
bool AmultiplayerCombatCharacter::IsCombatReady() const { return bCombatSessionReady && IsAlive() && !Coop->IsDowned(); }
bool AmultiplayerCombatCharacter::IsDefending() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Defending); }
bool AmultiplayerCombatCharacter::IsCasting() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Casting); }
bool AmultiplayerCombatCharacter::IsStunned() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Stunned); }
bool AmultiplayerCombatCharacter::IsRooted() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Rooted); }
bool AmultiplayerCombatCharacter::IsBurning() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Burning); }
bool AmultiplayerCombatCharacter::IsParrying() const { return IsCombatReady() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Parrying); }
// 兼容业务直接授予的眩晕标签；正式 GE 同时持有统一禁行权限。
bool AmultiplayerCombatCharacter::IsMovementBlocked() const
{
	return IsStunned() || IsRooted() || (IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_MovementBlocked));
}
float AmultiplayerCombatCharacter::GetRootRemaining() const { return GetEffectRemaining(CombatTags::State_Rooted); }
float AmultiplayerCombatCharacter::GetBurnRemaining() const { return GetEffectRemaining(CombatTags::State_Burning); }
float AmultiplayerCombatCharacter::GetStunRemaining() const { return GetEffectRemaining(CombatTags::State_Stunned); }
float AmultiplayerCombatCharacter::GetAreaCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Area); }
float AmultiplayerCombatCharacter::GetAttackCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Attack); }
float AmultiplayerCombatCharacter::GetHealCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Heal); }
float AmultiplayerCombatCharacter::GetDashCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Dash); }
const UmultiplayerCombatConfig& AmultiplayerCombatCharacter::GetCombatConfig() const
{
	return CombatConfig ? *CombatConfig : *GetDefault<UmultiplayerCombatConfig>();
}
const UmultiplayerCombatConfig* AmultiplayerCombatCharacter::GetCombatConfigAsset() const { return &GetCombatConfig(); }

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
float AmultiplayerCombatCharacter::GetDefenseCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Defend); }

// 两个蓝图查询共用实际效果计时，死亡时统一返回零。
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

// 断线前从服务器真实属性与 GameplayEffect 读取快照，不接受客户端上报的资源和计时。
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
		// 离线期间没有其他属性变化；按断线时来源快照和目标护甲保存减防御前的每跳伤害。
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

// 仅对尚未接纳的新 Pawn 还原服务器快照；离线时间正常扣除，不刷新资源、冷却或死亡状态。
void AmultiplayerCombatCharacter::RestoreResumeState(const FmultiplayerCombatResumeState& State, float OfflineSeconds)
{
	if (!HasAuthority() || bCombatSessionReady) return;
	AbilitySystem->CancelAllAbilities();
	ClearTransientEffects();
	// 先恢复长期上限和装配，再钳本局资源；不把成长加载当成一次治疗或免费回能。
	Progression->RestoreState(State.Progression);
	const float Elapsed = FMath::Max(0.f, OfflineSeconds);
	float OfflineDamage = 0.f;
	// 白名单中只有灼烧造成离线数值变化；先分开防御有效和到期后的跳数，再统一先盾后血。
	for (const FmultiplayerCombatResumeEffect& Effect : State.PersistentEffects)
	{
		if (!Effect.Spec.IsValid() || !Effect.Spec.Data->Def || !Effect.Spec.Data->Def->IsA<UmultiplayerBurningEffect>()) continue;
		const int32 TotalTicks = CountOfflineBurnTicks(Effect, Elapsed);
		const int32 DefendedTicks = State.DefenseRemaining > 0.f
			? CountOfflineBurnTicks(Effect, FMath::Min(Elapsed, FMath::Max(0.f, State.DefenseRemaining - KINDA_SMALL_NUMBER))) : 0;
		OfflineDamage += Effect.RawDamagePerTick * (TotalTicks - DefendedTicks + DefendedTicks * GetCombatConfig().DefenseDamageMultiplier);
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
	TeleportTo(State.Location, State.Rotation);
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
}

// 技能请求与服务器投射物。
// 输入使用 GAS 的拥有者请求通道，服务器仍会重新检查技能条件。
void AmultiplayerCombatCharacter::Attack()
{
	if (IsTargetingSkill()) ConfirmSkillTargeting();
	else if (Progression->GetSlotSkill(0) == EMultiplayerCombatSkill::Area) BeginSlotTargeting(0);
	else Progression->ActivateSlot(0);
}
void AmultiplayerCombatCharacter::Defend() { Progression->ActivateSlot(1); }
void AmultiplayerCombatCharacter::Heal() { Progression->ActivateSlot(2); }
void AmultiplayerCombatCharacter::CastArea() { Progression->ActivateSlot(3); }
void AmultiplayerCombatCharacter::Dash() { Progression->ActivateSlot(4); }
bool AmultiplayerCombatCharacter::ActivateSkill(EMultiplayerCombatSkill Skill)
{
	if (!CheckSkillAvailable(Skill)) { ReportSkillFailure(Skill); return false; }
	const auto Class = GetSkillAbilityClass(Skill);
	if (Skill == EMultiplayerCombatSkill::Attack) return ActivateAimedSkill(Class, CombatTags::Event_Attack);
	if (Skill == EMultiplayerCombatSkill::Defend) return ActivateAimedSkill(Class, CombatTags::Event_Defend);
	if (Skill == EMultiplayerCombatSkill::Area) return ActivateAimedSkill(Class, CombatTags::Event_Area);
	if (Skill == EMultiplayerCombatSkill::Dash) return ActivateAimedSkill(Class, CombatTags::Event_Dash);
	return Class && AbilitySystem->TryActivateAbilityByClass(Class);
}
void AmultiplayerCombatCharacter::Rescue() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerRescueAbility::StaticClass()); }
void AmultiplayerCombatCharacter::CancelRescue()
{
	if (!HasAuthority()) { ServerCancelRescue(); return; }
	if (const auto* Spec = AbilitySystem->FindAbilitySpecFromClass(UmultiplayerRescueAbility::StaticClass())) AbilitySystem->CancelAbilityHandle(Spec->Handle);
}
void AmultiplayerCombatCharacter::ServerCancelRescue_Implementation() { CancelRescue(); }
void AmultiplayerCombatCharacter::RefreshCombatPermissions() { RefreshMovementLock(); OnCombatStateChanged.Broadcast(); }

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
void AmultiplayerCombatCharacter::SaveProgress() { ServerPersistProgress(false); }
void AmultiplayerCombatCharacter::LoadProgress() { ServerPersistProgress(true); }
// 路径与档案由所属服务器决定；限制磁盘操作频率，不接受客户端成长数据。
void AmultiplayerCombatCharacter::ServerPersistProgress_Implementation(bool bLoad)
{
	const double Now = GetWorld()->GetRealTimeSeconds();
	if (!IsCombatReady() || Now - LastPersistenceRequest < 1.0) return;
	LastPersistenceRequest = Now;
	ClientProgressResult(bLoad ? Progression->LoadProgress() : Progression->SaveProgress(), bLoad);
}
void AmultiplayerCombatCharacter::ClientProgressResult_Implementation(bool bSuccess, bool bLoad)
{
	OnCombatMessage.Broadcast(FText::FromString(bSuccess ? (bLoad ? TEXT("已读取成长与配装；本局生命、能量和冷却不回退") : TEXT("成长与配装已保存"))
		: TEXT("操作未完成：存档不存在、不兼容或当前动作尚未结束")));
}

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
void AmultiplayerCombatCharacter::CancelCast()
{
	CancelSkillTargeting();
	CancelRescue();
	FGameplayTagContainer AbilityTags;
	AbilityTags.AddTag(CombatTags::Ability_Area);
	AbilityTags.AddTag(CombatTags::Ability_Dash);
	AbilitySystem->CancelAbilities(&AbilityTags);
}

// 死亡玩家不能通过原有跳跃输入重新进入移动状态。
void AmultiplayerCombatCharacter::Jump()
{
	if (IsCombatReady() && !IsMovementBlocked() && !IsCasting()) Super::Jump();
}

// 唯一业务伤害入口；服务器生成效果并读取来源/目标属性，不信任客户端上传结果。
bool AmultiplayerCombatCharacter::ApplyCombatDamage(AmultiplayerCombatCharacter* Target, float Amount, const FHitResult* Hit)
{
	if (!HasAuthority() || !IsValid(Target) || !Target->IsCombatReady() || !IsHostileTo(Target)
		|| !FMath::IsFinite(Amount) || Amount <= 0) return false;
	FGameplayEffectContextHandle Context = AbilitySystem->MakeEffectContext();
	if (Hit) Context.AddHitResult(*Hit);
	FGameplayEffectSpecHandle Spec = AbilitySystem->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, Context);
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
	AbilitySystem->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->AbilitySystem);
	FGameplayCueParameters Feedback(Context);
	Feedback.Location = Hit ? FVector(Hit->ImpactPoint) : Target->GetActorLocation();
	if (IsValid(Target)) Target->AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Impact, Feedback);
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

// 一次释放只遍历查询到的唯一角色；友军、死亡、越界或隔墙目标都不参与结算。
void AmultiplayerCombatCharacter::ResolveArea(const FVector& Center)
{
	if (!HasAuthority() || !IsCombatReady() || IsStunned()) return;
	const auto Targets = CombatTargeting::CollectAreaTargets(*this, Center, GetCombatConfig().AreaRadius);
	// 快照与去重先完成，结算回调中即使销毁其他目标也不会访问悬空指针。
	for (auto Weak : Targets)
	{
		auto* Target = Weak.Get();
		if (!Target || !CombatTargeting::CanAffectArea(*this, *Target, Center, GetCombatConfig().AreaRadius)) continue;
		ApplyCombatDamage(Target, GetSkillRule(EMultiplayerCombatSkill::Area).Amount);
		if (IsValid(Target) && Target->IsCombatReady()) Target->ApplyStun(GetSkillRule(EMultiplayerCombatSkill::Area).Duration);
		// 持续效果仍由本次服务器技能施加；只扩展目标效果，不让 Cue 或客户端再执行伤害。
		if (IsValid(Target) && Target->IsCombatReady())
		{
			if (GetCombatConfig().AreaRootDuration > 0) Target->ApplyRoot(GetCombatConfig().AreaRootDuration);
			ApplyBurning(Target, GetCombatConfig().BurningDamagePerTick, GetCombatConfig().BurningDuration);
		}
		if (!IsCombatReady() || IsStunned()) break;
	}
	FGameplayCueParameters Feedback;
	Feedback.Location = Center;
	Feedback.RawMagnitude = GetCombatConfig().AreaRadius;
	AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Area, Feedback);
}

// 网络技能固定本次输入方向，服务器 AI 则读取本地控制朝向；不接受客户端提供的伤害、起点或命中对象。
bool AmultiplayerCombatCharacter::FireProjectile(float Damage, float Speed, const FRotator* AimSnapshot)
{
	if (!HasAuthority() || !IsCombatReady() || !FMath::IsFinite(Damage) || !FMath::IsFinite(Speed)) return false;
	UWorld* World = GetWorld();
	const FRotator Aim = AimSnapshot ? *AimSnapshot : GetControlRotation();
	if (Aim.ContainsNaN()) return false;
	const FVector MuzzleStart = GetCombatMuzzleLocation();
	// 不能把发射点直接放到墙后，否则贴墙时会绕过投射物的碰撞检测。
	// 从真实起点开启扫掠，不再跳过前 75cm 的敌人；出生在实体内部时才拒绝，友军与触发盒不阻挡。
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CoopProjectileMuzzle), false, this);
	FCollisionResponseParams SceneOnly(ECR_Ignore);
	SceneOnly.CollisionResponse.SetResponse(ECC_WorldStatic, ECR_Block);
	SceneOnly.CollisionResponse.SetResponse(ECC_WorldDynamic, ECR_Block);
	if (World->OverlapBlockingTestByChannel(MuzzleStart, FQuat::Identity, ECC_WorldDynamic,
		FCollisionShape::MakeSphere(CombatTargeting::ProjectileRadius), Query, SceneOnly)) return false;
	// 镜头选目标、真实弹源收敛；贴近敌人或俯视时也不会发出平行于准星的另一条弹道。
	const auto Preview = GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
	const FRotator LaunchRotation = (Preview.End - MuzzleStart).Rotation();
	const FTransform Transform(LaunchRotation, MuzzleStart);
	AmultiplayerCombatProjectile* Projectile = World->SpawnActorDeferred<AmultiplayerCombatProjectile>(
		AmultiplayerCombatProjectile::StaticClass(), Transform, this, this, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Projectile) return false;
	Projectile->Initialize(this, Damage, Speed, GetSkillRule(EMultiplayerCombatSkill::Attack).Range);
	Projectile->FinishSpawning(Transform);
	return true;
}

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
void AmultiplayerCombatCharacter::OnRep_SessionReady()
{
	RefreshMovementLock();
	OnCombatStateChanged.Broadcast();
}
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

void AmultiplayerCombatCharacter::RecordDamageSource(AmultiplayerCombatCharacter* Source)
{
	if (HasAuthority() && Source && IsHostileTo(Source)) LastDamageSource = Source;
}
