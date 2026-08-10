// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopKey.h"

#include "multiplayerKeySocket.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

AmultiplayerCoopKey::AmultiplayerCoopKey()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	KeyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("KeyMesh"));
	KeyMesh->SetupAttachment(SceneRoot);
	KeyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PickupTrigger = CreateDefaultSubobject<USphereComponent>(TEXT("PickupTrigger"));
	PickupTrigger->SetupAttachment(SceneRoot);
	PickupTrigger->SetSphereRadius(100.0f);
	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupTrigger->OnComponentBeginOverlap.AddDynamic(
		this,
		&AmultiplayerCoopKey::HandlePickupOverlap);
}

void AmultiplayerCoopKey::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (RotationSpeedDegrees <= 0.0f || RotationAxis.IsNearlyZero())
	{
		return;
	}

	const FQuat RotationDelta(
		RotationAxis.GetSafeNormal(),
		FMath::DegreesToRadians(RotationSpeedDegrees * DeltaSeconds));
	KeyMesh->AddLocalRotation(RotationDelta);
}

void AmultiplayerCoopKey::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Holder != nullptr)
	{
		Holder->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	}

	PickupTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerCoopKey::HandlePickupOverlap);
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopKey::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopKey, Holder);
	DOREPLIFETIME(AmultiplayerCoopKey, bInstalled);
}

void AmultiplayerCoopKey::HandlePickupOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || Holder != nullptr || bInstalled)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character != nullptr && Character->IsPlayerControlled())
	{
		PickupBy(Character);
	}
}

void AmultiplayerCoopKey::PickupBy(ACharacter* Character)
{
	if (!HasAuthority() || Character == nullptr || Holder != nullptr || bInstalled)
	{
		return;
	}

	if (DestinationSocket != nullptr && DestinationSocket->StoreCollectedKey(this))
	{
		OnKeyPickedUp.Broadcast(Character);
		ReceiveKeyHolderChanged(nullptr);
		return;
	}

	Holder = Character;
	Holder->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	SetOwner(Character);
	ApplyHeldState();
	ForceNetUpdate();
	OnRep_Holder();
}

bool AmultiplayerCoopKey::InstallAtSocket(USceneComponent* SocketPoint)
{
	if (!HasAuthority() || SocketPoint == nullptr || bInstalled)
	{
		return false;
	}

	ReleaseHolder(false);
	bInstalled = true;
	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetOwner(SocketPoint->GetOwner());
	AttachToComponent(
		SocketPoint,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	OnRep_Installed();
	ForceNetUpdate();
	return true;
}

bool AmultiplayerCoopKey::ConsumeAtSocket()
{
	if (!HasAuthority() || Holder == nullptr)
	{
		return false;
	}

	ReleaseHolder(false);
	Destroy();
	return true;
}

void AmultiplayerCoopKey::HandleHolderDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority() || DestroyedActor != Holder)
	{
		return;
	}

	ReleaseHolder(true);
}

void AmultiplayerCoopKey::ReleaseHolder(bool bBroadcastChange)
{
	if (Holder != nullptr)
	{
		Holder->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	}

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Holder = nullptr;
	SetOwner(nullptr);
	ApplyHeldState();
	ForceNetUpdate();

	if (bBroadcastChange)
	{
		OnRep_Holder();
	}
}

void AmultiplayerCoopKey::OnRep_Holder()
{
	ApplyHeldState();
	OnKeyPickedUp.Broadcast(Holder);
	ReceiveKeyHolderChanged(Holder);
}

void AmultiplayerCoopKey::OnRep_Installed()
{
	if (!bInstalled)
	{
		return;
	}

	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AmultiplayerCoopKey::ApplyHeldState()
{
	const bool bIsHeld = Holder != nullptr;
	PickupTrigger->SetCollisionEnabled(
		bIsHeld ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);

	if (!bIsHeld)
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		return;
	}

	USceneComponent* AttachParent = Holder->GetMesh();
	if (AttachParent == nullptr)
	{
		AttachParent = Holder->GetRootComponent();
	}

	if (AttachParent != nullptr)
	{
		AttachToComponent(
			AttachParent,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale,
			CarrySocketName);
	}
}
