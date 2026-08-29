// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerKeySocket.generated.h"

class ACharacter;
class AmultiplayerCoopKey;
class UBoxComponent;
class USceneComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerKeySocketActivatedEvent);

/** Consumes one carried key on the server and replicates its activated state. */
UCLASS()
class MULTIPLAYER_API AmultiplayerKeySocket : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerKeySocket();

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop|Key Socket")
	bool IsActivated() const { return bActivated; }

	/** Installs a collected key directly into this rack slot on the server. */
	bool StoreCollectedKey(AmultiplayerCoopKey* Key);

	UPROPERTY(BlueprintAssignable, Category = "Coop|Key Socket")
	FmultiplayerKeySocketActivatedEvent OnSocketActivated;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleSocketOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void OnRep_Activated();

	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Key Socket", meta = (DisplayName = "On Socket Visual Activated"))
	void ReceiveSocketVisualActivated();

private:
	AmultiplayerCoopKey* FindCarriedKey(ACharacter* Character) const;
	void CommitServerActivation();
	void HandleActivatedChanged();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<UStaticMeshComponent> SocketMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Key Socket", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> KeyDisplayPoint;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<UBoxComponent> ActivationTrigger;

	UPROPERTY(ReplicatedUsing = OnRep_Activated)
	bool bActivated = false;
};
