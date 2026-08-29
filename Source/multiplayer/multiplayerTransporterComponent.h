// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerTransporterComponent.generated.h"

/**
 * Reusable server-side mover. The owning actor replicates its transform while
 * this component only owns movement rules and stops ticking at either target.
 */
UCLASS(ClassGroup = (Coop), meta = (BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerTransporterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerTransporterComponent();

	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	void SetTransportActive(bool bNewActive);

	/** Captures fixed world-space endpoints supplied by the owning mechanism. */
	void ConfigureWorldTargets(const FVector& InStartLocation, const FVector& InActiveLocation);

private:
	FVector GetTargetLocation() const;
	void FinishMovement();

	UPROPERTY(EditAnywhere, Category = "Coop|Transport", meta = (ClampMin = "1.0"))
	float MoveSpeed = 150.0f;

	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	bool bReturnWhenInactive = true;

	FVector StartLocation = FVector::ZeroVector;
	FVector ActiveLocation = FVector::ZeroVector;
	bool bTransportActive = false;
	bool bMoving = false;
};
