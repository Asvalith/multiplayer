// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatCharacter.h"

#include "AbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatConfig.h"
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

// ASC 与属性集属于角色生命周期；短时重连由服务器快照恢复，不跨回合保存。
AmultiplayerCombatCharacter::AmultiplayerCombatCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AbilitySystem = CreateDefaultSubobject<UmultiplayerCombatAbilitySystem>(TEXT("AbilitySystem"));
	AbilitySystem->SetIsReplicated(true);
	AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CombatAttributes = CreateDefaultSubobject<UmultiplayerCombatAttributes>(TEXT("CombatAttributes"));
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
	for (FGameplayTag Tag : TArray<FGameplayTag>{CombatTags::State_Casting, CombatTags::State_Stunned})
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
		AbilitySystem->GiveAbility(FGameplayAbilitySpec(UmultiplayerAttackAbility::StaticClass(), 1));
		if (bGrantSupportAbilities)
		{
			AbilitySystem->GiveAbility(FGameplayAbilitySpec(UmultiplayerDefendAbility::StaticClass(), 1));
			AbilitySystem->GiveAbility(FGameplayAbilitySpec(UmultiplayerHealAbility::StaticClass(), 1));
			AbilitySystem->GiveAbility(FGameplayAbilitySpec(UmultiplayerAreaAbility::StaticClass(), 1));
		}
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
	for (FGameplayTag Tag : TArray<FGameplayTag>{CombatTags::State_Casting, CombatTags::State_Stunned})
		AbilitySystem->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved).RemoveAll(this);
	AbilitySystem->AbilityFailedCallbacks.RemoveAll(this);
	bCombatSessionReady = false;
	AbilitySystem->CancelAllAbilities();
	for (const FGameplayAttribute& Attribute : {
		UmultiplayerCombatAttributes::GetHealthAttribute(), UmultiplayerCombatAttributes::GetMaxHealthAttribute(),
		UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetMaxEnergyAttribute()})
	{
		AbilitySystem->GetGameplayAttributeValueChangeDelegate(Attribute).RemoveAll(this);
	}
	AbilitySystem->ClearActorInfo();
	PresentationAssets.Reset();
	Super::EndPlay(EndPlayReason);
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
}

// 只读状态查询。
// 接口与数值查询都读取现存组件，不创建临时状态。
UAbilitySystemComponent* AmultiplayerCombatCharacter::GetAbilitySystemComponent() const { return AbilitySystem; }
float AmultiplayerCombatCharacter::GetHealth() const { return CombatAttributes->GetHealth(); }
float AmultiplayerCombatCharacter::GetMaxHealth() const { return CombatAttributes->GetMaxHealth(); }
float AmultiplayerCombatCharacter::GetEnergy() const { return CombatAttributes->GetEnergy(); }
float AmultiplayerCombatCharacter::GetMaxEnergy() const { return CombatAttributes->GetMaxEnergy(); }
bool AmultiplayerCombatCharacter::IsAlive() const { return !bDead && GetHealth() > 0.0f; }
bool AmultiplayerCombatCharacter::IsDefending() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Defending); }
bool AmultiplayerCombatCharacter::IsCasting() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Casting); }
bool AmultiplayerCombatCharacter::IsStunned() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Stunned); }
float AmultiplayerCombatCharacter::GetStunRemaining() const { return GetEffectRemaining(CombatTags::State_Stunned); }
float AmultiplayerCombatCharacter::GetAreaCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Area); }
float AmultiplayerCombatCharacter::GetAttackCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Attack); }
float AmultiplayerCombatCharacter::GetHealCooldownRemaining() const { return GetEffectRemaining(CombatTags::Cooldown_Heal); }
const UmultiplayerCombatConfig& AmultiplayerCombatCharacter::GetCombatConfig() const
{
	return CombatConfig ? *CombatConfig : *GetDefault<UmultiplayerCombatConfig>();
}
const UmultiplayerCombatConfig* AmultiplayerCombatCharacter::GetCombatConfigAsset() const { return &GetCombatConfig(); }

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
	SetActorEnableCollision(bReady && IsAlive());
	RefreshMovementLock();
	OnCombatStateChanged.Broadcast();
}

