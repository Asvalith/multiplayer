// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayCueInterface.h"
#include "GameplayTagContainer.h"
#include "Logging/LogMacros.h"
#include "TimerManager.h"
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
class AmultiplayerGASTargetDummy;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNetworkActionCountChanged, int32, NewCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnServerActionConfirmed, int32, ConfirmedCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNetworkActionEffect, FVector, EffectLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMultiplayerAbilitySystemInitialized);

UCLASS(config=Game)
class AmultiplayerCharacter : public ACharacter, public IAbilitySystemInterface, public IGameplayCueInterface
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

	/** Optional editor-authored set. The C++ demo abilities are used when this is unset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerAbilitySet> StartupAbilitySet;

	/** Formal Enhanced Input to GAS tag mapping. Keys 4/5/6 remain debug fallbacks. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS|Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerInputConfig> AbilityInputConfig;

	/** Dedicated mapping context for formal GAS actions. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS|Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> AbilityMappingContext;

	/** Owns the local GAS HUD and its binding lifecycle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS|UI", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerGASHUDPresenterComponent> GASHUDPresenter;

public:
	AmultiplayerCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GameplayCueDefaultHandler(
		EGameplayCueEvent::Type EventType,
		const FGameplayCueParameters& Parameters) override;

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

	UFUNCTION(BlueprintCallable, Category = "Network|Debug")
	void PrintNetworkRole();

	UFUNCTION(BlueprintCallable, Category = "Network|Test")
	void RequestServerAction();

	UFUNCTION(BlueprintCallable, Category = "Network|Test")
	void RequestSpawnReplicatedCube();

	/** May be called by the victory widget on either player's owning client. */
	UFUNCTION(BlueprintCallable, Category = "Coop|Match")
	void RequestRestartCoopGame();

	/** Called on server and clients by the persistent PlayerState death state. */
	void ApplyDeathState(bool bNewDeadState);

	/** Idempotently reconciles the local-only M6 pending presentation. */
	void ReconcilePredictionLabPendingPresentation(
		const TCHAR* Outcome,
		int16 PredictionKey);

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

	UPROPERTY(BlueprintAssignable, Category = "GAS")
	FOnMultiplayerAbilitySystemInitialized OnAbilitySystemInitialized;

protected:

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

	void DamageAbilityPressed();
	void DamageAbilityReleased();
	void HealAbilityPressed();
	void HealAbilityReleased();
	void ImmunityAbilityPressed();
	void ImmunityAbilityReleased();
	void RequestBaselineEnemyTarget();
	void RequestBaselineEnemyDamage();
	void RequestArmNextImmunityPredictionRejection();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestBaselineEnemyTarget();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestBaselineEnemyDamage();


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
	void InitializeAbilitySystem();
	void AbilityInputTagPressed(FGameplayTag InputTag);
	void AbilityInputTagReleased(FGameplayTag InputTag);
	void ShowGameplayCueFlash(const FLinearColor& Color, float Intensity, float Duration);
	void ClearGameplayCueFlash();
	void SetGameplayCueState(const FLinearColor& Color, float Intensity);
	void ClearGameplayCueState();
	void RefreshGameplayCueState();
	void PositionGameplayCueFlashFromImpact(const FVector& ImpactImpulse);
	void TryStartGASAutomation();
	void RunNextGASM5AutomationStep();
	void RunNextGASM6AutomationStep();
	void RunNextGASM6IntentAutomationStep();
	void ScheduleNextGASM5AutomationStep(float DelaySeconds);
	bool AimGASM5AutomationAtTarget();
	void LogGASM6Snapshot(const TCHAR* Phase) const;
	void FailGASM6Automation(const TCHAR* Reason);
	bool IsGASM6AutomationTimedOut() const;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	UPROPERTY(Transient)
	TObjectPtr<AmultiplayerGASTargetDummy> BaselineEnemyTarget;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueFlashLight;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueStateLight;

	FTimerHandle GameplayCueFlashTimer;
	FTimerHandle GASM5AutomationTimer;
	bool bGameplayCueImmunityActive = false;
	bool bGameplayCueVulnerabilityActive = false;
	bool bGameplayCueDeathActive = false;
	bool bGameplayCuePredictionPendingActive = false;
	int32 GASM5AutomationStep = 0;
	int32 GASM6AutomationStep = 0;
	int32 GASM6IntentAutomationStep = 0;
	uint32 GASM6AutomationTrialId = 6000;
	uint32 GASM6IntentResultSerialBefore = 0;
	float GASM6AutomationDeadlineSeconds = 0.0f;

	UPROPERTY(ReplicatedUsing = OnRep_NetworkActionCount)
	int32 NetworkActionCount = 0;

public:
	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }
};

