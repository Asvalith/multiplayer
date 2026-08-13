// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGASTargetDummy.h"

#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "multiplayer.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerGASTargetDummy::AmultiplayerGASTargetDummy()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);

	TargetMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TargetMesh"));
	SetRootComponent(TargetMesh);
	TargetMesh->SetMobility(EComponentMobility::Movable);
	TargetMesh->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	TargetMesh->SetRelativeScale3D(FVector(0.75f, 0.75f, 1.5f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		TargetMesh->SetStaticMesh(CubeMesh.Object);
	}

	GameplayCueFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GameplayCueFlashLight"));
	GameplayCueFlashLight->SetupAttachment(TargetMesh);
	GameplayCueFlashLight->SetRelativeLocation(FVector(0.0f, 0.0f, 80.0f));
	GameplayCueFlashLight->SetAttenuationRadius(260.0f);
	GameplayCueFlashLight->SetIntensity(0.0f);
	GameplayCueFlashLight->SetVisibility(false);

	GameplayCueStateLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GameplayCueStateLight"));
	GameplayCueStateLight->SetupAttachment(TargetMesh);
	GameplayCueStateLight->SetRelativeLocation(FVector(0.0f, 0.0f, 110.0f));
	GameplayCueStateLight->SetAttenuationRadius(200.0f);
	GameplayCueStateLight->SetIntensity(0.0f);
	GameplayCueStateLight->SetVisibility(false);

	AbilitySystemComponent = CreateDefaultSubobject<UmultiplayerAbilitySystemComponent>(
		TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Full);

	AttributeSet = CreateDefaultSubobject<UmultiplayerAttributeSet>(TEXT("AttributeSet"));
}

void AmultiplayerGASTargetDummy::BeginPlay()
{
	Super::BeginPlay();

	AbilitySystemComponent->InitAbilityActorInfo(this, this);
	ApplyTeamIdentity();
	AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerAttributeSet::GetHealthAttribute()).AddUObject(
		this,
		&AmultiplayerGASTargetDummy::HandleHealthChanged);
	UpdateBaselineVisuals(GetHealth());

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_TARGET_READY Target=%s Role=%s Health=%.1f Team=Team.Enemy"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		GetHealth());
}

void AmultiplayerGASTargetDummy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(GameplayCueFlashTimer);
	ClearGameplayCueFlash();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerGASTargetDummy::GameplayCueDefaultHandler(
	EGameplayCueEvent::Type EventType,
	const FGameplayCueParameters& Parameters)
{
	const FGameplayTag CueTag = Parameters.OriginalTag;
	bool bHandled = true;
	bool bCritical = false;
	EmultiplayerHitType HitType = EmultiplayerHitType::Direct;
	FVector ImpactImpulse = FVector::ZeroVector;

	if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact)
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
			bCritical ? FLinearColor(1.0f, 0.12f, 0.0f) : FLinearColor(1.0f, 0.0f, 0.0f),
			bCritical ? 9000.0f : 5500.0f,
			bCritical ? 0.4f : 0.25f);
	}
	else if (CueTag.MatchesTagExact(
		MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability))
	{
		if (EventType == EGameplayCueEvent::Removed)
		{
			SetVulnerabilityCueVisible(false);
		}
		else if (EventType == EGameplayCueEvent::OnActive
			|| EventType == EGameplayCueEvent::WhileActive)
		{
			SetVulnerabilityCueVisible(true);
		}
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Death)
		&& EventType == EGameplayCueEvent::Executed)
	{
		bDeathCueActive = true;
		RefreshGameplayCueState();
	}
	else
	{
		bHandled = false;
	}

	if (!bHandled)
	{
		IGameplayCueInterface::GameplayCueDefaultHandler(EventType, Parameters);
		return;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_CUE_HANDLER Actor=%s Cue=%s Event=%s Role=%s Local=false Critical=%s HitType=%s ImpactDir=%s"),
		*GetName(),
		*CueTag.ToString(),
		*UEnum::GetValueAsString(EventType),
		*UEnum::GetValueAsString(GetLocalRole()),
		bCritical ? TEXT("true") : TEXT("false"),
		*UEnum::GetValueAsString(HitType),
		*ImpactImpulse.GetSafeNormal().ToCompactString());
}

void AmultiplayerGASTargetDummy::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerGASTargetDummy, CoopTeamId);
}

void AmultiplayerGASTargetDummy::OnRep_CoopTeamId()
{
	ApplyTeamIdentity();
}

void AmultiplayerGASTargetDummy::ApplyTeamIdentity()
{
	if (AbilitySystemComponent == nullptr)
	{
		return;
	}

	AbilitySystemComponent->SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Team_Player,
		CoopTeamId == MultiplayerTeams::Players ? 1 : 0);
	AbilitySystemComponent->SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Team_Enemy,
		CoopTeamId == MultiplayerTeams::Enemies ? 1 : 0);
}

UAbilitySystemComponent* AmultiplayerGASTargetDummy::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

float AmultiplayerGASTargetDummy::GetHealth() const
{
	return AttributeSet != nullptr ? AttributeSet->GetHealth() : 0.0f;
}

