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

	/** Reloads the current match and travels every connected player together. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Coop|Match")
	void RestartCoopGame();

protected:
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	/** Authoritative number of rack slots that must be activated before victory. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Coop|Objective", meta = (ClampMin = "1"))
	int32 RequiredKeys = 4;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|LAN")
	FString LANMapPath = TEXT("/Script/Engine.World'/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo.Stylized_Egypt_Demo'");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|LAN")
	FString LANServerAddress = TEXT("127.0.0.1");

private:
	void LogConnectionSnapshot(
		const TCHAR* Phase,
		const AController* Controller,
		const APawn* Pawn) const;
};
