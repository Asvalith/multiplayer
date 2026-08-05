// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.generated.h"

class ACharacter;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;

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

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop|Key")
	ACharacter* GetHolder() const { return Holder; }

	UFUNCTION(BlueprintPure, Category = "Coop|Key")
	bool IsHeldBy(const ACharacter* Character) const { return Holder == Character; }

	bool ConsumeAtSocket();

	UPROPERTY(BlueprintAssignable, Category = "Coop|Key")
	FmultiplayerKeyPickedUpEvent OnKeyPickedUp;

protected:
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
	void HandleHolderDestroyed(AActor* DestroyedActor);

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Key", meta = (DisplayName = "On Key Holder Changed"))
	void ReceiveKeyHolderChanged(ACharacter* NewHolder);

private:
	void PickupBy(ACharacter* Character);
	void ReleaseHolder(bool bBroadcastChange);
	void ApplyHeldState();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<UStaticMeshComponent> KeyMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USphereComponent> PickupTrigger;

	UPROPERTY(EditAnywhere, Category = "Coop|Key")
	FName CarrySocketName = TEXT("KeySocket");

	UPROPERTY(ReplicatedUsing = OnRep_Holder)
	TObjectPtr<ACharacter> Holder;
};
