// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopGate.generated.h"

class AmultiplayerPressurePlate;
class AmultiplayerCoopGameState;
class UArrowComponent;
class USceneComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCoopGateStateChanged, bool, bIsOpen);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCoopGatePlateProgressChanged, int32, ActivePlateCount, int32, RequiredPlateCount);

/**
 * Server-authoritative gate that opens when linked pressure plates are active.
 *
 * The gate no longer owns pressure plate overlap logic. Designers place any
 * number of AmultiplayerPressurePlate actors in the level, then link them here.
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
	int32 GetActivePlateCount() const { return ActivePlateCount; }

	UFUNCTION(BlueprintPure, Category = "Coop Gate")
	int32 GetRequiredPlateCount() const;

	UPROPERTY(BlueprintAssignable, Category = "Coop Gate")
	FOnCoopGateStateChanged OnGateStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Coop Gate")
	FOnCoopGatePlateProgressChanged OnPlateProgressChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_GateOpen();

	UFUNCTION()
	void OnRep_ActivePlateCount();

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop Gate", meta = (DisplayName = "On Gate Visual State Changed"))
	void ReceiveGateVisualStateChanged(bool bIsOpen);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop Gate", meta = (DisplayName = "On Plate Progress Changed"))
	void ReceivePlateProgressChanged(int32 NewActivePlateCount, int32 NewRequiredPlateCount);

private:
	void BindRequiredPlates();
	void UnbindRequiredPlates();
	void EvaluateGateState();
	void HandleGateStateChanged();
	void HandleActivePlateCountChanged();
	void ApplyGateState(bool bSnapToTarget);

	UPROPERTY(VisibleAnywhere, Category = "Coop Gate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> DoorMesh;

	/** Designers position these two points in a Blueprint child to define door travel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> ClosedPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop Gate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> OpenPoint;

	UPROPERTY(EditInstanceOnly, Category = "Coop Gate|Rules")
	TArray<TObjectPtr<AmultiplayerPressurePlate>> RequiredPlates;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules", meta = (ClampMin = "1"))
	int32 RequiredActivePlateCount = 1;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bStayOpenOnceActivated = false;

	/** Prevents the gate from opening until every required key is activated. */
	UPROPERTY(EditAnywhere, Category = "Coop Gate|Rules")
	bool bRequireObjectiveComplete = false;

	UPROPERTY(EditAnywhere, Category = "Coop Gate|Movement", meta = (ClampMin = "1.0"))
	float DoorMoveSpeed = 250.0f;

	UPROPERTY(ReplicatedUsing = OnRep_GateOpen)
	bool bGateOpen = false;

	UPROPERTY(ReplicatedUsing = OnRep_ActivePlateCount)
	int32 ActivePlateCount = 0;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
