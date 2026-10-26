// Copyright Epic Games, Inc. All Rights Reserved.
// 倒地状态由组件提交，救援时序由服务器 GAS 能力持有。
// 所有外部通知都读取已提交的状态；结束流程统一释放占用和定时器。
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatAbilitySystem.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatTags.h"
#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	// 客户端与服务器使用同一时间尺度，倒计时不依赖本地入场时刻。
	double CoopTime(const UWorld* World)
	{
		const auto* State = World->GetGameState();
		return State ? State->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
	}
}

// 复制少量生命阶段数据，不启用逐帧轮询。
UmultiplayerCombatCoopComponent::UmultiplayerCombatCoopComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}
// 合作状态随所属角色生灭，不缓存跨 Pawn 的拥有者。
AmultiplayerCombatCharacter* UmultiplayerCombatCoopComponent::OwnerCharacter() const
{
	return Cast<AmultiplayerCombatCharacter>(GetOwner());
}
// 复制倒地、救援目标和截止时间，让两端重建相同显示。
void UmultiplayerCombatCoopComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, bDowned);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, DownedUntil);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, RescueTarget);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, RescueUntil);
}
// 只有仍能战斗的友方玩家可以让致死伤害转为倒地，敌人不算救援后备。
bool UmultiplayerCombatCoopComponent::HasActivePartner() const
{
	const auto* Character = OwnerCharacter();
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
		if (*It != Character && It->IsPlayerControlled() && !Character->IsHostileTo(*It) && It->IsCombatReady()) return true;
	return false;
}
// 剩余时间由服务器截止时刻推算，负值只显示为零。
float UmultiplayerCombatCoopComponent::GetDownedRemaining() const
{
	return bDowned ? FMath::Max(0.f, float(DownedUntil - CoopTime(GetWorld()))) : 0.f;
}
// 进度只用于显示；达到一并不绕过服务器的到时与占用检查。
float UmultiplayerCombatCoopComponent::GetRescueProgress() const
{
	return RescueTarget ? FMath::Clamp(1.f - float(RescueUntil - CoopTime(GetWorld())) / OwnerCharacter()->GetCombatConfig().RescueDuration, 0.f, 1.f) : 0.f;
}
// 血量归零的同一权威调用栈内先提交倒地，再写回占位生命，回调不会再进入一次倒地。
bool UmultiplayerCombatCoopComponent::TryEnterDowned()
{
	auto* Character = OwnerCharacter();
	if (!GetOwner()->HasAuthority() || !bEnableDowned || bDowned || !Character->IsPlayerControlled() || !HasActivePartner()) return false;
	bDowned = true;
	DownedUntil = CoopTime(GetWorld()) + Character->GetCombatConfig().BleedoutDuration;
	auto* ASC = Character->GetAbilitySystemComponent();
	ASC->CancelAllAbilities();
	FGameplayTagContainer Transient;
	for (FGameplayTag Tag : {CombatTags::State_Burning.GetTag(), CombatTags::State_Rooted.GetTag(), CombatTags::State_Stunned.GetTag(),
		CombatTags::State_Defending.GetTag(), CombatTags::State_Parrying.GetTag()}) Transient.AddTag(Tag);
	ASC->RemoveActiveEffectsWithGrantedTags(Transient);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute(), 0.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 1.f);
	GetWorld()->GetTimerManager().SetTimer(BleedoutTimer, this, &ThisClass::BleedOut, Character->GetCombatConfig().BleedoutDuration);
	NotifyLifeState();
	return true;
}
// 不重新给完整失血时间；离线已经到期的倒地直接进入终局死亡。
void UmultiplayerCombatCoopComponent::RestoreDowned(float Remaining)
{
	if (!GetOwner()->HasAuthority()) return;
	bDowned = true;
	DownedUntil = CoopTime(GetWorld()) + FMath::Max(0.f, Remaining);
	NotifyLifeState();
	if (Remaining <= 0.f) BleedOut();
	else GetWorld()->GetTimerManager().SetTimer(BleedoutTimer, this, &ThisClass::BleedOut, Remaining);
}
// 权威写入与 RepNotify 共用刷新路径，标签只表达当前阶段，不再另存权限布尔值。
void UmultiplayerCombatCoopComponent::NotifyLifeState()
{
	if (auto* Character = OwnerCharacter())
	{
		Character->GetAbilitySystemComponent()->SetLooseGameplayTagCount(CombatTags::State_Downed, bDowned ? 1 : 0);
		Character->SetActorEnableCollision(Character->IsCombatReady());
		Character->RefreshCombatPermissions();
		Character->RequestLifePresentationRefresh();
	}
}
// 救援距离和视线只查询当前世界，不修改目标占用或预先恢复生命。
bool UmultiplayerCombatCoopComponent::CanReach(const AmultiplayerCombatCharacter* Target) const
{
	const auto* Character = OwnerCharacter();
	if (!IsValid(Target) || Character == Target || Character->IsHostileTo(Target)
		|| !Target->GetCoopComponent()->IsDowned() || !Target->IsAlive()
		|| FVector::DistSquared(Character->GetActorLocation(), Target->GetActorLocation()) > FMath::Square(Character->GetCombatConfig().RescueRange)) return false;
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatRescue), false, Character);
	Query.AddIgnoredActor(Target);
	return !GetWorld()->LineTraceSingleByChannel(Hit, Character->GetActorLocation(), Target->GetActorLocation(), ECC_Visibility, Query);
}
// 按真实位置选择最近的可救援队友，已被占用者不参加候选。
AmultiplayerCombatCharacter* UmultiplayerCombatCoopComponent::FindRescueTarget() const
{
	AmultiplayerCombatCharacter* Found = nullptr;
	float Best = TNumericLimits<float>::Max();
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
	{
		if (It->GetCoopComponent()->CurrentRescuer.IsValid() || !CanReach(*It)) continue;
		const float Distance = FVector::DistSquared(GetOwner()->GetActorLocation(), It->GetActorLocation());
		if (Distance < Best) { Found = *It; Best = Distance; }
	}
	return Found;
}
// 准入查询不持有目标；真正开始时仍需重新选择并占用。
bool UmultiplayerCombatCoopComponent::CanStartRescue() const
{
	const auto* Character = OwnerCharacter();
	return Character && Character->IsCombatReady() && !Character->IsStunned() && !Character->IsCasting()
		&& !Character->IsMovementBlocked() && !RescueTarget && FindRescueTarget();
}
// [亮点] 服务器先写齐施救者 Target/Until 和被救者 CurrentRescuer，再通知本机监听者。
// [边界] 这是单服游戏线程内的独占规则；完成/取消只释放自己的占用，不是线程锁或客户端授权。
bool UmultiplayerCombatCoopComponent::BeginRescue()
{
	if (!GetOwner()->HasAuthority() || !CanStartRescue()) return false;
	RescueTarget = FindRescueTarget();
	if (!RescueTarget) return false;
	RescueTarget->GetCoopComponent()->CurrentRescuer = OwnerCharacter();
	RescueUntil = CoopTime(GetWorld()) + OwnerCharacter()->GetCombatConfig().RescueDuration;
	NotifyLifeState();
	return true;
}
// 引导过程中同时检查双方状态、可达性及令牌归属，不能只信开始时的快照。
bool UmultiplayerCombatCoopComponent::IsRescueValid() const
{
	return OwnerCharacter()->IsCombatReady() && !OwnerCharacter()->IsStunned() && CanReach(RescueTarget)
		&& RescueTarget->GetCoopComponent()->CurrentRescuer == OwnerCharacter();
}
// [亮点] 到时复核后先释放占用、退出倒地，再修改生命，属性回调不能重复救起同一目标。
bool UmultiplayerCombatCoopComponent::CompleteRescue()
{
	if (!GetOwner()->HasAuthority() || !IsRescueValid() || CoopTime(GetWorld()) + KINDA_SMALL_NUMBER < RescueUntil) return false;
	auto* Target = RescueTarget.Get();
	auto* Life = Target->GetCoopComponent();
	Life->CurrentRescuer.Reset();
	Life->bDowned = false;
	Life->DownedUntil = 0;
	GetWorld()->GetTimerManager().ClearTimer(Life->BleedoutTimer);
	Target->GetAbilitySystemComponent()->SetLooseGameplayTagCount(CombatTags::State_Downed, 0);
	Target->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), Target->GetMaxHealth() * .3f);
	Life->NotifyLifeState();
	return true;
}
// 结束只释放属于自己的占用；目标已被其他流程接管时不清除他人的令牌。
void UmultiplayerCombatCoopComponent::EndRescue()
{
	if (IsValid(RescueTarget) && RescueTarget->GetCoopComponent()->CurrentRescuer == OwnerCharacter())
		RescueTarget->GetCoopComponent()->CurrentRescuer.Reset();
	RescueTarget = nullptr;
	RescueUntil = 0;
	NotifyLifeState();
}
// 死亡与销毁共用清理，先解除倒地再取消施救者，避免其回调继续救援。
void UmultiplayerCombatCoopComponent::ClearLifeState()
{
	GetWorld()->GetTimerManager().ClearTimer(BleedoutTimer);
	bDowned = false;
	DownedUntil = 0;
	if (auto* Rescuer = CurrentRescuer.Get()) Rescuer->CancelRescue();
	CurrentRescuer.Reset();
	EndRescue();
}
// 服务器失血终结倒地，临时关闭再次倒地资格后通过属性入口触发死亡。
void UmultiplayerCombatCoopComponent::BleedOut()
{
	if (!bDowned || !GetOwner()->HasAuthority()) return;
	auto* Character = OwnerCharacter();
	ClearLifeState();
	// 这次归零是终局，不再尝试进入第二次倒地。
	const bool Previous = bEnableDowned; bEnableDowned = false;
	Character->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	bEnableDowned = Previous;
}
// 生命周期退出也执行同一套占用清理，不遗留失血任务或双向引用。
void UmultiplayerCombatCoopComponent::EndPlay(EEndPlayReason::Type Reason)
{
	ClearLifeState();
	Super::EndPlay(Reason);
}