// 断线前从服务器真实属性与 GameplayEffect 读取快照，不接受客户端上报的资源和计时。
FmultiplayerCombatResumeState AmultiplayerCombatCharacter::CaptureResumeState() const
{
	FmultiplayerCombatResumeState State;
	State.Health = GetHealth();
	State.Energy = GetEnergy();
	State.AttackCooldown = GetEffectRemaining(CombatTags::Cooldown_Attack);
	State.DefenseCooldown = GetDefenseCooldownRemaining();
	State.HealCooldown = GetEffectRemaining(CombatTags::Cooldown_Heal);
	State.DefenseRemaining = GetDefenseRemaining();
	State.AreaCooldown = GetAreaCooldownRemaining();
	State.StunRemaining = GetStunRemaining();
	State.bDead = bDead;
	State.Location = GetActorLocation();
	State.Rotation = GetActorRotation();
	return State;
}

// 仅对尚未接纳的新 Pawn 还原服务器快照；离线时间正常扣除，不刷新资源、冷却或死亡状态。
void AmultiplayerCombatCharacter::RestoreResumeState(const FmultiplayerCombatResumeState& State, float OfflineSeconds)
{
	if (!HasAuthority() || bCombatSessionReady) return;
	AbilitySystem->CancelAllAbilities();
	ClearTransientEffects();
	bDead = State.bDead || State.Health <= 0.0f;
	CombatAttributes->SetEnergy(FMath::Clamp(State.Energy, 0.0f, GetMaxEnergy()));
	CombatAttributes->SetHealth(bDead ? 0.0f : FMath::Clamp(State.Health, 0.0f, GetMaxHealth()));
	TeleportTo(State.Location, State.Rotation);
	if (bDead)
	{
		OnRep_Dead();
		HandleDeath();
	}
	else
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
	}
	ForceNetUpdate();
}

// 技能请求与服务器投射物。
// 输入使用 GAS 的拥有者请求通道，服务器仍会重新检查技能条件。
void AmultiplayerCombatCharacter::Attack() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerAttackAbility::StaticClass()); }
void AmultiplayerCombatCharacter::Defend() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerDefendAbility::StaticClass()); }
void AmultiplayerCombatCharacter::Heal() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()); }
void AmultiplayerCombatCharacter::CastArea() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerAreaAbility::StaticClass()); }
void AmultiplayerCombatCharacter::CancelCast()
{
	FGameplayTagContainer AbilityTags(CombatTags::Ability_Area);
	AbilitySystem->CancelAbilities(&AbilityTags);
}

// 死亡玩家不能通过原有跳跃输入重新进入移动状态。
void AmultiplayerCombatCharacter::Jump()
{
	if (IsCombatReady() && !IsStunned() && !IsCasting()) Super::Jump();
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

// 固定服务器朝向上的水平射程，先防穿墙，再投影到地面；不接收客户端伪造落点。
FVector AmultiplayerCombatCharacter::GetAreaTarget(FVector* OutGroundNormal) const
{
	if (OutGroundNormal) *OutGroundNormal = FVector::UpVector;
	const FRotator Aim = Controller ? Controller->GetControlRotation() : GetActorRotation();
	const FVector Start = GetActorLocation();
	FVector End = Start + FRotator(0, Aim.Yaw, 0).Vector() * GetCombatConfig().Area.Range;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatAreaTarget), false, this);
	FHitResult Hit;
	FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
	StaticObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
	if (GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, StaticObjects, Query)) End = Hit.ImpactPoint;
	if (GetWorld()->LineTraceSingleByObjectType(Hit, End + FVector(0, 0, 150), End - FVector(0, 0, 1000), StaticObjects, Query))
	{
		End = Hit.ImpactPoint;
		if (OutGroundNormal) *OutGroundNormal = Hit.ImpactNormal;
	}
	return End;
}

// 一次释放只遍历查询到的唯一角色；友军、死亡、越界或隔墙目标都不参与结算。
void AmultiplayerCombatCharacter::ResolveArea(const FVector& Center)
{
	if (!HasAuthority() || !IsCombatReady() || IsStunned()) return;
	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatAreaDamage), false, this);
	GetWorld()->OverlapMultiByObjectType(Hits, Center, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
		FCollisionShape::MakeSphere(GetCombatConfig().AreaRadius), Query);
	TSet<TWeakObjectPtr<AmultiplayerCombatCharacter>> Targets;
	for (const auto& Hit : Hits)
		if (auto* Target = Cast<AmultiplayerCombatCharacter>(Hit.GetActor())) Targets.Add(Target);
	// 快照与去重先完成，结算回调中即使销毁其他目标也不会访问悬空指针。
	for (auto Weak : Targets)
	{
		auto* Target = Weak.Get();
		if (!Target || !IsHostileTo(Target) || !Target->IsCombatReady()
			|| FVector::DistSquared2D(Center, Target->GetActorLocation()) > FMath::Square(GetCombatConfig().AreaRadius)) continue;
		FHitResult Block;
		FCollisionObjectQueryParams Blockers(ECC_WorldStatic);
		Blockers.AddObjectTypesToQuery(ECC_WorldDynamic);
		if (GetWorld()->LineTraceSingleByObjectType(Block, Center + FVector(0, 0, 40), Target->GetActorLocation(),
			Blockers, Query)) continue;
		ApplyCombatDamage(Target, GetCombatConfig().Area.Amount);
		if (IsValid(Target) && Target->IsCombatReady()) Target->ApplyStun(GetCombatConfig().Area.Duration);
		if (!IsCombatReady() || IsStunned()) break;
	}
	FGameplayCueParameters Feedback;
	Feedback.Location = Center;
	Feedback.RawMagnitude = GetCombatConfig().AreaRadius;
	AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Area, Feedback);
}

