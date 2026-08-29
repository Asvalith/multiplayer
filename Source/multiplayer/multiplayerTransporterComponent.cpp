// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerTransporterComponent.h"

#include "GameFramework/Actor.h"

UmultiplayerTransporterComponent::UmultiplayerTransporterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
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

void UmultiplayerTransporterComponent::ConfigureWorldTargets(
	const FVector& InStartLocation,
	const FVector& InActiveLocation)
{
	StartLocation = InStartLocation;
	ActiveLocation = InActiveLocation;
}

FVector UmultiplayerTransporterComponent::GetTargetLocation() const
{
	if (!bTransportActive)
	{
		return StartLocation;
	}

	return ActiveLocation;
}

void UmultiplayerTransporterComponent::FinishMovement()
{
	bMoving = false;
	SetComponentTickEnabled(false);
}
