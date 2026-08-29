// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerMovingPlatform.generated.h"

class AmultiplayerPressurePlate;
class UArrowComponent;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;
class UmultiplayerTransporterComponent;
class USceneComponent;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EMovingPlatformActivationSource : uint8
{
	ExternalPressurePlate,
	PlatformOccupancy
};

/**
 * Replicated moving platform.
 *
 * PlayerOccupancy owns overlap bookkeeping, while Transporter owns movement.
 * This actor only selects the activation source; transform replication handles
 * client presentation.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerMovingPlatform : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerMovingPlatform();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleActivationPlateChanged(
		AmultiplayerPressurePlate* Plate,
		bool bIsActive);

private:
	void RefreshActivation();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<USceneComponent> PlatformRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UStaticMeshComponent> PlatformMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UBoxComponent> ActivationVolume;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerTransporterComponent> Transporter;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> StartPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> TargetPoint;

	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation")
	EMovingPlatformActivationSource ActivationSource =
		EMovingPlatformActivationSource::ExternalPressurePlate;

	UPROPERTY(EditInstanceOnly, Category = "Coop|Platform|Activation", meta = (EditCondition = "ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate", EditConditionHides))
	TObjectPtr<AmultiplayerPressurePlate> ActivationPlate;

	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation", meta = (ClampMin = "1", EditCondition = "ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy", EditConditionHides))
	int32 RequiredPlayers = 1;

};
