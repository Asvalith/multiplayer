// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.generated.h"

class ACharacter;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;
class AmultiplayerKeySocket;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerKeyPickedUpEvent,
	ACharacter*,
	Holder);

/** Server-authoritative replicated key that attaches to the first player who overlaps it. */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopKey : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopKey();

	virtual void Tick(float DeltaSeconds) override;

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop|Key")
	ACharacter* GetHolder() const { return Holder; }

	UFUNCTION(BlueprintPure, Category = "Coop|Key")
	bool IsHeldBy(const ACharacter* Character) const { return Holder == Character; }

	bool ConsumeAtSocket();
	bool InstallAtSocket(USceneComponent* SocketPoint);

	UPROPERTY(BlueprintAssignable, Category = "Coop|Key")
	FmultiplayerKeyPickedUpEvent OnKeyPickedUp;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandlePickupOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void OnRep_Holder();

	UFUNCTION()
	void OnRep_Installed();

	UFUNCTION()
	void HandleHolderDestroyed(AActor* DestroyedActor);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Key", meta = (DisplayName = "On Key Holder Changed"))
	void ReceiveKeyHolderChanged(ACharacter* NewHolder);

private:
	void PickupBy(ACharacter* Character);
	void ReleaseHolder(bool bBroadcastChange);
	void ApplyHeldState();
	void HandleHolderChanged();
	void HandleInstalledChanged();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<UStaticMeshComponent> KeyMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USphereComponent> PickupTrigger;

	UPROPERTY(EditAnywhere, Category = "Coop|Key")
	FName CarrySocketName = TEXT("KeySocket");

	/** Legacy direct-install link retained for existing placed Blueprint instances. */
	UPROPERTY(EditInstanceOnly, Category = "Coop|Key")
	TObjectPtr<AmultiplayerKeySocket> DestinationSocket;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual", meta = (ClampMin = "0.0"))
	float RotationSpeedDegrees = 90.0f;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual")
	FVector RotationAxis = FVector::UpVector;

	UPROPERTY(ReplicatedUsing = OnRep_Installed)
	bool bInstalled = false;

	UPROPERTY(ReplicatedUsing = OnRep_Holder)
	TObjectPtr<ACharacter> Holder;
};
