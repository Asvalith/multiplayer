// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "multiplayerTeamLibrary.generated.h"

UCLASS()
class MULTIPLAYER_API UmultiplayerTeamLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Coop|Team")
	static int32 ResolveTeamId(const AActor* Actor);

	UFUNCTION(BlueprintPure, Category = "Coop|Team")
	static bool AreHostile(const AActor* SourceActor, const AActor* TargetActor);
};
