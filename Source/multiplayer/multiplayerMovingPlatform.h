// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerMovingPlatform.generated.h"

class ACharacter;
class UBoxComponent;
class UStaticMeshComponent;
class UmultiplayerTransporterComponent;
class AmultiplayerPressurePlate;
class USceneComponent;

UENUM(BlueprintType)
enum class EMovingPlatformActivationSource : uint8
{
	ExternalPressurePlate,
	PlatformOccupancy
};

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

	UFUNCTION()
	void HandleActivationPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Platform", meta = (DisplayName = "On Platform Occupancy Visual Changed"))
	void ReceivePlatformOccupancyChanged(int32 CurrentPlayers, int32 NeededPlayers);

private:
	void RefreshOccupancy();
	void RefreshActivation();
	void RemoveInvalidOccupants();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<USceneComponent> PlatformRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UStaticMeshComponent> PlatformMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UBoxComponent> ActivationVolume;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerTransporterComponent> Transporter;

	/** Fixed endpoints are captured at BeginPlay and may be positioned in a Blueprint child. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> StartPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> TargetPoint;

	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation")
	EMovingPlatformActivationSource ActivationSource = EMovingPlatformActivationSource::ExternalPressurePlate;

	/** A separately placed pressure plate; the platform never owns the plate. */
	UPROPERTY(EditInstanceOnly, Category = "Coop|Platform|Activation", meta = (EditCondition = "ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate", EditConditionHides))
	TObjectPtr<AmultiplayerPressurePlate> ActivationPlate;

	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation", meta = (ClampMin = "1", EditCondition = "ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy", EditConditionHides))
	int32 RequiredPlayers = 1;

	UPROPERTY(ReplicatedUsing = OnRep_PlayerCount)
	int32 ReplicatedPlayerCount = 0;

	/** Counts overlapping components per Character to keep occupancy stable. */
	TMap<TWeakObjectPtr<ACharacter>, int32> OccupantOverlapCounts;
};
