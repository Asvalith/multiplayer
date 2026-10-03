// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerCoopKey.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "Core/multiplayerLog.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Net/UnrealNetwork.h"

AmultiplayerCoopKey::AmultiplayerCoopKey()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// 安装后的父节点与偏移继续使用 Actor 原生附件复制。
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
		if (!IsValid(DestinationSocket))
		{
			UE_LOG(LogMultiplayer, Warning, TEXT("Key[%s] has no DestinationSocket; collection is disabled until configured."), *GetName());
		}
		PickupTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerCoopKey::HandlePickupOverlap);
	}
	RefreshKeyState();
}

void AmultiplayerCoopKey::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	PickupTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerCoopKey::HandlePickupOverlap);
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopKey::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopKey, bInstalled);
}

/** 真实重叠触发自动归位；同帧重复触碰由钥匙和插槽的已安装状态拦截。 */
void AmultiplayerCoopKey::HandlePickupOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bInstalled)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (IsValid(Character) && !Character->IsActorBeingDestroyed() && Character->IsPlayerControlled()
		&& IsValid(DestinationSocket) && !DestinationSocket->IsActorBeingDestroyed())
	{
		DestinationSocket->StoreCollectedKey(this);
	}
}

bool AmultiplayerCoopKey::InstallAtSocket(USceneComponent* SocketPoint)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bInstalled
		|| !IsValid(SocketPoint) || !SocketPoint->IsRegistered() || SocketPoint == GetRootComponent()
		|| SocketPoint->GetWorld() != GetWorld())
	{
		return false;
	}

	// 附着可能触发重叠回调，先标记安装防止重入；失败时恢复原状态。
	bInstalled = true;
	if (!AttachToComponent(
		SocketPoint,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale))
	{
		bInstalled = false;
		return false;
	}
	SetOwner(SocketPoint->GetOwner());
	RefreshKeyState();
	ForceNetUpdate();
	return true;
}

void AmultiplayerCoopKey::OnRep_Installed()
{
	RefreshKeyState();
}

/** 碰撞最后更新，避免重叠回调重入后又写回旧展示状态。 */
void AmultiplayerCoopKey::RefreshKeyState()
{
	const bool bFree = !bInstalled;
	SetActorTickEnabled(
		bFree
		&& RotationSpeedDegrees > 0.0f
		&& !RotationAxis.IsNearlyZero());
	PickupTrigger->SetCollisionEnabled(
		HasAuthority() && bFree
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);
}
