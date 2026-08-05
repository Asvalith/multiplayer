// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
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

	PlateMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateMesh"));
	PlateMesh->SetupAttachment(SceneRoot);
	PlateMesh->SetMobility(EComponentMobility::Movable);
	PlateMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlateMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->SetRelativeLocation(FVector(0.0f, 0.0f, 60.0f));
	ActivationTrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlateMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

	ReleasedRelativeLocation = PlateMesh->GetRelativeLocation();
	ActivationTrigger->OnComponentBeginOverlap.AddDynamic(this, &AmultiplayerPressurePlate::HandleBeginOverlap);
	ActivationTrigger->OnComponentEndOverlap.AddDynamic(this, &AmultiplayerPressurePlate::HandleEndOverlap);
	ApplyPlateState(true);
}

void AmultiplayerPressurePlate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationTrigger->OnComponentBeginOverlap.RemoveDynamic(this, &AmultiplayerPressurePlate::HandleBeginOverlap);
	ActivationTrigger->OnComponentEndOverlap.RemoveDynamic(this, &AmultiplayerPressurePlate::HandleEndOverlap);

	for (const TWeakObjectPtr<ACharacter>& Occupant : Occupants)
	{
		if (Occupant.IsValid())
		{
			Occupant->OnDestroyed.RemoveDynamic(this, &AmultiplayerPressurePlate::HandleOccupantDestroyed);
		}
	}

	Occupants.Reset();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector TargetLocation = ReleasedRelativeLocation + (bPlateActive ? PressedOffset : FVector::ZeroVector);
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

void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
}

void AmultiplayerPressurePlate::GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const
{
	for (const TWeakObjectPtr<ACharacter>& Occupant : Occupants)
	{
		if (Occupant.IsValid())
		{
			OutCharacters.Add(Occupant.Get());
		}
	}
}

void AmultiplayerPressurePlate::HandleBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	AddOccupant(OtherActor);
}

void AmultiplayerPressurePlate::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	RemoveOccupant(OtherActor);
}

void AmultiplayerPressurePlate::AddOccupant(AActor* OtherActor)
{
	if (!HasAuthority())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	if (bRequirePlayerControlledCharacter && !Character->IsPlayerControlled())
	{
		return;
	}

	Occupants.Add(Character);
	Character->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerPressurePlate::HandleOccupantDestroyed);
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::RemoveOccupant(AActor* OtherActor)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		Occupants.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(this, &AmultiplayerPressurePlate::HandleOccupantDestroyed);
		EvaluatePlateState();
	}
}

void AmultiplayerPressurePlate::HandleOccupantDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(DestroyedActor))
	{
		Occupants.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(this, &AmultiplayerPressurePlate::HandleOccupantDestroyed);
		EvaluatePlateState();
	}
}

void AmultiplayerPressurePlate::RemoveInvalidOccupants()
{
	for (auto It = Occupants.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void AmultiplayerPressurePlate::EvaluatePlateState()
{
	if (!HasAuthority())
	{
		return;
	}

	RemoveInvalidOccupants();

	const bool bNewPlateActive = !Occupants.IsEmpty();
	if (bPlateActive != bNewPlateActive)
	{
		bPlateActive = bNewPlateActive;
		OnRep_PlateActive();
		ForceNetUpdate();
	}
}

void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	ApplyPlateState(false);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

void AmultiplayerPressurePlate::ApplyPlateState(bool bSnapToTarget)
{
	const FVector TargetLocation = ReleasedRelativeLocation + (bPlateActive ? PressedOffset : FVector::ZeroVector);
	if (bSnapToTarget)
	{
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	SetActorTickEnabled(true);
}
