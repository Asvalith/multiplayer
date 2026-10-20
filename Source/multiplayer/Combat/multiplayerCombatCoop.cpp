// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatCoop.h"
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
	double CoopTime(const UWorld* World)
	{
		const auto* State = World->GetGameState();
		return State ? State->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
	}
}

UmultiplayerCombatCoopComponent::UmultiplayerCombatCoopComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}
AmultiplayerCombatCharacter* UmultiplayerCombatCoopComponent::OwnerCharacter() const
{
	return Cast<AmultiplayerCombatCharacter>(GetOwner());
}
void UmultiplayerCombatCoopComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, bDowned);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, DownedUntil);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, RescueTarget);
	DOREPLIFETIME(UmultiplayerCombatCoopComponent, RescueUntil);
}
bool UmultiplayerCombatCoopComponent::HasActivePartner() const
{
	const auto* Character = OwnerCharacter();
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
		if (*It != Character && It->IsPlayerControlled() && !Character->IsHostileTo(*It) && It->IsCombatReady()) return true;
	return false;
}
float UmultiplayerCombatCoopComponent::GetDownedRemaining() const
{
	return bDowned ? FMath::Max(0.f, float(DownedUntil - CoopTime(GetWorld()))) : 0.f;
}
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
bool UmultiplayerCombatCoopComponent::CanStartRescue() const
{
	const auto* Character = OwnerCharacter();
	return Character && Character->IsCombatReady() && !Character->IsStunned() && !Character->IsCasting()
		&& !Character->IsMovementBlocked() && !RescueTarget && FindRescueTarget();
}
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
bool UmultiplayerCombatCoopComponent::IsRescueValid() const
{
	return OwnerCharacter()->IsCombatReady() && !OwnerCharacter()->IsStunned() && CanReach(RescueTarget)
		&& RescueTarget->GetCoopComponent()->CurrentRescuer == OwnerCharacter();
}
// 先取回并释放独占令牌，再提交生命与状态，第二名救援者或晚到回调无法重复结算。
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
void UmultiplayerCombatCoopComponent::EndRescue()
{
	if (IsValid(RescueTarget) && RescueTarget->GetCoopComponent()->CurrentRescuer == OwnerCharacter())
		RescueTarget->GetCoopComponent()->CurrentRescuer.Reset();
	RescueTarget = nullptr;
	RescueUntil = 0;
	NotifyLifeState();
}
void UmultiplayerCombatCoopComponent::ClearLifeState()
{
	GetWorld()->GetTimerManager().ClearTimer(BleedoutTimer);
	bDowned = false;
	DownedUntil = 0;
	if (auto* Rescuer = CurrentRescuer.Get()) Rescuer->CancelRescue();
	CurrentRescuer.Reset();
	EndRescue();
}
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
void UmultiplayerCombatCoopComponent::EndPlay(EEndPlayReason::Type Reason)
{
	ClearLifeState();
	Super::EndPlay(Reason);
}

UmultiplayerRescueAbility::UmultiplayerRescueAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	SetAssetTags(FGameplayTagContainer(CombatTags::Ability_Rescue));
	ActivationOwnedTags.AddTag(CombatTags::State_Rescuing);
}
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
void UmultiplayerRescueAbility::CheckRescue()
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	if (!Character || !Character->GetCoopComponent()->IsRescueValid())
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}
void UmultiplayerRescueAbility::FinishRescue()
{
	auto* Character = Cast<AmultiplayerCombatCharacter>(GetAvatarActorFromActorInfo());
	const bool Success = Character && Character->GetCoopComponent()->CompleteRescue();
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, !Success);
}
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