void AmultiplayerGASTargetDummy::ResetForBaseline(const FVector& NewLocation)
{
	if (!HasAuthority() || AttributeSet == nullptr || AbilitySystemComponent == nullptr)
	{
		return;
	}

	SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	FGameplayTagContainer TransientTags;
	TransientTags.AddTag(MultiplayerGameplayTags::Effect_Negative_Vulnerability);
	TransientTags.AddTag(MultiplayerGameplayTags::State_Vulnerable);
	AbilitySystemComponent->RemoveActiveEffectsWithTags(TransientTags);
	bDeathCueActive = false;
	SetVulnerabilityCueVisible(false);
	ClearGameplayCueFlash();
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetHealthAttribute(),
		AttributeSet->GetMaxHealth());
	ForceNetUpdate();

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_TARGET_RESET Target=%s Health=%.1f Location=%s"),
		*GetName(),
		GetHealth(),
		*NewLocation.ToCompactString());
}

void AmultiplayerGASTargetDummy::HandleHealthChanged(const FOnAttributeChangeData& ChangeData)
{
	UpdateBaselineVisuals(ChangeData.NewValue);

	if (HasAuthority() && ChangeData.OldValue > 0.0f && ChangeData.NewValue <= 0.0f)
	{
		FGameplayTagContainer VulnerabilityTags;
		VulnerabilityTags.AddTag(MultiplayerGameplayTags::Effect_Negative_Vulnerability);
		VulnerabilityTags.AddTag(MultiplayerGameplayTags::State_Vulnerable);
		AbilitySystemComponent->RemoveActiveEffectsWithTags(VulnerabilityTags);
		AbilitySystemComponent->ExecuteGameplayCue(
			MultiplayerGameplayTags::GameplayCue_Coop_Death);
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_TARGET_HEALTH Target=%s Role=%s Old=%.1f New=%.1f"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		ChangeData.OldValue,
		ChangeData.NewValue);
}

void AmultiplayerGASTargetDummy::ShowGameplayCueFlash(
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
	GetWorldTimerManager().SetTimer(
		GameplayCueFlashTimer,
		this,
		&AmultiplayerGASTargetDummy::ClearGameplayCueFlash,
		FMath::Max(Duration, 0.01f),
		false);
}

void AmultiplayerGASTargetDummy::ClearGameplayCueFlash()
{
	if (GameplayCueFlashLight != nullptr)
	{
		GameplayCueFlashLight->SetIntensity(0.0f);
		GameplayCueFlashLight->SetVisibility(false);
	}
}

void AmultiplayerGASTargetDummy::SetVulnerabilityCueVisible(bool bVisible)
{
	bVulnerabilityCueActive = bVisible;
	RefreshGameplayCueState();
}

void AmultiplayerGASTargetDummy::RefreshGameplayCueState()
{
	if (GameplayCueStateLight == nullptr)
	{
		return;
	}

	if (bDeathCueActive)
	{
		GameplayCueStateLight->SetLightColor(FLinearColor(1.0f, 0.0f, 0.0f));
		GameplayCueStateLight->SetIntensity(7500.0f);
		GameplayCueStateLight->SetVisibility(true);
	}
	else if (bVulnerabilityCueActive)
	{
		GameplayCueStateLight->SetLightColor(FLinearColor(0.75f, 0.0f, 1.0f));
		GameplayCueStateLight->SetIntensity(3600.0f);
		GameplayCueStateLight->SetVisibility(true);
	}
	else
	{
		GameplayCueStateLight->SetIntensity(0.0f);
		GameplayCueStateLight->SetVisibility(false);
	}
}

void AmultiplayerGASTargetDummy::PositionGameplayCueFlashFromImpact(
	const FVector& ImpactImpulse)
{
	if (GameplayCueFlashLight == nullptr || ImpactImpulse.IsNearlyZero())
	{
		return;
	}

	const FVector IncomingDirection = ImpactImpulse.GetSafeNormal();
	GameplayCueFlashLight->SetWorldLocation(
		GetActorLocation() - IncomingDirection * 55.0f + FVector(0.0f, 0.0f, 80.0f));
}

void AmultiplayerGASTargetDummy::UpdateBaselineVisuals(float CurrentHealth)
{
	if (TargetMesh == nullptr || AttributeSet == nullptr)
	{
		return;
	}

	const float MaxHealth = FMath::Max(AttributeSet->GetMaxHealth(), 1.0f);
	const float HealthFraction = FMath::Clamp(CurrentHealth / MaxHealth, 0.0f, 1.0f);
	const bool bIsAlive = CurrentHealth > 0.0f;
	bDeathCueActive = !bIsAlive;
	RefreshGameplayCueState();

	TargetMesh->SetVisibility(bIsAlive, false);
	TargetMesh->SetCollisionEnabled(
		bIsAlive ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	TargetMesh->SetRelativeScale3D(
		FVector(0.75f, 0.75f, FMath::Lerp(0.2f, 1.5f, HealthFraction)));

	if (GEngine != nullptr)
	{
		const FString Status = bIsAlive
			? FString::Printf(TEXT("Enemy Target HP: %.0f / %.0f"), CurrentHealth, MaxHealth)
			: TEXT("Enemy Target defeated - press 7 to reset");
		GEngine->AddOnScreenDebugMessage(
			static_cast<uint64>(GetUniqueID()),
			2.5f,
			bIsAlive ? FColor::Red : FColor::Green,
			Status);
	}
}
