// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerMovingPlatform.h"

#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "multiplayerPlayerOccupancyComponent.h"
#include "multiplayerPressurePlate.h"
#include "multiplayerTransporterComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);

	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);

	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->bEditableWhenInherited = true;
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
	Transporter =
		CreateDefaultSubobject<UmultiplayerTransporterComponent>(
			TEXT("Transporter"));

	StartPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("StartPoint"));
	StartPoint->SetupAttachment(PlatformRoot);
	StartPoint->SetMobility(EComponentMobility::Movable);
	StartPoint->bEditableWhenInherited = true;
	StartPoint->ArrowColor = FColor::Red;

	TargetPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("TargetPoint"));
	TargetPoint->SetupAttachment(PlatformRoot);
	TargetPoint->SetMobility(EComponentMobility::Movable);
	TargetPoint->bEditableWhenInherited = true;
	TargetPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 500.0f));
	TargetPoint->ArrowColor = FColor::Green;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlatformMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleOccupancyChanged);

	if (!HasAuthority())
	{
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HandlePlayerCountChanged();
		return;
	}

	if (ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy)
	{
		PlayerOccupancy->BindTrigger(ActivationVolume);
	}
	else
	{
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (ActivationPlate != nullptr)
		{
			ActivationPlate->OnPlateActiveChanged.AddUniqueDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
		}
	}

	HandlePlayerCountChanged();
	RefreshActivation();
}

void AmultiplayerMovingPlatform::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (ActivationPlate != nullptr)
	{
		ActivationPlate->OnPlateActiveChanged.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AmultiplayerMovingPlatform::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerMovingPlatform, ReplicatedPlayerCount);
}

void AmultiplayerMovingPlatform::HandleOccupancyChanged(int32 PlayerCount)
{
	if (!HasAuthority()
		|| ActivationSource != EMovingPlatformActivationSource::PlatformOccupancy)
	{
		return;
	}

	if (ReplicatedPlayerCount != PlayerCount)
	{
		ReplicatedPlayerCount = PlayerCount;
		HandlePlayerCountChanged();
		ForceNetUpdate();
	}

	RefreshActivation();
}

void AmultiplayerMovingPlatform::HandleActivationPlateChanged(
	AmultiplayerPressurePlate* Plate,
	bool bIsActive)
{
	RefreshActivation();
}

void AmultiplayerMovingPlatform::RefreshActivation()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bShouldActivate =
		ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate
			? ActivationPlate != nullptr && ActivationPlate->IsPlateActive()
			: ReplicatedPlayerCount >= RequiredPlayers;
	Transporter->SetTransportActive(bShouldActivate);
}

void AmultiplayerMovingPlatform::OnRep_PlayerCount()
{
	HandlePlayerCountChanged();
}

void AmultiplayerMovingPlatform::HandlePlayerCountChanged()
{
	OnPlatformOccupancyChanged.Broadcast(
		ReplicatedPlayerCount,
		RequiredPlayers);
	ReceivePlatformOccupancyChanged(
		ReplicatedPlayerCount,
		RequiredPlayers);
}