// 救援不做客户端结果预测，服务器能力持有引导标签直到统一结束。
UmultiplayerRescueAbility::UmultiplayerRescueAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Rescue));
	ActivationOwnedTags.AddTag(CombatTags::State_Rescuing);
}
// 业务预检只读目标资格，仍由 Super 校验原生阻断关系和激活条件。
bool UmultiplayerRescueAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
	const FGameplayTagContainer* Source, const FGameplayTagContainer* Target, FGameplayTagContainer* Reasons) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(Info->AvatarActor.Get());
	return !bEndingRescue && Character && Character->GetCoopComponent()->CanStartRescue()
		&& Super::CanActivateAbility(Handle, Info, Source, Target, Reasons);
}
// 救援不继承战斗消耗基类，但使用相同关系查询；其倒地目标/占用校验仍由合作组件负责。
bool UmultiplayerRescueAbility::DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
	const FGameplayTagContainer* Source, const FGameplayTagContainer* Target, FGameplayTagContainer* Reasons) const
{
	const auto* CombatASC = Cast<UmultiplayerCombatAbilitySystem>(&ASC);
	return (!CombatASC || CombatASC->CheckAbilityTagRelationships(GetAssetTags(), Reasons))
		&& Super::DoesAbilitySatisfyTagRequirements(ASC, Source, Target, Reasons);
}
// 先取得救援占用，再建立等待与巡检任务；任一步的同步取消都不能继续引导。
void UmultiplayerRescueAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
	FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event)
{
	// 替换普攻或占用通知都可能同步取消救援；取消后不能再创建引导任务。
	if (!IsActive()) return;
	auto* Character = Cast<AmultiplayerCombatCharacter>(Info->AvatarActor.Get());
	if (!Character || !Info->IsNetAuthority() || !Character->GetCoopComponent()->BeginRescue())
	{ EndAbility(Handle, Info, Activation, true, true); return; }
	if (!IsActive()) return;
	auto* Delay = UAbilityTask_WaitDelay::WaitDelay(this, Character->GetCombatConfig().RescueDuration);
	Delay->OnFinish.AddDynamic(this, &ThisClass::FinishRescue);
	GetWorld()->GetTimerManager().SetTimer(ValidationTimer, this, &ThisClass::CheckRescue, .1f, true);
	Delay->ReadyForActivation();
}
// 巡检只负责中断失效引导，不负责提前结算或恢复目标生命。
void UmultiplayerRescueAbility::CheckRescue()
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	if (!Character || !Character->GetCoopComponent()->IsRescueValid())
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}
// 到时后由合作组件复核并结算，随后统一释放任务与占用。
void UmultiplayerRescueAbility::FinishRescue()
{
	auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	const bool Success = Character && Character->GetCoopComponent()->CompleteRescue();
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, !Success);
}
// [职责] 统一取消巡检、释放本次占用并结束 GAS 任务；作用域锁与结束保护覆盖同步重入。
void UmultiplayerRescueAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
	FGameplayAbilityActivationInfo Activation, bool bReplicate, bool bCancelled)
{
	if (bEndingRescue || !IsEndAbilityValid(Handle, Info)) return;
	if (ScopeLockCount > 0)
	{
		WaitingToExecute.Add(FPostLockDelegate::CreateUObject(this, &ThisClass::EndAbility,
			Handle, Info, Activation, bReplicate, bCancelled));
		return;
	}
	// 释放占用会同步通知 HUD/移动/外部监听；重入取消不能重复收尾，也不能激活半清理的实例。
	TGuardValue<bool> EndingGuard(bEndingRescue, true);
	GetWorld()->GetTimerManager().ClearTimer(ValidationTimer);
	if (auto* Character = Cast<AmultiplayerCombatCharacter>(Info->AvatarActor.Get())) Character->GetCoopComponent()->EndRescue();
	Super::EndAbility(Handle, Info, Activation, bReplicate, bCancelled);
}
