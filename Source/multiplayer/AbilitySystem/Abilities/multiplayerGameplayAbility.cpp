// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "multiplayer.h"
#include "multiplayerCharacter.h"

UmultiplayerGameplayAbility::UmultiplayerGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
}

UmultiplayerDamageAbility::UmultiplayerDamageAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Damage);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerDamageCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDamageCooldownEffect::StaticClass();
}

void UmultiplayerDamageAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	UmultiplayerAbilityTask_TargetActor* TargetTask =
		UmultiplayerAbilityTask_TargetActor::CreateTargetActorTask(this, TargetRange);
	if (TargetTask == nullptr)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	TargetTask->ValidData.AddDynamic(this, &UmultiplayerDamageAbility::HandleTargetData);
	TargetTask->ReadyForActivation();
}

void UmultiplayerDamageAbility::HandleTargetData(
	const FGameplayAbilityTargetDataHandle& TargetData)
{
	AActor* TargetActor = nullptr;
	if (TargetData.Num() > 0 && TargetData.Get(0) != nullptr)
	{
		const TArray<TWeakObjectPtr<AActor>> Actors = TargetData.Get(0)->GetActors();
		TargetActor = Actors.Num() > 0 ? Actors[0].Get() : nullptr;
	}

	const bool bIsAuthority = CurrentActorInfo != nullptr && CurrentActorInfo->IsNetAuthority();
	const bool bTargetIsValid = bIsAuthority
		? IsServerTargetValid(TargetActor)
		: TargetActor != nullptr;
	if (!bTargetIsValid
		|| !CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	if (bIsAuthority)
	{
		UAbilitySystemComponent* TargetASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor);
		FGameplayEffectSpecHandle DamageSpec = MakeOutgoingGameplayEffectSpec(
			UmultiplayerDamageEffect::StaticClass(),
			GetAbilityLevel());
		if (TargetASC != nullptr && DamageSpec.IsValid())
		{
			DamageSpec.Data->SetSetByCallerMagnitude(
				MultiplayerGameplayTags::Data_Damage,
				DamageAmount);
			GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToTarget(
				*DamageSpec.Data.Get(),
				TargetASC);

			UE_LOG(
				LogMultiplayerGAS,
				Log,
				TEXT("Server applied %.1f damage: %s -> %s"),
				DamageAmount,
				*GetNameSafe(GetAvatarActorFromActorInfo()),
				*GetNameSafe(TargetActor));
		}
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

bool UmultiplayerDamageAbility::IsServerTargetValid(AActor* TargetActor) const
{
	AActor* AvatarActor = GetAvatarActorFromActorInfo();
	UWorld* World = AvatarActor != nullptr ? AvatarActor->GetWorld() : nullptr;
	AmultiplayerCharacter* TargetCharacter = Cast<AmultiplayerCharacter>(TargetActor);
	if (World == nullptr
		|| TargetCharacter == nullptr
		|| TargetActor == AvatarActor
		|| TargetActor->IsActorBeingDestroyed())
	{
		return false;
	}

	if (FVector::DistSquared(AvatarActor->GetActorLocation(), TargetActor->GetActorLocation())
		> FMath::Square(TargetRange + 50.0f))
	{
		return false;
	}

	UAbilitySystemComponent* TargetASC = TargetCharacter->GetAbilitySystemComponent();
	if (TargetASC == nullptr
		|| TargetASC->GetNumericAttribute(UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GASDamageTargetValidation), false, AvatarActor);
	FHitResult HitResult;
	const bool bBlocked = World->LineTraceSingleByChannel(
		HitResult,
		AvatarActor->GetActorLocation(),
		TargetActor->GetActorLocation(),
		ECC_Visibility,
		QueryParams);

	return !bBlocked || HitResult.GetActor() == TargetActor;
}

UmultiplayerHealAbility::UmultiplayerHealAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Heal);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerHealCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerHealCooldownEffect::StaticClass();
}

void UmultiplayerHealAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (ActorInfo != nullptr && ActorInfo->IsNetAuthority())
	{
		FGameplayEffectSpecHandle HealSpec = MakeOutgoingGameplayEffectSpec(
			UmultiplayerHealingEffect::StaticClass(),
			GetAbilityLevel());
		if (HealSpec.IsValid())
		{
			HealSpec.Data->SetSetByCallerMagnitude(
				MultiplayerGameplayTags::Data_Heal,
				HealingAmount);
			GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToSelf(
				*HealSpec.Data.Get());
			UE_LOG(
				LogMultiplayerGAS,
				Log,
				TEXT("Server applied %.1f healing to %s"),
				HealingAmount,
				*GetNameSafe(GetAvatarActorFromActorInfo()));
		}
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

UmultiplayerImmunityAbility::UmultiplayerImmunityAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Immunity);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerImmunityCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerImmunityCooldownEffect::StaticClass();
}

void UmultiplayerImmunityAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ApplyGameplayEffectToOwner(
		Handle,
		ActorInfo,
		ActivationInfo,
		UmultiplayerImmunityEffect::StaticClass()->GetDefaultObject<UGameplayEffect>(),
		GetAbilityLevel());

	UE_LOG(
		LogMultiplayerGAS,
		Log,
		TEXT("%s applied predicted immunity to %s"),
		ActorInfo != nullptr && ActorInfo->IsNetAuthority() ? TEXT("Server") : TEXT("Client"),
		*GetNameSafe(GetAvatarActorFromActorInfo()));

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
