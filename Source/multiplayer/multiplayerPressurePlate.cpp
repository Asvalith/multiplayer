// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerLog.h"
#include "multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerPressurePlate::AmultiplayerPressurePlate()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(5.0f);
	SetMinNetUpdateFrequency(2.0f);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	SceneRoot->SetMobility(EComponentMobility::Movable);

	PlateMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateMesh"));
	PlateMesh->SetupAttachment(SceneRoot);
	PlateMesh->SetMobility(EComponentMobility::Movable);
	PlateMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlateMesh->SetGenerateOverlapEvents(false);
	PlateMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->bEditableWhenInherited = true;
	ActivationTrigger->SetRelativeLocation(FVector(0.0f, 0.0f, 60.0f));
	ActivationTrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlateMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

	ReleasedRelativeLocation = PlateMesh->GetRelativeLocation();
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(
		ActivationTrigger,
		bRequirePlayerControlledCharacter);
	ApplyPlateState(true);

	if (!HasAuthority())
	{
		return;
	}

	if (bRequireObjectiveComplete)
	{
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this,
				&AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
		}
	}
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector TargetLocation =
		ReleasedRelativeLocation
		+ (bPlateActive ? PressedOffset : FVector::ZeroVector);
	const FVector NewLocation = FMath::VInterpConstantTo(
		PlateMesh->GetRelativeLocation(),
		TargetLocation,
		DeltaSeconds,
		PressMoveSpeed);

	PlateMesh->SetRelativeLocation(NewLocation);
	if (NewLocation.Equals(TargetLocation, 0.25f))
	{
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
	}
}

void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
}

void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::EvaluatePlateState()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bObjectiveReady =
		!bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bHasOccupants = PlayerOccupancy->GetPlayerCount() > 0;
	const bool bNewPlateActive =
		(bLatchOnceActivated && bPlateActive)
		|| (bObjectiveReady && bHasOccupants);

	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("PressurePlate[%s] Players=%d ObjectiveReady=%s Active=%s"),
		*GetName(),
		PlayerOccupancy->GetPlayerCount(),
		bObjectiveReady ? TEXT("true") : TEXT("false"),
		bNewPlateActive ? TEXT("true") : TEXT("false"));

	if (bPlateActive == bNewPlateActive)
	{
		return;
	}

	bPlateActive = bNewPlateActive;
	HandlePlateActiveChanged();
	ForceNetUpdate();
}

void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	HandlePlateActiveChanged();
}

void AmultiplayerPressurePlate::HandlePlateActiveChanged()
{
	ApplyPlateState(false);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

void AmultiplayerPressurePlate::ApplyPlateState(bool bSnapToTarget)
{
	const FVector TargetLocation =
		ReleasedRelativeLocation
		+ (bPlateActive ? PressedOffset : FVector::ZeroVector);
	if (bSnapToTarget)
	{
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	SetActorTickEnabled(true);
}
