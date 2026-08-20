// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGameState.h"

#include "Net/UnrealNetwork.h"

void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopGameState, ObjectiveState);
}
void AmultiplayerCoopGameState::ConfigureRequiredKeys(int32 RequiredKeys)
{
	if (!HasAuthority() || ObjectiveState.bGameWon)
	{
		return;
	}

	ObjectiveState.RequiredKeys = FMath::Max(1, RequiredKeys);
	ObjectiveState.ActivatedKeys = FMath::Min(
		ObjectiveState.ActivatedKeys,
		ObjectiveState.RequiredKeys);
	ForceNetUpdate();
	OnRep_ObjectiveState();
}

bool AmultiplayerCoopGameState::RegisterActivatedKey()
{
	if (!HasAuthority()
		|| ObjectiveState.bGameWon
		|| IsObjectiveComplete())
	{
		return false;
	}

	++ObjectiveState.ActivatedKeys;
	ForceNetUpdate();
	OnRep_ObjectiveState();
	return true;
}

bool AmultiplayerCoopGameState::TryCompleteGame()
{
	if (!HasAuthority()
		|| ObjectiveState.bGameWon
		|| !IsObjectiveComplete())
	{
		return false;
	}

	ObjectiveState.bGameWon = true;
	ForceNetUpdate();
	OnRep_ObjectiveState();
	return true;
}

void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	OnObjectiveProgressChanged.Broadcast(
		ObjectiveState.ActivatedKeys,
		ObjectiveState.RequiredKeys);

	if (ObjectiveState.bGameWon)
	{
		OnGameWon.Broadcast();
		ReceiveGameWon();
	}
}
