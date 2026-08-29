// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGameState.h"

#include "Net/UnrealNetwork.h"

void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopGameState, ObjectiveState);
}
void AmultiplayerCoopGameState::ApplyAuthoritativeState(
	const FmultiplayerCoopObjectiveState& NewObjectiveState)
{
	if (!HasAuthority())
	{
		return;
	}

	FmultiplayerCoopObjectiveState SanitizedState = NewObjectiveState;
	SanitizedState.RequiredKeys = FMath::Max(0, SanitizedState.RequiredKeys);
	SanitizedState.ActivatedKeys = FMath::Clamp(
		SanitizedState.ActivatedKeys,
		0,
		SanitizedState.RequiredKeys);
	if (SanitizedState.RequiredKeys == 0)
	{
		SanitizedState.bGameWon = false;
	}

	if (ObjectiveState.ActivatedKeys == SanitizedState.ActivatedKeys
		&& ObjectiveState.RequiredKeys == SanitizedState.RequiredKeys
		&& ObjectiveState.bGameWon == SanitizedState.bGameWon)
	{
		return;
	}

	ObjectiveState = SanitizedState;
	HandleObjectiveStateChanged();
	ForceNetUpdate();
}

void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	HandleObjectiveStateChanged();
}

void AmultiplayerCoopGameState::HandleObjectiveStateChanged()
{
	OnObjectiveProgressChanged.Broadcast(
		ObjectiveState.ActivatedKeys,
		ObjectiveState.RequiredKeys);

	if (ObjectiveState.bGameWon)
	{
		OnGameWon.Broadcast();
	}
}
