// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopGate.generated.h"

class ACharacter;
class UBoxComponent;
class USceneComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCoopGateStateChanged, bool, bIsOpen);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCoopPlateStateChanged, int32, ActivePlateMask);

/**
 * Server-authoritative two-player pressure gate.
 *
 * The server owns overlap checks and replicated state. Clients only animate the
 * replicated result, so no per-frame door transform needs to be replicated.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCoopGate : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopGate();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop Gate")
	bool IsGateOpen() const { return bGateOpen; }

	UFUNCTION(BlueprintPure, Category = "Coop Gate")
	int32 GetActivePlateMask() const { return ActivePlateMask; }

	UPROPERTY(BlueprintAssignable, Category = "Coop Gate")
	FOnCoopGateStateChanged OnGateStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Coop Gate")
	FOnCoopPlateStateChanged OnPlateStateChanged;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandlePlateABeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void HandlePlateAEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex);

	UFUNCTION()
	void HandlePlateBBeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void HandlePlateBEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex);

	UFUNCTION()
	void OnRep_GateOpen();

	UFUNCTION()
	void OnRep_ActivePlateMask();

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop Gate", meta = (DisplayName = "On Gate Visual State Changed"))
	void ReceiveGateVisualStateChanged(bool bIsOpen);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop Gate", meta = (DisplayName = "On Plate Visual State Changed"))
	void ReceivePlateVisualStateChanged(int32 NewActivePlateMask);

private:
	void AddPlateOccupant(TSet<TWeakObjectPtr<ACharacter>>& Occupants, AActor* OtherActor);
	void RemovePlateOccupant(TSet<TWeakObjectPtr<ACharacter>>& Occupants, AActor* OtherActor);
	void RemoveInvalidOccupants(TSet<TWeakObjectPtr<ACharacter>>& Occupants);
	void EvaluateGateState();
	bool HasTwoDistinctPlayers() const;
	void ApplyGateState(bool bSnapToTarget);
	void ApplyPlateState();

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<UStaticMeshComponent> DoorMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<UStaticMeshComponent> PlateAMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<UStaticMeshComponent> PlateBMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<UBoxComponent> PlateATrigger;

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<UBoxComponent> PlateBTrigger;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Movement")
	FVector DoorOpenOffset = FVector(0.0f, 0.0f, 400.0f);

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Movement", meta = (ClampMin = "1.0"))
	float DoorMoveSpeed = 250.0f;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bStayOpenOnceActivated = true;

	UPROPERTY(ReplicatedUsing = OnRep_GateOpen)
	bool bGateOpen = false;

	UPROPERTY(ReplicatedUsing = OnRep_ActivePlateMask)
	uint8 ActivePlateMask = 0;

	FVector DoorClosedRelativeLocation = FVector::ZeroVector;
	FVector PlateAReleasedRelativeLocation = FVector::ZeroVector;
	FVector PlateBReleasedRelativeLocation = FVector::ZeroVector;

	TSet<TWeakObjectPtr<ACharacter>> PlateAOccupants;
	TSet<TWeakObjectPtr<ACharacter>> PlateBOccupants;
};
