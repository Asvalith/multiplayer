// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerTransporterComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerTransportStateEvent,
	bool,
	bAtActiveTarget);

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

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Coop|Transport")
	void SetTransportActive(bool bNewActive);

	UFUNCTION(BlueprintPure, Category = "Coop|Transport")
	bool IsTransportActive() const { return bTransportActive; }

	UFUNCTION(BlueprintPure, Category = "Coop|Transport")
	bool IsMoving() const { return bMoving; }

	UPROPERTY(BlueprintAssignable, Category = "Coop|Transport")
	FmultiplayerTransportStateEvent OnTransportTargetReached;

private:
	FVector GetTargetLocation() const;
	void FinishMovement();

	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	FVector ActiveOffset = FVector(0.0f, 0.0f, 500.0f);

	UPROPERTY(EditAnywhere, Category = "Coop|Transport", meta = (ClampMin = "1.0"))
	float MoveSpeed = 150.0f;

	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	bool bReturnWhenInactive = true;

	UPROPERTY(EditAnywhere, Category = "Coop|Transport")
	bool bOffsetUsesActorRotation = true;

	FVector StartLocation = FVector::ZeroVector;
	bool bTransportActive = false;
	bool bMoving = false;
};
