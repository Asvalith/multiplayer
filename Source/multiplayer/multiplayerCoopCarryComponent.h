// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerCoopCarryComponent.generated.h"

class AmultiplayerCoopKey;

/** Server-authoritative inventory slot for the Character's currently carried key. */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerCoopCarryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerCoopCarryComponent();

	UFUNCTION(BlueprintPure, Category = "Coop|Carry")
	AmultiplayerCoopKey* GetCarriedKey() const { return CarriedKey; }

	bool TryCarryKey(AmultiplayerCoopKey* Key);
	void ClearCarriedKey(const AmultiplayerCoopKey* ExpectedKey);

private:
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Coop|Carry", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<AmultiplayerCoopKey> CarriedKey;
};
