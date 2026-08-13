// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/multiplayerGASCuePresenterComponent.h"

#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/Pawn.h"
#include "multiplayer.h"

UmultiplayerGASCuePresenterComponent::UmultiplayerGASCuePresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerGASCuePresenterComponent::BindLights(
	UPointLightComponent* InGameplayCueFlashLight,
	UPointLightComponent* InGameplayCueStateLight)
{
	if (GameplayCueFlashLight != nullptr
		&& GameplayCueFlashLight != InGameplayCueFlashLight)
	{
		GameplayCueFlashLight->SetIntensity(0.0f);
		GameplayCueFlashLight->SetVisibility(false);
	}
	if (GameplayCueStateLight != nullptr
		&& GameplayCueStateLight != InGameplayCueStateLight)
	{
		GameplayCueStateLight->SetIntensity(0.0f);
		GameplayCueStateLight->SetVisibility(false);
	}

	GameplayCueFlashLight = InGameplayCueFlashLight;
	GameplayCueStateLight = InGameplayCueStateLight;
	ClearGameplayCueFlash();
	RefreshGameplayCueState();
}

void UmultiplayerGASCuePresenterComponent::BindAbilitySystem(
	UmultiplayerAbilitySystemComponent* InAbilitySystemComponent)
{
	if (AbilitySystemComponent == InAbilitySystemComponent)
	{
		return;
	}

	UnbindAbilitySystem();
	AbilitySystemComponent = InAbilitySystemComponent;
	if (AbilitySystemComponent != nullptr)
	{
		PredictionLabReconciledHandle =
			AbilitySystemComponent->OnPredictionLabReconciled().AddUObject(
				this,
				&UmultiplayerGASCuePresenterComponent::HandlePredictionLabReconciled);
	}
}

bool UmultiplayerGASCuePresenterComponent::HandleGameplayCue(
	EGameplayCueEvent::Type EventType,
	const FGameplayCueParameters& Parameters)
{
	const FGameplayTag CueTag = Parameters.OriginalTag;
	bool bHandled = true;
	bool bCritical = false;
	EmultiplayerHitType HitType = EmultiplayerHitType::Direct;
	FVector ImpactImpulse = FVector::ZeroVector;

	if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Damage_Cast)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(1.0f, 0.75f, 0.05f), 4500.0f, 0.18f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact)
		&& EventType == EGameplayCueEvent::Executed)
	{
		const FGameplayEffectContext* BaseContext = Parameters.EffectContext.Get();
		const FmultiplayerGameplayEffectContext* ProjectContext =
			BaseContext != nullptr
			&& BaseContext->GetScriptStruct()->IsChildOf(
				FmultiplayerGameplayEffectContext::StaticStruct())
				? static_cast<const FmultiplayerGameplayEffectContext*>(BaseContext)
				: nullptr;
		bCritical = ProjectContext != nullptr && ProjectContext->IsCriticalHit();
		if (ProjectContext != nullptr)
		{
			HitType = ProjectContext->GetHitType();
			ImpactImpulse = ProjectContext->GetImpactImpulse();
			PositionGameplayCueFlashFromImpact(ImpactImpulse);
		}
		ShowGameplayCueFlash(
			bCritical ? FLinearColor(1.0f, 0.15f, 0.0f) : FLinearColor(1.0f, 0.0f, 0.0f),
			bCritical ? 8000.0f : 5000.0f,
			bCritical ? 0.35f : 0.22f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Heal_Cast)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(0.1f, 0.6f, 1.0f), 4200.0f, 0.18f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Heal_Result)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(0.0f, 1.0f, 0.15f), 6000.0f, 0.3f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_State_Immunity))
	{
		HandlePersistentCueEvent(bGameplayCueImmunityActive, EventType);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability))
	{
		HandlePersistentCueEvent(bGameplayCueVulnerabilityActive, EventType);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending))
	{
		HandlePersistentCueEvent(bGameplayCuePredictionPendingActive, EventType);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Death)
		&& EventType == EGameplayCueEvent::Executed)
	{
		bGameplayCueDeathActive = true;
		RefreshGameplayCueState();
	}
	else
	{
		bHandled = false;
	}

	if (!bHandled)
	{
		return false;
	}

	const AActor* OwnerActor = GetOwner();
	const APawn* OwnerPawn = Cast<APawn>(OwnerActor);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_CUE_HANDLER Actor=%s Cue=%s Event=%s Role=%s Local=%s Critical=%s HitType=%s ImpactDir=%s"),
		*GetNameSafe(OwnerActor),
		*CueTag.ToString(),
		*UEnum::GetValueAsString(EventType),
		OwnerActor != nullptr
			? *UEnum::GetValueAsString(OwnerActor->GetLocalRole())
			: TEXT("None"),
		OwnerPawn != nullptr && OwnerPawn->IsLocallyControlled() ? TEXT("true") : TEXT("false"),
		bCritical ? TEXT("true") : TEXT("false"),
		*UEnum::GetValueAsString(HitType),
		*ImpactImpulse.GetSafeNormal().ToCompactString());
	return true;
}

void UmultiplayerGASCuePresenterComponent::ApplyDeathState(bool bNewDeadState)
{
	bGameplayCueDeathActive = bNewDeadState;
	if (!bNewDeadState)
	{
		bGameplayCueImmunityActive = false;
		bGameplayCueVulnerabilityActive = false;
		bGameplayCuePredictionPendingActive = false;
	}
	RefreshGameplayCueState();
}

