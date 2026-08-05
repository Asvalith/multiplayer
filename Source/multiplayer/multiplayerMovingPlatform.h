// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerMovingPlatform.generated.h"

class ACharacter;
class UBoxComponent;
class UStaticMeshComponent;
class UmultiplayerTransporterComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerPlatformOccupancyEvent,
	int32,
	CurrentPlayers,
	int32,
	RequiredPlayers);

/** A replicated platform activated by a configurable number of distinct players. */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerMovingPlatform : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerMovingPlatform();

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop|Platform")
	int32 GetPlayerCount() const { return ReplicatedPlayerCount; }

	UFUNCTION(BlueprintPure, Category = "Coop|Platform")
	int32 GetRequiredPlayers() const { return RequiredPlayers; }

	UPROPERTY(BlueprintAssignable, Category = "Coop|Platform")
	FmultiplayerPlatformOccupancyEvent OnPlatformOccupancyChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleBeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void HandleEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex);

	UFUNCTION()
	void OnRep_PlayerCount();

	UFUNCTION()
	void HandleOccupantDestroyed(AActor* DestroyedActor);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Platform", meta = (DisplayName = "On Platform Occupancy Visual Changed"))
	void ReceivePlatformOccupancyChanged(int32 CurrentPlayers, int32 NeededPlayers);

private:
	void RefreshOccupancy();
	void RemoveInvalidOccupants();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UStaticMeshComponent> PlatformMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UBoxComponent> ActivationVolume;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerTransporterComponent> Transporter;

	UPROPERTY(EditAnywhere, Category = "Coop|Platform", meta = (ClampMin = "1"))
	int32 RequiredPlayers = 1;

	UPROPERTY(ReplicatedUsing = OnRep_PlayerCount)
	int32 ReplicatedPlayerCount = 0;

	TSet<TWeakObjectPtr<ACharacter>> Occupants;
};
