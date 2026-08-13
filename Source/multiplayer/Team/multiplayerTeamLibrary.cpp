// Copyright Epic Games, Inc. All Rights Reserved.

#include "Team/multiplayerTeamLibrary.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"

int32 UmultiplayerTeamLibrary::ResolveTeamId(const AActor* Actor)
{
	if (Actor == nullptr)
	{
		return MultiplayerTeams::NoTeam;
	}

	if (Actor->Implements<UmultiplayerCoopTeamAgentInterface>())
	{
		return ImultiplayerCoopTeamAgentInterface::Execute_GetCoopTeamId(
			const_cast<AActor*>(Actor));
	}

	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		if (const APlayerState* PlayerState = Pawn->GetPlayerState())
		{
			const int32 PlayerStateTeam = ResolveTeamId(PlayerState);
			if (PlayerStateTeam != MultiplayerTeams::NoTeam)
			{
				return PlayerStateTeam;
			}
		}
	}

	if (const UAbilitySystemComponent* ASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(
			const_cast<AActor*>(Actor)))
	{
		const AActor* ASCActor = ASC->GetOwnerActor();
		if (ASCActor != nullptr && ASCActor != Actor)
		{
			return ResolveTeamId(ASCActor);
		}
	}

	return MultiplayerTeams::NoTeam;
}

bool UmultiplayerTeamLibrary::AreHostile(
	const AActor* SourceActor,
	const AActor* TargetActor)
{
	const int32 SourceTeam = ResolveTeamId(SourceActor);
	const int32 TargetTeam = ResolveTeamId(TargetActor);
	return SourceTeam != MultiplayerTeams::NoTeam
		&& TargetTeam != MultiplayerTeams::NoTeam
		&& SourceTeam != TargetTeam;
}