// 方向取服务器持有的控制器朝向，不接受客户端提供的伤害、起点或命中对象。
bool AmultiplayerCombatCharacter::FireProjectile(float Damage, float Speed)
{
	if (!HasAuthority() || !IsCombatReady() || !FMath::IsFinite(Damage) || !FMath::IsFinite(Speed)) return false;
	UWorld* World = GetWorld();
	const FRotator Aim = Controller ? Controller->GetControlRotation() : GetActorRotation();
	if (Aim.ContainsNaN()) return false;
	const FVector MuzzleStart = GetActorLocation() + FVector(0, 0, 40);
	const FVector Origin = MuzzleStart + Aim.Vector() * 75.0f;
	// 不能把发射点直接放到墙后，否则贴墙时会绕过投射物的碰撞检测。
	FHitResult MuzzleHit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CoopProjectileMuzzle), false, this);
	if (World->SweepSingleByChannel(MuzzleHit, MuzzleStart, Origin,
		FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(12.0f), Query)) return false;
	const FTransform Transform(Aim, Origin);
	AmultiplayerCombatProjectile* Projectile = World->SpawnActorDeferred<AmultiplayerCombatProjectile>(
		AmultiplayerCombatProjectile::StaticClass(), Transform, this, this, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Projectile) return false;
	Projectile->Initialize(this, Damage, Speed);
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
	if (HasAuthority() && Change.Attribute == UmultiplayerCombatAttributes::GetHealthAttribute() && Change.NewValue <= 0.0f && !bDead)
	{
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
	if (Tag == CombatTags::State_Stunned && NewCount > 0) AbilitySystem->CancelAllAbilities();
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
	const bool bLock = !IsCombatReady() || IsStunned() || IsCasting();
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
		CombatTags::Cooldown_Attack, CombatTags::Cooldown_Defend, CombatTags::Cooldown_Heal, CombatTags::Cooldown_Area}) EffectTags.AddTag(Tag);
	AbilitySystem->RemoveActiveEffectsWithGrantedTags(EffectTags);
}

void AmultiplayerCombatCharacter::HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons)
{
	if (IsLocallyControlled() && GetNetMode() != NM_DedicatedServer)
	{
		OnCombatMessage.Broadcast(FText::FromString(IsStunned() ? TEXT("眩晕期间无法施放") : IsCasting()
			? TEXT("正在施法，可按右键取消") : !IsAlive() ? TEXT("死亡后无法施放")
			: TEXT("施放失败：资源不足、冷却中或没有合法目标")));
		AbilitySystem->ExecuteGameplayCue(CombatTags::Cue_Failure);
	}
}

// 死亡是持久状态，不仅是一条特效消息；晚加入也能禁止移动并恢复死亡表现。
void AmultiplayerCombatCharacter::OnRep_Dead()
{
	if (!bDead) return;
	AbilitySystem->AddLooseGameplayTag(CombatTags::State_Dead);
	AbilitySystem->CancelAllAbilities();
	AbilitySystem->CurrentMontageStop(0.1f);
	ClearTransientEffects();
	GetCharacterMovement()->DisableMovement();
	SetActorEnableCollision(false);
	if (GetNetMode() != NM_DedicatedServer)
	{
		if (UAnimInstance* Animation = GetMesh()->GetAnimInstance())
		{
			Animation->Montage_Stop(0.1f);
			if (DeathMontage != nullptr) Animation->Montage_Play(DeathMontage);
		}
	}
	// 胶囊停止碰撞时同步结束占用重叠，压力板和胜利区域不再把尸体计作有效玩家。
	OnCombatDeath();
	OnCombatStateChanged.Broadcast();
}

// 玩家默认不销毁 Pawn，保留控制器和界面；敌人覆盖此处清理 AI 与尸体。
void AmultiplayerCombatCharacter::HandleDeath()
{
}
