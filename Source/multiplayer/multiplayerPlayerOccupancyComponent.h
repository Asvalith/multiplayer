// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerPlayerOccupancyComponent.generated.h"

class ACharacter;
class UPrimitiveComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerOccupancyChangedEvent,
	int32,
	PlayerCount);

/**
 * Server-only overlap bookkeeping shared by cooperative gameplay actors.
 *
 * Counts overlapping primitive components per Character so one EndOverlap
 * cannot remove a Character whose other component is still inside the volume.
 * Owners remain responsible for gameplay rules and replicated presentation.
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerPlayerOccupancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerPlayerOccupancyComponent();

	/** Binds one logic-only overlap volume. Non-authority owners disable it. */
	void BindTrigger(
		UPrimitiveComponent* InTrigger,
		bool bInRequirePlayerControlledCharacter = true);

	void UnbindTrigger();

	int32 GetPlayerCount() const;
	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	UPROPERTY(BlueprintAssignable, Category = "Coop|Occupancy")
	FmultiplayerOccupancyChangedEvent OnOccupancyChanged;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
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
	void HandleOccupantDestroyed(AActor* DestroyedActor);

	ACharacter* GetValidOccupant(AActor* OtherActor) const;
	void AddOccupant(AActor* OtherActor);
	void RemoveOccupant(AActor* OtherActor);
	void BroadcastIfPlayerCountChanged(int32 PreviousPlayerCount);
	void ClearOccupants();

	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> BoundTrigger;

	TMap<TWeakObjectPtr<ACharacter>, int32> OverlapCounts;
	bool bRequirePlayerControlledCharacter = true;
};
