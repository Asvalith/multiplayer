// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "AbilitySystem/Abilities/multiplayerAbilityPresentationInterface.h"
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayCueInterface.h"
#include "GameplayTagContainer.h"
#include "Logging/LogMacros.h"
#include "multiplayerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputMappingContext;
class UInputAction;
class UPointLightComponent;
class UmultiplayerVictoryPresenterComponent;
class UmultiplayerAbilitySet;
class UmultiplayerAbilitySystemComponent;
class UmultiplayerAttributeSet;
class UmultiplayerInputConfig;
class UmultiplayerGASHUDPresenterComponent;
class UmultiplayerGASDeveloperHarnessComponent;
class UmultiplayerGASCuePresenterComponent;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMultiplayerAbilitySystemInitialized);

UCLASS(config=Game)
class AmultiplayerCharacter : public ACharacter,
	public IAbilitySystemInterface,
	public IGameplayCueInterface,
	public ImultiplayerAbilityPresentationInterface
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

	/** Local bridge from replicated game-over state to the Character Blueprint event. */
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

	/** Single authoritative startup ability/effect set for this character. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerAbilitySet> StartupAbilitySet;

	/** Formal Enhanced Input to GAS tag mapping. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS|Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerInputConfig> AbilityInputConfig;

	/** Dedicated mapping context for formal GAS actions. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS|Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> AbilityMappingContext;

	/** Owns the local GAS HUD and its binding lifecycle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS|UI", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerGASHUDPresenterComponent> GASHUDPresenter;

	/** Owns local GameplayCue lights and prediction presentation state. */
	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerGASCuePresenterComponent> GASCuePresenter;

	/** Non-shipping multiplayer/GAS verification fixture, isolated from gameplay responsibilities. */
	UPROPERTY(VisibleAnywhere, Category = "Developer", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerGASDeveloperHarnessComponent> GASDeveloperHarness;

public:
	AmultiplayerCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void GameplayCueDefaultHandler(
		EGameplayCueEvent::Type EventType,
		const FGameplayCueParameters& Parameters) override;
	virtual void HandleAbilityPresentation_Implementation(
		const FmultiplayerAbilityPresentationEvent& Event) override;

	UFUNCTION(BlueprintPure, Category = "GAS")
	UmultiplayerAbilitySystemComponent* GetMultiplayerAbilitySystemComponent() const;

	UFUNCTION(BlueprintPure, Category = "GAS")
	UmultiplayerAttributeSet* GetMultiplayerAttributeSet() const { return AttributeSet; }

	UFUNCTION(BlueprintPure, Category = "GAS|Input")
	UmultiplayerInputConfig* GetAbilityInputConfig() const { return AbilityInputConfig; }

	UFUNCTION(BlueprintPure, Category = "GAS|Input")
	UInputMappingContext* GetAbilityMappingContext() const { return AbilityMappingContext; }

	UFUNCTION(BlueprintPure, Category = "GAS")
	UmultiplayerAbilitySet* GetStartupAbilitySet() const { return StartupAbilitySet; }

	/** Local Blueprint presentation hook fired once when replicated victory arrives. */
	UFUNCTION(
		BlueprintImplementableEvent,
		Category = "Coop|Victory",
		meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

	/** May be called by the victory widget on either player's owning client. */
	UFUNCTION(
		BlueprintCallable,
		Category = "Coop|Victory",
		meta = (DisplayName = "Request Restart Coop Game"))
	void RequestRestartCoopGame();

	/** Called on server and clients by the persistent PlayerState death state. */
	void ApplyDeathState(bool bNewDeadState);

	UPROPERTY(BlueprintAssignable, Category = "GAS")
	FOnMultiplayerAbilitySystemInitialized OnAbilitySystemInitialized;

protected:

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

protected:

	virtual void NotifyControllerChanged() override;

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRestartCoopGame();

private:
	void InitializeAbilitySystem();
	void AbilityInputTagPressed(FGameplayTag InputTag);
	void AbilityInputTagReleased(FGameplayTag InputTag);

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueFlashLight;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueStateLight;

public:
	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }
};

