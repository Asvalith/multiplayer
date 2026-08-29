// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopKey.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "multiplayerCoopCarryComponent.h"
#include "multiplayerKeySocket.h"
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

void AmultiplayerCoopKey::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		PickupTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerCoopKey::HandlePickupOverlap);
	}
	else
	{
		PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
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

	UmultiplayerCoopCarryComponent* CarryComponent =
		Character->FindComponentByClass<UmultiplayerCoopCarryComponent>();
	if (CarryComponent == nullptr || !CarryComponent->TryCarryKey(this))
	{
		return;
	}

	Holder = Character;
	Holder->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	SetOwner(Character);
	HandleHolderChanged();
	ForceNetUpdate();
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
	HandleInstalledChanged();
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
		if (UmultiplayerCoopCarryComponent* CarryComponent =
			Holder->FindComponentByClass<UmultiplayerCoopCarryComponent>())
		{
			CarryComponent->ClearCarriedKey(this);
		}
	}

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Holder = nullptr;
	SetOwner(nullptr);
	ApplyHeldState();
	ForceNetUpdate();

	if (bBroadcastChange)
	{
		HandleHolderChanged();
	}
}

void AmultiplayerCoopKey::OnRep_Holder()
{
	HandleHolderChanged();
}

void AmultiplayerCoopKey::OnRep_Installed()
{
	HandleInstalledChanged();
}

void AmultiplayerCoopKey::HandleHolderChanged()
{
	ApplyHeldState();
	OnKeyPickedUp.Broadcast(Holder);
	ReceiveKeyHolderChanged(Holder);
}

void AmultiplayerCoopKey::HandleInstalledChanged()
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
		HasAuthority() && !bIsHeld && !bInstalled
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);

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
