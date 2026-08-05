// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"

UmultiplayerTransporterComponent::UmultiplayerTransporterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UmultiplayerTransporterComponent::BeginPlay()
{
	Super::BeginPlay();

	if (const AActor* Owner = GetOwner())
	{
		StartLocation = Owner->GetActorLocation();
	}
}

void UmultiplayerTransporterComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		SetComponentTickEnabled(false);
		return;
	}

	const FVector TargetLocation = GetTargetLocation();
	const FVector NewLocation = FMath::VInterpConstantTo(
		Owner->GetActorLocation(),
		TargetLocation,
		DeltaTime,
		MoveSpeed);

	Owner->SetActorLocation(NewLocation, true);
	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		Owner->SetActorLocation(TargetLocation, true);
		FinishMovement();
	}
}

void UmultiplayerTransporterComponent::SetTransportActive(bool bNewActive)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (!bReturnWhenInactive && !bNewActive)
	{
		return;
	}

	bTransportActive = bNewActive;
	if (Owner->GetActorLocation().Equals(GetTargetLocation(), 0.5f))
	{
		FinishMovement();
		return;
	}

	bMoving = true;
	SetComponentTickEnabled(true);
}

FVector UmultiplayerTransporterComponent::GetTargetLocation() const
{
	if (!bTransportActive)
	{
		return StartLocation;
	}

	const AActor* Owner = GetOwner();
	const FVector Offset = bOffsetUsesActorRotation && Owner != nullptr
		? Owner->GetActorQuat().RotateVector(ActiveOffset)
		: ActiveOffset;

	return StartLocation + Offset;
}

void UmultiplayerTransporterComponent::FinishMovement()
{
	bMoving = false;
	SetComponentTickEnabled(false);
	OnTransportTargetReached.Broadcast(bTransportActive);
}
