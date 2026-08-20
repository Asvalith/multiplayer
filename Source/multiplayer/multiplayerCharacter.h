// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "multiplayerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputMappingContext;
class UInputAction;
class UmultiplayerVictoryPresenterComponent;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNetworkActionCountChanged, int32, NewCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnServerActionConfirmed, int32, ConfirmedCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNetworkActionEffect, FVector, EffectLocation);

UCLASS(config=Game)
class AmultiplayerCharacter : public ACharacter
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

	/** Local-only bridge from replicated game-over state to the configured victory UMG. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Victory", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerVictoryPresenterComponent> VictoryPresenter;
	
	/** MappingContext */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputMappingContext* DefaultMappingContext;

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* LookAction;

public:
	AmultiplayerCharacter();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, Category = "Network|Debug")
	void PrintNetworkRole();

	UFUNCTION(BlueprintCallable, Category = "Network|Test")
	void RequestServerAction();

	UFUNCTION(BlueprintCallable, Category = "Network|Test")
	void RequestSpawnReplicatedCube();

	/** May be called by the victory widget on either player's owning client. */
	UFUNCTION(BlueprintCallable, Category = "Coop|Match")
	void RequestRestartCoopGame();

	UFUNCTION(BlueprintPure, Category = "Network|Test")
	int32 GetNetworkActionCount() const { return NetworkActionCount; }

	UPROPERTY(BlueprintAssignable, Category = "Network|Test")
	FOnNetworkActionCountChanged OnNetworkActionCountChanged;

	/** Only the owning client receives this server acknowledgement. */
	UPROPERTY(BlueprintAssignable, Category = "Network|Test")
	FOnServerActionConfirmed OnServerActionConfirmed;

	/** Every connected peer receives this transient presentation event. */
	UPROPERTY(BlueprintAssignable, Category = "Network|Test")
	FOnNetworkActionEffect OnNetworkActionEffect;

protected:

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);
			

protected:

	virtual void NotifyControllerChanged() override;

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestAction();

	UFUNCTION(Client, Reliable)
	void ClientConfirmServerAction(int32 ConfirmedCount);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayNetworkActionEffect(FVector_NetQuantize EffectLocation);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSpawnReplicatedCube(FVector_NetQuantize SpawnLocation);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRestartCoopGame();

	UFUNCTION()
	void OnRep_NetworkActionCount();

private:
	void BroadcastNetworkActionCount();

	UPROPERTY(ReplicatedUsing = OnRep_NetworkActionCount)
	int32 NetworkActionCount = 0;

public:
	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }
};