void UmultiplayerGASCuePresenterComponent::ReconcilePredictionLabPendingPresentation(
	const TCHAR* Outcome,
	int16 PredictionKey)
{
	bGameplayCuePredictionPendingActive = false;
	RefreshGameplayCueState();
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_VISUAL Phase=PendingCleared Outcome=%s PredictionKey=%d Actor=%s PendingVisual=false"),
		Outcome,
		static_cast<int32>(PredictionKey),
		*GetNameSafe(GetOwner()));
}

void UmultiplayerGASCuePresenterComponent::ClearPresentation()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(GameplayCueFlashTimer);
	}

	bGameplayCueImmunityActive = false;
	bGameplayCueVulnerabilityActive = false;
	bGameplayCueDeathActive = false;
	bGameplayCuePredictionPendingActive = false;
	ClearGameplayCueFlash();
	ClearGameplayCueState();
}

void UmultiplayerGASCuePresenterComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	UnbindAbilitySystem();
	ClearPresentation();
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerGASCuePresenterComponent::HandlePredictionLabReconciled(
	bool bRejected,
	int16 PredictionKey)
{
	ReconcilePredictionLabPendingPresentation(
		bRejected ? TEXT("Rejected") : TEXT("CaughtUp"),
		PredictionKey);
}

void UmultiplayerGASCuePresenterComponent::UnbindAbilitySystem()
{
	if (AbilitySystemComponent != nullptr
		&& PredictionLabReconciledHandle.IsValid())
	{
		AbilitySystemComponent->OnPredictionLabReconciled().Remove(
			PredictionLabReconciledHandle);
	}
	PredictionLabReconciledHandle.Reset();
	AbilitySystemComponent = nullptr;
}

void UmultiplayerGASCuePresenterComponent::ShowGameplayCueFlash(
	const FLinearColor& Color,
	float Intensity,
	float Duration)
{
	if (GameplayCueFlashLight == nullptr)
	{
		return;
	}

	GameplayCueFlashLight->SetLightColor(Color);
	GameplayCueFlashLight->SetIntensity(Intensity);
	GameplayCueFlashLight->SetVisibility(true);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			GameplayCueFlashTimer,
			this,
			&UmultiplayerGASCuePresenterComponent::ClearGameplayCueFlash,
			FMath::Max(Duration, 0.01f),
			false);
	}
}

void UmultiplayerGASCuePresenterComponent::ClearGameplayCueFlash()
{
	if (GameplayCueFlashLight != nullptr)
	{
		GameplayCueFlashLight->SetIntensity(0.0f);
		GameplayCueFlashLight->SetVisibility(false);
	}
}

void UmultiplayerGASCuePresenterComponent::SetGameplayCueState(
	const FLinearColor& Color,
	float Intensity)
{
	if (GameplayCueStateLight != nullptr)
	{
		GameplayCueStateLight->SetLightColor(Color);
		GameplayCueStateLight->SetIntensity(Intensity);
		GameplayCueStateLight->SetVisibility(true);
	}
}

void UmultiplayerGASCuePresenterComponent::ClearGameplayCueState()
{
	if (GameplayCueStateLight != nullptr)
	{
		GameplayCueStateLight->SetIntensity(0.0f);
		GameplayCueStateLight->SetVisibility(false);
	}
}

void UmultiplayerGASCuePresenterComponent::HandlePersistentCueEvent(
	bool& bState,
	EGameplayCueEvent::Type EventType)
{
	if (EventType == EGameplayCueEvent::Removed)
	{
		bState = false;
		RefreshGameplayCueState();
	}
	else if (EventType == EGameplayCueEvent::OnActive
		|| EventType == EGameplayCueEvent::WhileActive)
	{
		bState = true;
		RefreshGameplayCueState();
	}
}

void UmultiplayerGASCuePresenterComponent::RefreshGameplayCueState()
{
	if (bGameplayCueDeathActive)
	{
		SetGameplayCueState(FLinearColor(1.0f, 0.0f, 0.0f), 6500.0f);
	}
	else if (bGameplayCuePredictionPendingActive)
	{
		SetGameplayCueState(FLinearColor(1.0f, 0.0f, 1.0f), 2500.0f);
	}
	else if (bGameplayCueImmunityActive)
	{
		SetGameplayCueState(FLinearColor(0.0f, 0.35f, 1.0f), 3200.0f);
	}
	else if (bGameplayCueVulnerabilityActive)
	{
		SetGameplayCueState(FLinearColor(0.75f, 0.0f, 1.0f), 3000.0f);
	}
	else
	{
		ClearGameplayCueState();
	}
}

void UmultiplayerGASCuePresenterComponent::PositionGameplayCueFlashFromImpact(
	const FVector& ImpactImpulse)
{
	const AActor* OwnerActor = GetOwner();
	if (GameplayCueFlashLight == nullptr
		|| OwnerActor == nullptr
		|| ImpactImpulse.IsNearlyZero())
	{
		return;
	}

	const FVector IncomingDirection = ImpactImpulse.GetSafeNormal();
	GameplayCueFlashLight->SetWorldLocation(
		OwnerActor->GetActorLocation()
		- IncomingDirection * 45.0f
		+ FVector(0.0f, 0.0f, 80.0f));
}
