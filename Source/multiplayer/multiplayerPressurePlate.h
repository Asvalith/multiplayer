// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerPressurePlate.generated.h"

class ACharacter;
class AmultiplayerCoopGameState;
class AmultiplayerPressurePlate;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;
class USceneComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnPressurePlateActiveChanged,
	AmultiplayerPressurePlate*,
	Plate,
	bool,
	bIsActive);

/**
 * Server-authoritative pressure plate.
 *
 * Occupancy bookkeeping is delegated to PlayerOccupancy. This actor owns only
 * the activation rule, replicated state, and plate presentation.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerPressurePlate : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerPressurePlate();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

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
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_PlateActive();

	UFUNCTION(BlueprintImplementableEvent, Category = "Pressure Plate", meta = (DisplayName = "On Plate Visual State Changed"))
	void ReceivePlateVisualStateChanged(bool bIsActive);

private:
	void EvaluatePlateState();
	void HandlePlateActiveChanged();
	void ApplyPlateState(bool bSnapToTarget);

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pressure Plate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> PlateMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<UBoxComponent> ActivationTrigger;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Movement")
	FVector PressedOffset = FVector(0.0f, 0.0f, -8.0f);

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Movement", meta = (ClampMin = "1.0"))
	float PressMoveSpeed = 80.0f;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequirePlayerControlledCharacter = true;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bLatchOnceActivated = false;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequireObjectiveComplete = false;

	UPROPERTY(ReplicatedUsing = OnRep_PlateActive)
	bool bPlateActive = false;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	FVector ReleasedRelativeLocation = FVector::ZeroVector;
};
