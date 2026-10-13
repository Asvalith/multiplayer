// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatCharacter.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"

// ASC 与属性集属于角色生命周期；本版不跨重生保存技能或个人数值。
AmultiplayerCombatCharacter::AmultiplayerCombatCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
	AbilitySystem->SetIsReplicated(true);
	AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CombatAttributes = CreateDefaultSubobject<UmultiplayerCombatAttributes>(TEXT("CombatAttributes"));
}

// 服务器初始化数值并授予技能；两端监听同一组属性变化，供本地界面读取。
void AmultiplayerCombatCharacter::BeginPlay()
{
	Super::BeginPlay();
	AbilitySystem->InitAbilityActorInfo(this, this);
	AbilitySystem->RegisterGameplayTagEvent(CombatTags::State_Defending, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &ThisClass::HandleDefenseTagChanged);
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

// 释放角色持有的技能任务与监听，避免退出或重开后旧回调继续执行。
void AmultiplayerCombatCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	AbilitySystem->RegisterGameplayTagEvent(CombatTags::State_Defending, EGameplayTagEventType::NewOrRemoved).RemoveAll(this);
	AbilitySystem->CancelAllAbilities();
	for (const FGameplayAttribute& Attribute : {
		UmultiplayerCombatAttributes::GetHealthAttribute(), UmultiplayerCombatAttributes::GetMaxHealthAttribute(),
		UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetMaxEnergyAttribute()})
	{
		AbilitySystem->GetGameplayAttributeValueChangeDelegate(Attribute).RemoveAll(this);
	}
	AbilitySystem->ClearActorInfo();
	Super::EndPlay(EndPlayReason);
}

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
}

// 死亡是持久状态，不仅是一条特效消息；晚加入也能禁止移动并恢复死亡表现。
void AmultiplayerCombatCharacter::OnRep_Dead()
{
	if (!bDead) return;
	AbilitySystem->AddLooseGameplayTag(CombatTags::State_Dead);
	AbilitySystem->CancelAllAbilities();
	FGameplayTagContainer DefenseTags(CombatTags::State_Defending);
	AbilitySystem->RemoveActiveEffectsWithGrantedTags(DefenseTags);
	GetCharacterMovement()->DisableMovement();
	SetActorEnableCollision(false);
	// 胶囊停止碰撞时同步结束占用重叠，压力板和胜利区域不再把尸体计作有效玩家。
	OnCombatDeath();
}

// 玩家默认不销毁 Pawn，保留控制器和界面；敌人覆盖此处清理 AI 与尸体。
void AmultiplayerCombatCharacter::HandleDeath()
{
}

// 阵营与死亡状态由服务器复制；属性集另行通过 GAS 的 RepNotify 同步。
void AmultiplayerCombatCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AmultiplayerCombatCharacter, CombatTeam, COND_InitialOnly);
	DOREPLIFETIME(AmultiplayerCombatCharacter, bDead);
}

// 接口与数值查询都读取现存组件，不创建临时状态。
UAbilitySystemComponent* AmultiplayerCombatCharacter::GetAbilitySystemComponent() const { return AbilitySystem; }
float AmultiplayerCombatCharacter::GetHealth() const { return CombatAttributes->GetHealth(); }
float AmultiplayerCombatCharacter::GetMaxHealth() const { return CombatAttributes->GetMaxHealth(); }
float AmultiplayerCombatCharacter::GetEnergy() const { return CombatAttributes->GetEnergy(); }
float AmultiplayerCombatCharacter::GetMaxEnergy() const { return CombatAttributes->GetMaxEnergy(); }
bool AmultiplayerCombatCharacter::IsAlive() const { return !bDead && GetHealth() > 0.0f; }
bool AmultiplayerCombatCharacter::IsDefending() const { return IsAlive() && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Defending); }

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

// 输入使用 GAS 的拥有者请求通道，服务器仍会重新检查技能条件。
void AmultiplayerCombatCharacter::Attack() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerAttackAbility::StaticClass()); }
void AmultiplayerCombatCharacter::Defend() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerDefendAbility::StaticClass()); }
void AmultiplayerCombatCharacter::Heal() { AbilitySystem->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()); }

// 死亡玩家不能通过原有跳跃输入重新进入移动状态。
void AmultiplayerCombatCharacter::Jump()
{
	if (IsAlive()) Super::Jump();
}

// 方向取服务器持有的控制器朝向，不接受客户端提供的伤害、起点或命中对象。
bool AmultiplayerCombatCharacter::FireProjectile(float Damage, float Speed)
{
	if (!HasAuthority() || !IsAlive()) return false;
	UWorld* World = GetWorld();
	const FRotator Aim = Controller ? Controller->GetControlRotation() : GetActorRotation();
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
