// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerPressurePlate.generated.h"

class ACharacter;
class UBoxComponent;
class USceneComponent;
class UStaticMeshComponent;
class AmultiplayerPressurePlate;
class AmultiplayerCoopGameState;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPressurePlateActiveChanged, AmultiplayerPressurePlate*, Plate, bool, bIsActive);

/**
 * Standalone server-authoritative pressure plate.
 *
 * The plate only answers one question: is at least one valid player standing on me?
 * Doors, platforms, or other mechanisms can subscribe to that replicated answer.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerPressurePlate : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerPressurePlate();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Pressure Plate")
	bool IsPlateActive() const { return bPlateActive; }

	UFUNCTION(BlueprintPure, Category = "Pressure Plate")
	bool IsLatchedOnceActivated() const { return bLatchOnceActivated; }

	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	UPROPERTY(BlueprintAssignable, Category = "Pressure Plate")
	FOnPressurePlateActiveChanged OnPlateActiveChanged;

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
	void HandleOccupantDestroyed(AActor* DestroyedActor);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_PlateActive();

	UFUNCTION(BlueprintImplementableEvent, Category = "Pressure Plate", meta = (DisplayName = "On Plate Visual State Changed"))
	void ReceivePlateVisualStateChanged(bool bIsActive);

private:
	void AddOccupant(AActor* OtherActor);
	void RemoveOccupant(AActor* OtherActor);
	void RemoveInvalidOccupants();
	void EvaluatePlateState();
	void ApplyPlateState(bool bSnapToTarget);

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pressure Plate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> PlateMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<UBoxComponent> ActivationTrigger;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Movement")
	FVector PressedOffset = FVector(0.0f, 0.0f, -8.0f);

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Movement", meta = (ClampMin = "1.0"))
	float PressMoveSpeed = 80.0f;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequirePlayerControlledCharacter = true;

	/** When enabled, the first valid press permanently activates this plate. */
	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bLatchOnceActivated = false;

	/** When enabled, overlap is ignored until all configured keys are in the rack. */
	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequireObjectiveComplete = false;

	UPROPERTY(ReplicatedUsing = OnRep_PlateActive)
	bool bPlateActive = false;

	FVector ReleasedRelativeLocation = FVector::ZeroVector;
	TSet<TWeakObjectPtr<ACharacter>> Occupants;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
