// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "multiplayerCoopTeamAgentInterface.generated.h"

namespace MultiplayerTeams
{
	inline constexpr int32 NoTeam = INDEX_NONE;
	inline constexpr int32 Players = 1;
	inline constexpr int32 Enemies = 2;
}

UINTERFACE(BlueprintType)
class MULTIPLAYER_API UmultiplayerCoopTeamAgentInterface : public UInterface
{
	GENERATED_BODY()
};

class MULTIPLAYER_API ImultiplayerCoopTeamAgentInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Coop|Team")
	int32 GetCoopTeamId() const;
};
