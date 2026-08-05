// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "multiplayerGameMode.generated.h"

UCLASS(minimalapi)
class AmultiplayerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AmultiplayerGameMode();

	UFUNCTION(BlueprintCallable, Category = "Network|LAN")
	void HostLANGame();

	UFUNCTION(BlueprintCallable, Category = "Network|LAN")
	void JoinLANGame();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|LAN")
	FString LANMapPath = TEXT("/Script/Engine.World'/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo.Stylized_Egypt_Demo'");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|LAN")
	FString LANServerAddress = TEXT("127.0.0.1");
};



