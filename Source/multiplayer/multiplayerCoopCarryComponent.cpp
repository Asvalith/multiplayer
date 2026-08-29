// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopCarryComponent.h"

#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.h"

UmultiplayerCoopCarryComponent::UmultiplayerCoopCarryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

bool UmultiplayerCoopCarryComponent::TryCarryKey(AmultiplayerCoopKey* Key)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority() || Key == nullptr)
	{
		return false;
	}

	if (CarriedKey != nullptr && !IsValid(CarriedKey))
	{
		CarriedKey = nullptr;
	}

	if (CarriedKey != nullptr && CarriedKey != Key)
	{
		return false;
	}

	CarriedKey = Key;
	return true;
}

void UmultiplayerCoopCarryComponent::ClearCarriedKey(
	const AmultiplayerCoopKey* ExpectedKey)
{
	AActor* Owner = GetOwner();
	if (Owner != nullptr
		&& Owner->HasAuthority()
		&& CarriedKey == ExpectedKey)
	{
		CarriedKey = nullptr;
	}
}
