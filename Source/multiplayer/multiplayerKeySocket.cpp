// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerKeySocket.h"

#include "multiplayerCoopCarryComponent.h"
#include "multiplayerCoopKey.h"
#include "multiplayerGameMode.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

AmultiplayerKeySocket::AmultiplayerKeySocket()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	SocketMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SocketMesh"));
	SocketMesh->SetupAttachment(SceneRoot);
	SocketMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	KeyDisplayPoint = CreateDefaultSubobject<USceneComponent>(TEXT("KeyDisplayPoint"));
	KeyDisplayPoint->SetupAttachment(SceneRoot);
	KeyDisplayPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 75.0f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->SetBoxExtent(FVector(100.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

void AmultiplayerKeySocket::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		ActivationTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerKeySocket::HandleSocketOverlap);
	}
	else
	{
		ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AmultiplayerKeySocket::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerKeySocket::HandleSocketOverlap);
	Super::EndPlay(EndPlayReason);
}

bool AmultiplayerKeySocket::StoreCollectedKey(AmultiplayerCoopKey* Key)
{
	if (!HasAuthority() || bActivated || Key == nullptr || !Key->InstallAtSocket(KeyDisplayPoint))
	{
		return false;
	}

	CommitServerActivation();

	return true;
}
void AmultiplayerKeySocket::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerKeySocket, bActivated);
}

void AmultiplayerKeySocket::HandleSocketOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || bActivated)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr || !Character->IsPlayerControlled())
	{
		return;
	}

	AmultiplayerCoopKey* Key = FindCarriedKey(Character);
	if (Key == nullptr || !Key->ConsumeAtSocket())
	{
		return;
	}

	CommitServerActivation();
}

void AmultiplayerKeySocket::CommitServerActivation()
{
	if (!HasAuthority() || bActivated)
	{
		return;
	}

	bActivated = true;
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandleActivatedChanged();
	ForceNetUpdate();

	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
	{
		CoopGameMode->RegisterActivatedKey();
	}
}

AmultiplayerCoopKey* AmultiplayerKeySocket::FindCarriedKey(
	ACharacter* Character) const
{
	if (Character == nullptr)
	{
		return nullptr;
	}

	const UmultiplayerCoopCarryComponent* CarryComponent =
		Character->FindComponentByClass<UmultiplayerCoopCarryComponent>();
	AmultiplayerCoopKey* Key =
		CarryComponent != nullptr ? CarryComponent->GetCarriedKey() : nullptr;
	return Key != nullptr && Key->IsHeldBy(Character) ? Key : nullptr;
}

void AmultiplayerKeySocket::OnRep_Activated()
{
	HandleActivatedChanged();
}

void AmultiplayerKeySocket::HandleActivatedChanged()
{
	if (!bActivated)
	{
		return;
	}

	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OnSocketActivated.Broadcast();
	ReceiveSocketVisualActivated();
}
