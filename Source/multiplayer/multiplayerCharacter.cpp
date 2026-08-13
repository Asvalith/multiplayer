// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCharacter.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Input/multiplayerInputConfig.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "multiplayer.h"
#include "multiplayerGASTargetDummy.h"
#include "multiplayerReplicatedCube.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "multiplayerGameMode.h"
#include "Net/UnrealNetwork.h"
#include "Player/multiplayerGASPlayerState.h"
#include "UI/multiplayerGASHUDPresenterComponent.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

namespace
{
	bool bGASM5AutomationStarted = false;
}

//////////////////////////////////////////////////////////////////////////
// AmultiplayerCharacter

AmultiplayerCharacter::AmultiplayerCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true; // Character moves in the direction of input...	
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f); // ...at this rotation rate

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 700.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f; // The camera follows at this distance behind the character	
	CameraBoom->bUsePawnControlRotation = true; // Rotate the arm based on the controller

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName); // Attach the camera to the end of the boom and let the boom adjust to match the controller orientation
	FollowCamera->bUsePawnControlRotation = false; // Camera does not rotate relative to arm

	VictoryPresenter = CreateDefaultSubobject<UmultiplayerVictoryPresenterComponent>(TEXT("VictoryPresenter"));
	GASHUDPresenter = CreateDefaultSubobject<UmultiplayerGASHUDPresenterComponent>(TEXT("GASHUDPresenter"));

	GameplayCueFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GameplayCueFlashLight"));
	GameplayCueFlashLight->SetupAttachment(RootComponent);
	GameplayCueFlashLight->SetRelativeLocation(FVector(0.0f, 0.0f, 80.0f));
	GameplayCueFlashLight->SetAttenuationRadius(260.0f);
	GameplayCueFlashLight->SetIntensity(0.0f);
	GameplayCueFlashLight->SetVisibility(false);

	GameplayCueStateLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GameplayCueStateLight"));
	GameplayCueStateLight->SetupAttachment(RootComponent);
	GameplayCueStateLight->SetRelativeLocation(FVector(0.0f, 0.0f, 110.0f));
	GameplayCueStateLight->SetAttenuationRadius(180.0f);
	GameplayCueStateLight->SetIntensity(0.0f);
	GameplayCueStateLight->SetVisibility(false);

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

//////////////////////////////////////////////////////////////////////////
// Input

void AmultiplayerCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitializeAbilitySystem();
}

void AmultiplayerCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitializeAbilitySystem();
}

void AmultiplayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(GameplayCueFlashTimer);
	GetWorldTimerManager().ClearTimer(GASM5AutomationTimer);
	ClearGameplayCueFlash();
	ClearGameplayCueState();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCharacter::GameplayCueDefaultHandler(
	EGameplayCueEvent::Type EventType,
	const FGameplayCueParameters& Parameters)
{
	const FGameplayTag CueTag = Parameters.OriginalTag;
	bool bHandled = true;
	bool bCritical = false;
	EmultiplayerHitType HitType = EmultiplayerHitType::Direct;
	FVector ImpactImpulse = FVector::ZeroVector;

	if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Damage_Cast)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(1.0f, 0.75f, 0.05f), 4500.0f, 0.18f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact)
		&& EventType == EGameplayCueEvent::Executed)
	{
		const FGameplayEffectContext* BaseContext = Parameters.EffectContext.Get();
		const FmultiplayerGameplayEffectContext* ProjectContext =
			BaseContext != nullptr
			&& BaseContext->GetScriptStruct()->IsChildOf(
				FmultiplayerGameplayEffectContext::StaticStruct())
				? static_cast<const FmultiplayerGameplayEffectContext*>(BaseContext)
				: nullptr;
		bCritical = ProjectContext != nullptr && ProjectContext->IsCriticalHit();
		if (ProjectContext != nullptr)
		{
			HitType = ProjectContext->GetHitType();
			ImpactImpulse = ProjectContext->GetImpactImpulse();
			PositionGameplayCueFlashFromImpact(ImpactImpulse);
		}
		ShowGameplayCueFlash(
			bCritical ? FLinearColor(1.0f, 0.15f, 0.0f) : FLinearColor(1.0f, 0.0f, 0.0f),
			bCritical ? 8000.0f : 5000.0f,
			bCritical ? 0.35f : 0.22f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Heal_Cast)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(0.1f, 0.6f, 1.0f), 4200.0f, 0.18f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Heal_Result)
		&& EventType == EGameplayCueEvent::Executed)
	{
		ShowGameplayCueFlash(FLinearColor(0.0f, 1.0f, 0.15f), 6000.0f, 0.3f);
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_State_Immunity))
	{
		if (EventType == EGameplayCueEvent::Removed)
		{
			bGameplayCueImmunityActive = false;
			RefreshGameplayCueState();
		}
		else if (EventType == EGameplayCueEvent::OnActive
			|| EventType == EGameplayCueEvent::WhileActive)
		{
			bGameplayCueImmunityActive = true;
			RefreshGameplayCueState();
		}
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability))
	{
		if (EventType == EGameplayCueEvent::Removed)
		{
			bGameplayCueVulnerabilityActive = false;
			RefreshGameplayCueState();
		}
		else if (EventType == EGameplayCueEvent::OnActive
			|| EventType == EGameplayCueEvent::WhileActive)
		{
			bGameplayCueVulnerabilityActive = true;
			RefreshGameplayCueState();
		}
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending))
	{
		if (EventType == EGameplayCueEvent::Removed)
		{
			bGameplayCuePredictionPendingActive = false;
			RefreshGameplayCueState();
		}
		else if (EventType == EGameplayCueEvent::OnActive
			|| EventType == EGameplayCueEvent::WhileActive)
		{
			bGameplayCuePredictionPendingActive = true;
			RefreshGameplayCueState();
		}
	}
	else if (CueTag.MatchesTagExact(MultiplayerGameplayTags::GameplayCue_Coop_Death)
		&& EventType == EGameplayCueEvent::Executed)
	{
		bGameplayCueDeathActive = true;
		RefreshGameplayCueState();
	}
	else
	{
		bHandled = false;
	}

	if (!bHandled)
	{
		IGameplayCueInterface::GameplayCueDefaultHandler(EventType, Parameters);
		return;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_CUE_HANDLER Actor=%s Cue=%s Event=%s Role=%s Local=%s Critical=%s HitType=%s ImpactDir=%s"),
		*GetName(),
		*CueTag.ToString(),
		*UEnum::GetValueAsString(EventType),
		*UEnum::GetValueAsString(GetLocalRole()),
		IsLocallyControlled() ? TEXT("true") : TEXT("false"),
		bCritical ? TEXT("true") : TEXT("false"),
		*UEnum::GetValueAsString(HitType),
		*ImpactImpulse.GetSafeNormal().ToCompactString());
}

void AmultiplayerCharacter::ShowGameplayCueFlash(
	const FLinearColor& Color,
	float Intensity,
	float Duration)
{
	if (GameplayCueFlashLight == nullptr)
	{
		return;
	}

	GameplayCueFlashLight->SetLightColor(Color);
	GameplayCueFlashLight->SetIntensity(Intensity);
	GameplayCueFlashLight->SetVisibility(true);
	GetWorldTimerManager().SetTimer(
		GameplayCueFlashTimer,
		this,
		&AmultiplayerCharacter::ClearGameplayCueFlash,
		FMath::Max(Duration, 0.01f),
		false);
}

void AmultiplayerCharacter::ClearGameplayCueFlash()
{
	if (GameplayCueFlashLight != nullptr)
	{
		GameplayCueFlashLight->SetIntensity(0.0f);
		GameplayCueFlashLight->SetVisibility(false);
	}
}

void AmultiplayerCharacter::SetGameplayCueState(
	const FLinearColor& Color,
	float Intensity)
{
	if (GameplayCueStateLight != nullptr)
	{
		GameplayCueStateLight->SetLightColor(Color);
		GameplayCueStateLight->SetIntensity(Intensity);
		GameplayCueStateLight->SetVisibility(true);
	}
}

void AmultiplayerCharacter::ClearGameplayCueState()
{
	if (GameplayCueStateLight != nullptr)
	{
		GameplayCueStateLight->SetIntensity(0.0f);
		GameplayCueStateLight->SetVisibility(false);
	}
}

void AmultiplayerCharacter::ReconcilePredictionLabPendingPresentation(
	const TCHAR* Outcome,
	int16 PredictionKey)
{
	bGameplayCuePredictionPendingActive = false;
	RefreshGameplayCueState();
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_VISUAL Phase=PendingCleared Outcome=%s PredictionKey=%d Actor=%s PendingVisual=false"),
		Outcome,
		static_cast<int32>(PredictionKey),
		*GetName());
}

void AmultiplayerCharacter::RefreshGameplayCueState()
{
	if (bGameplayCueDeathActive)
	{
		SetGameplayCueState(FLinearColor(1.0f, 0.0f, 0.0f), 6500.0f);
	}
	else if (bGameplayCuePredictionPendingActive)
	{
		SetGameplayCueState(FLinearColor(1.0f, 0.0f, 1.0f), 2500.0f);
	}
	else if (bGameplayCueImmunityActive)
	{
		SetGameplayCueState(FLinearColor(0.0f, 0.35f, 1.0f), 3200.0f);
	}
	else if (bGameplayCueVulnerabilityActive)
	{
		SetGameplayCueState(FLinearColor(0.75f, 0.0f, 1.0f), 3000.0f);
	}
	else
	{
		ClearGameplayCueState();
	}
}

void AmultiplayerCharacter::PositionGameplayCueFlashFromImpact(
	const FVector& ImpactImpulse)
{
	if (GameplayCueFlashLight == nullptr || ImpactImpulse.IsNearlyZero())
	{
		return;
	}

	const FVector IncomingDirection = ImpactImpulse.GetSafeNormal();
	GameplayCueFlashLight->SetWorldLocation(
		GetActorLocation() - IncomingDirection * 45.0f + FVector(0.0f, 0.0f, 80.0f));
}

UAbilitySystemComponent* AmultiplayerCharacter::GetAbilitySystemComponent() const
{
	if (AbilitySystemComponent != nullptr)
	{
		return AbilitySystemComponent;
	}

	const AmultiplayerGASPlayerState* GASPlayerState = GetPlayerState<AmultiplayerGASPlayerState>();
	return GASPlayerState != nullptr ? GASPlayerState->GetAbilitySystemComponent() : nullptr;
}

UmultiplayerAbilitySystemComponent* AmultiplayerCharacter::GetMultiplayerAbilitySystemComponent() const
{
	return Cast<UmultiplayerAbilitySystemComponent>(GetAbilitySystemComponent());
}

void AmultiplayerCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();
	VictoryPresenter->RefreshBinding();
	GASHUDPresenter->RefreshBinding();
	InitializeAbilitySystem();

	// Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
			if (AbilityMappingContext != nullptr)
			{
				Subsystem->AddMappingContext(AbilityMappingContext, 1);
			}
		}
	}
}

void AmultiplayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AmultiplayerCharacter::Move);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AmultiplayerCharacter::Look);

		// Formal GAS input is data-driven: InputAction -> InputTag -> AbilitySpec.
		if (AbilityInputConfig != nullptr)
		{
			for (const FmultiplayerTaggedInputAction& Entry : AbilityInputConfig->GetAbilityInputActions())
			{
				if (Entry.InputAction == nullptr || !Entry.InputTag.IsValid())
				{
					continue;
				}

				EnhancedInputComponent->BindAction(
					Entry.InputAction,
					ETriggerEvent::Started,
					this,
					&AmultiplayerCharacter::AbilityInputTagPressed,
					Entry.InputTag);
				EnhancedInputComponent->BindAction(
					Entry.InputAction,
					ETriggerEvent::Completed,
					this,
					&AmultiplayerCharacter::AbilityInputTagReleased,
					Entry.InputTag);
			}
		}

		// Temporary network verification controls.
		PlayerInputComponent->BindKey(EKeys::One, IE_Pressed, this, &AmultiplayerCharacter::PrintNetworkRole);
		PlayerInputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AmultiplayerCharacter::RequestServerAction);
		PlayerInputComponent->BindKey(EKeys::Three, IE_Pressed, this, &AmultiplayerCharacter::RequestSpawnReplicatedCube);

		// Asset-free GAS test controls. These can later be replaced by InputAction assets.
		PlayerInputComponent->BindKey(EKeys::Four, IE_Pressed, this, &AmultiplayerCharacter::DamageAbilityPressed);
		PlayerInputComponent->BindKey(EKeys::Four, IE_Released, this, &AmultiplayerCharacter::DamageAbilityReleased);
		PlayerInputComponent->BindKey(EKeys::Five, IE_Pressed, this, &AmultiplayerCharacter::HealAbilityPressed);
		PlayerInputComponent->BindKey(EKeys::Five, IE_Released, this, &AmultiplayerCharacter::HealAbilityReleased);
		PlayerInputComponent->BindKey(EKeys::Six, IE_Pressed, this, &AmultiplayerCharacter::ImmunityAbilityPressed);
		PlayerInputComponent->BindKey(EKeys::Six, IE_Released, this, &AmultiplayerCharacter::ImmunityAbilityReleased);
		PlayerInputComponent->BindKey(EKeys::Seven, IE_Pressed, this, &AmultiplayerCharacter::RequestBaselineEnemyTarget);
		PlayerInputComponent->BindKey(EKeys::Eight, IE_Pressed, this, &AmultiplayerCharacter::RequestBaselineEnemyDamage);
		PlayerInputComponent->BindKey(EKeys::Nine, IE_Pressed, this, &AmultiplayerCharacter::RequestArmNextImmunityPredictionRejection);
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AmultiplayerCharacter::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AmultiplayerCharacter, NetworkActionCount);
}

void AmultiplayerCharacter::PrintNetworkRole()
{
	const TCHAR* NetModeText = TEXT("Unknown");
	switch (GetNetMode())
	{
	case NM_Standalone:
		NetModeText = TEXT("Standalone");
		break;
	case NM_DedicatedServer:
		NetModeText = TEXT("DedicatedServer");
		break;
	case NM_ListenServer:
		NetModeText = TEXT("ListenServer");
		break;
	case NM_Client:
		NetModeText = TEXT("Client");
		break;
	default:
		break;
	}

	const FString Message = FString::Printf(
		TEXT("%s | NetMode=%s LocalRole=%s RemoteRole=%s Authority=%s LocallyControlled=%s"),
		*GetName(),
		NetModeText,
		*UEnum::GetValueAsString(GetLocalRole()),
		*UEnum::GetValueAsString(GetRemoteRole()),
		HasAuthority() ? TEXT("true") : TEXT("false"),
		IsLocallyControlled() ? TEXT("true") : TEXT("false")
	);

	UE_LOG(LogTemplateCharacter, Log, TEXT("%s"), *Message);

	const AmultiplayerGASPlayerState* GASPlayerState =
		GetPlayerState<AmultiplayerGASPlayerState>();
	const UmultiplayerAbilitySystemComponent* GASASC =
		GetMultiplayerAbilitySystemComponent();
	const int32 AbilityCount = GASASC != nullptr
		? GASASC->GetActivatableAbilities().Num()
		: 0;
	const FString GASMessage = FString::Printf(
		TEXT("GAS_BASELINE Character=%s PlayerState=%s ASC=%s Owner=%s Avatar=%s Abilities=%d Health=%.1f Energy=%.1f"),
		*GetName(),
		*GetNameSafe(GASPlayerState),
		*GetNameSafe(GASASC),
		GASASC != nullptr ? *GetNameSafe(GASASC->GetOwnerActor()) : TEXT("None"),
		GASASC != nullptr ? *GetNameSafe(GASASC->GetAvatarActor()) : TEXT("None"),
		AbilityCount,
		AttributeSet != nullptr ? AttributeSet->GetHealth() : 0.0f,
		AttributeSet != nullptr ? AttributeSet->GetEnergy() : 0.0f);

	UE_LOG(LogMultiplayerGAS, Display, TEXT("%s"), *GASMessage);
	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(
			-1,
			5.0f,
			HasAuthority() ? FColor::Green : FColor::Cyan,
			Message
		);
		GEngine->AddOnScreenDebugMessage(
			-1,
			5.0f,
			FColor::Yellow,
			GASMessage);
	}
}

void AmultiplayerCharacter::RequestServerAction()
{
	ServerRequestAction();
}

bool AmultiplayerCharacter::ServerRequestAction_Validate()
{
	return !IsActorBeingDestroyed();
}

void AmultiplayerCharacter::ServerRequestAction_Implementation()
{
	++NetworkActionCount;
	BroadcastNetworkActionCount();

	// Persistent state uses replication; these RPCs only carry acknowledgement
	// and transient presentation, so late joiners do not depend on them.
	ClientConfirmServerAction(NetworkActionCount);
	MulticastPlayNetworkActionEffect(GetActorLocation());
}

void AmultiplayerCharacter::DamageAbilityPressed()
{
	AbilityInputTagPressed(MultiplayerGameplayTags::InputTag_Ability_Damage);
}

void AmultiplayerCharacter::DamageAbilityReleased()
{
	AbilityInputTagReleased(MultiplayerGameplayTags::InputTag_Ability_Damage);
}

void AmultiplayerCharacter::HealAbilityPressed()
{
	AbilityInputTagPressed(MultiplayerGameplayTags::InputTag_Ability_Heal);
}

void AmultiplayerCharacter::HealAbilityReleased()
{
	AbilityInputTagReleased(MultiplayerGameplayTags::InputTag_Ability_Heal);
}

void AmultiplayerCharacter::ImmunityAbilityPressed()
{
	AbilityInputTagPressed(MultiplayerGameplayTags::InputTag_Ability_Immunity);
}

void AmultiplayerCharacter::ImmunityAbilityReleased()
{
	AbilityInputTagReleased(MultiplayerGameplayTags::InputTag_Ability_Immunity);
}

void AmultiplayerCharacter::RequestBaselineEnemyTarget()
{
	ServerRequestBaselineEnemyTarget();
}

void AmultiplayerCharacter::RequestBaselineEnemyDamage()
{
	ServerRequestBaselineEnemyDamage();
}

void AmultiplayerCharacter::RequestArmNextImmunityPredictionRejection()
{
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("GASM6Lab")))
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M6_REJECT Phase=ArmIgnored Reason=MissingGASM6Lab Actor=%s"),
			*GetName());
		return;
	}
	if (AbilitySystemComponent != nullptr)
	{
		// Use the same replicated ASC/PlayerState actor channel as ability
		// activation, then wait for the explicit client acknowledgement before
		// activating Immunity. The test does not depend on a fixed network delay.
		++GASM6AutomationTrialId;
		AbilitySystemComponent->ServerArmNextImmunityPredictionRejection(
			GASM6AutomationTrialId);
	}
#endif
}

bool AmultiplayerCharacter::ServerRequestBaselineEnemyDamage_Validate()
{
	return !IsActorBeingDestroyed();
}

void AmultiplayerCharacter::ServerRequestBaselineEnemyDamage_Implementation()
{
	if (!IsValid(BaselineEnemyTarget))
	{
		return;
	}

	UAbilitySystemComponent* EnemyASC = BaselineEnemyTarget->GetAbilitySystemComponent();
	UAbilitySystemComponent* PlayerASC = GetAbilitySystemComponent();
	if (EnemyASC == nullptr || PlayerASC == nullptr)
	{
		return;
	}

	FGameplayEffectContextHandle Context = EnemyASC->MakeEffectContext();
	Context.AddSourceObject(BaselineEnemyTarget);
	FGameplayEffectSpecHandle DamageSpec = EnemyASC->MakeOutgoingSpec(
		UmultiplayerDamageEffect::StaticClass(),
		1.0f,
		Context);
	if (DamageSpec.IsValid())
	{
		DamageSpec.Data->SetSetByCallerMagnitude(
			MultiplayerGameplayTags::Data_Damage,
			25.0f);
		EnemyASC->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data.Get(), PlayerASC);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_ENEMY_DAMAGE Source=%s Target=%s Amount=25"),
			*GetNameSafe(BaselineEnemyTarget),
			*GetName());
	}
}

void AmultiplayerCharacter::ApplyDeathState(bool bNewDeadState)
{
	bGameplayCueDeathActive = bNewDeadState;
	if (!bNewDeadState)
	{
		bGameplayCueImmunityActive = false;
		bGameplayCueVulnerabilityActive = false;
		bGameplayCuePredictionPendingActive = false;
	}
	RefreshGameplayCueState();

	if (bNewDeadState)
	{
		GetCharacterMovement()->DisableMovement();
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
		{
			DisableInput(PlayerController);
		}
	}
	else
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
		{
			EnableInput(PlayerController);
		}
	}
}

bool AmultiplayerCharacter::ServerRequestBaselineEnemyTarget_Validate()
{
	return !IsActorBeingDestroyed();
}

void AmultiplayerCharacter::ServerRequestBaselineEnemyTarget_Implementation()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const FVector SpawnLocation =
		GetActorLocation() + GetActorForwardVector() * 300.0f;
	if (IsValid(BaselineEnemyTarget))
	{
		BaselineEnemyTarget->ResetForBaseline(SpawnLocation);
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = this;
	SpawnParameters.Instigator = this;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	BaselineEnemyTarget = World->SpawnActor<AmultiplayerGASTargetDummy>(
		AmultiplayerGASTargetDummy::StaticClass(),
		SpawnLocation,
		GetActorRotation(),
		SpawnParameters);
}

void AmultiplayerCharacter::AbilityInputTagPressed(FGameplayTag InputTag)
{
	if (const AmultiplayerGASPlayerState* GASPlayerState =
		GetPlayerState<AmultiplayerGASPlayerState>();
		GASPlayerState != nullptr && GASPlayerState->IsDead())
	{
		return;
	}

	if (UmultiplayerAbilitySystemComponent* ASC = GetMultiplayerAbilitySystemComponent())
	{
		ASC->AbilityInputTagPressed(InputTag);
	}
}

void AmultiplayerCharacter::AbilityInputTagReleased(FGameplayTag InputTag)
{
	if (UmultiplayerAbilitySystemComponent* ASC = GetMultiplayerAbilitySystemComponent())
	{
		ASC->AbilityInputTagReleased(InputTag);
	}
}

void AmultiplayerCharacter::InitializeAbilitySystem()
{
	AmultiplayerGASPlayerState* GASPlayerState = GetPlayerState<AmultiplayerGASPlayerState>();
	if (GASPlayerState == nullptr)
	{
		return;
	}

	UmultiplayerAbilitySystemComponent* NewAbilitySystemComponent =
		GASPlayerState->GetMultiplayerAbilitySystemComponent();
	UmultiplayerAttributeSet* NewAttributeSet = GASPlayerState->GetAttributeSet();
	if (NewAbilitySystemComponent == nullptr || NewAttributeSet == nullptr)
	{
		return;
	}

	const bool bActorInfoAlreadyInitialized =
		AbilitySystemComponent == NewAbilitySystemComponent
		&& AttributeSet == NewAttributeSet
		&& NewAbilitySystemComponent->GetOwnerActor() == GASPlayerState
		&& NewAbilitySystemComponent->GetAvatarActor() == this;

	if (!bActorInfoAlreadyInitialized)
	{
		GASPlayerState->InitializeAbilityActorInfo(this);
		AbilitySystemComponent = NewAbilitySystemComponent;
		AttributeSet = NewAttributeSet;
	}

	if (HasAuthority())
	{
		GASPlayerState->GrantStartupAbilities(StartupAbilitySet);
	}

	if (!bActorInfoAlreadyInitialized)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_INIT Character=%s PlayerState=%s Owner=%s Avatar=%s LocalRole=%s LocallyControlled=%s"),
			*GetName(),
			*GetNameSafe(GASPlayerState),
			*GetNameSafe(AbilitySystemComponent->GetOwnerActor()),
			*GetNameSafe(AbilitySystemComponent->GetAvatarActor()),
			*UEnum::GetValueAsString(GetLocalRole()),
			IsLocallyControlled() ? TEXT("true") : TEXT("false"));

		OnAbilitySystemInitialized.Broadcast();
	}
	else
	{
		UE_LOG(
			LogMultiplayerGAS,
			Verbose,
			TEXT("GAS_INIT_SKIPPED Character=%s PlayerState=%s reason=AlreadyInitialized"),
			*GetName(),
			*GetNameSafe(GASPlayerState));
	}

	GASHUDPresenter->RefreshBinding();
	ApplyDeathState(GASPlayerState->IsDead());
	TryStartGASAutomation();
}

void AmultiplayerCharacter::TryStartGASAutomation()
{
#if !UE_BUILD_SHIPPING
	if (bGASM5AutomationStarted
		|| !IsLocallyControlled()
		|| (!FParse::Param(FCommandLine::Get(), TEXT("GASM5Auto"))
			&& !FParse::Param(FCommandLine::Get(), TEXT("GASM6Auto"))
			&& !FParse::Param(FCommandLine::Get(), TEXT("GASM6IntentAuto"))))
	{
		return;
	}

	bGASM5AutomationStarted = true;
	GASM5AutomationStep = 0;
	GASM6AutomationStep = 0;
	GASM6IntentAutomationStep = 0;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_AUTO Phase=Started Stage=%s Actor=%s Role=%s"),
		FParse::Param(FCommandLine::Get(), TEXT("GASM6IntentAuto"))
			? TEXT("M6Intent")
			: (FParse::Param(FCommandLine::Get(), TEXT("GASM6Auto"))
				? TEXT("M6")
				: TEXT("M5")),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()));
	ScheduleNextGASM5AutomationStep(2.0f);
#endif
}

void AmultiplayerCharacter::ScheduleNextGASM5AutomationStep(float DelaySeconds)
{
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().SetTimer(
		GASM5AutomationTimer,
		this,
		&AmultiplayerCharacter::RunNextGASM5AutomationStep,
		FMath::Max(DelaySeconds, 0.05f),
		false);
#endif
}

void AmultiplayerCharacter::RunNextGASM5AutomationStep()
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("GASM6Auto")))
	{
		RunNextGASM6AutomationStep();
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("GASM6IntentAuto")))
	{
		RunNextGASM6IntentAutomationStep();
		return;
	}

	const int32 ExecutedStep = GASM5AutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M5_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetName());

	switch (ExecutedStep)
	{
	case 0:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(2.0f);
		break;
	case 1:
	case 2:
	case 3:
		if (!AimGASM5AutomationAtTarget())
		{
			--GASM5AutomationStep;
			ScheduleNextGASM5AutomationStep(0.5f);
			break;
		}
		DamageAbilityPressed();
		DamageAbilityReleased();
		ScheduleNextGASM5AutomationStep(ExecutedStep == 3 ? 2.0f : 1.3f);
		break;
	case 4:
		ImmunityAbilityPressed();
		ImmunityAbilityReleased();
		ScheduleNextGASM5AutomationStep(6.0f);
		break;
	case 5:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(0.8f);
		break;
	case 6:
		AimGASM5AutomationAtTarget();
		DamageAbilityPressed();
		DamageAbilityReleased();
		ScheduleNextGASM5AutomationStep(8.8f);
		break;
	case 7:
		UE_LOG(LogMultiplayerGAS, Display, TEXT("GAS_M5_AUTO Phase=VulnerabilityExpiryCheckpoint"));
		AimGASM5AutomationAtTarget();
		DamageAbilityPressed();
		DamageAbilityReleased();
		ScheduleNextGASM5AutomationStep(0.6f);
		break;
	case 8:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(1.0f);
		break;
	case 9:
		RequestBaselineEnemyDamage();
		ScheduleNextGASM5AutomationStep(0.8f);
		break;
	case 10:
		HealAbilityPressed();
		HealAbilityReleased();
		ScheduleNextGASM5AutomationStep(3.5f);
		break;
	case 11:
	case 12:
	case 13:
		RequestBaselineEnemyDamage();
		ScheduleNextGASM5AutomationStep(0.5f);
		break;
	case 14:
		RequestBaselineEnemyDamage();
		UE_LOG(LogMultiplayerGAS, Display, TEXT("GAS_M5_AUTO Phase=SequenceComplete AwaitingDeathRespawn=true"));
		break;
	default:
		break;
	}
#endif
}

void AmultiplayerCharacter::RunNextGASM6AutomationStep()
{
#if !UE_BUILD_SHIPPING
	const int32 ExecutedStep = GASM6AutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetName());

	switch (ExecutedStep)
	{
	case 0:
		RequestArmNextImmunityPredictionRejection();
		LogGASM6Snapshot(TEXT("Initial"));
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	case 1:
		if (AbilitySystemComponent == nullptr
			|| !AbilitySystemComponent->HasPredictionRejectLabArmResult(
				GASM6AutomationTrialId))
		{
			if (IsGASM6AutomationTimedOut())
			{
				FailGASM6Automation(TEXT("ArmConfirmationTimeout"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		if (!AbilitySystemComponent->WasPredictionRejectLabArmSuccessful(
			GASM6AutomationTrialId))
		{
			FailGASM6Automation(TEXT("ServerRefusedToArm"));
			break;
		}
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=ForcedRejectInput TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		ImmunityAbilityPressed();
		ImmunityAbilityReleased();
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	case 2:
	{
		const float Energy = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute())
			: -1.0f;
		const bool bRollbackSettled = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetLastPredictionLabRejectedKey() != 0
			&& FMath::IsNearlyEqual(Energy, 100.0f, 0.1f)
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRollbackSettled)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RollbackTimeout"));
				FailGASM6Automation(TEXT("RollbackDidNotSettle"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("PostRejectCheckpoint"));
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=RecoveryInput TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		ImmunityAbilityPressed();
		ImmunityAbilityReleased();
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	}
	case 3:
	{
		const int16 RejectedKey = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabRejectedKey()
			: 0;
		const int16 CaughtUpKey = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabCaughtUpKey()
			: 0;
		const float Energy = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute())
			: -1.0f;
		const bool bRecoveryAccepted = AbilitySystemComponent != nullptr
			&& CaughtUpKey != 0
			&& CaughtUpKey != RejectedKey
			&& FMath::IsNearlyEqual(Energy, 70.0f, 0.1f)
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 1
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 1
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRecoveryAccepted)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RecoveryTimeout"));
				FailGASM6Automation(TEXT("RecoveryDidNotConverge"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("PostRecoveryCheckpoint"));
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	}
	case 4:
	{
		const bool bRecoveryExpired = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRecoveryExpired)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RecoveryExpiryTimeout"));
				FailGASM6Automation(TEXT("RecoveryDidNotExpire"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("RecoveryExpiryCheckpoint"));
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=SequenceComplete Result=Pass TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		break;
	}
	default:
		break;
	}
#endif
}

void AmultiplayerCharacter::RunNextGASM6IntentAutomationStep()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const int32 ExecutedStep = GASM6IntentAutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_INTENT_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetName());

	auto Submit = [this](
		EmultiplayerDamageIntentTestMutation Mutation,
		EmultiplayerDamageIntentResult ExpectedResult)
	{
		if (AbilitySystemComponent == nullptr || !AimGASM5AutomationAtTarget())
		{
			--GASM6IntentAutomationStep;
			ScheduleNextGASM5AutomationStep(0.25f);
			return false;
		}
		GASM6IntentResultSerialBefore =
			AbilitySystemComponent->GetDamageIntentResultSerial();
		AbilitySystemComponent->SetDamageIntentLabMutation(Mutation);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT_AUTO Phase=Input Mutation=%s Expected=%s ResultSerialBefore=%u Energy=%.1f"),
			GetMultiplayerDamageIntentTestMutationName(Mutation),
			GetMultiplayerDamageIntentResultName(ExpectedResult),
			GASM6IntentResultSerialBefore,
			AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute()));
		DamageAbilityPressed();
		DamageAbilityReleased();
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		return true;
	};

	auto WaitForResult = [this](
		EmultiplayerDamageIntentResult ExpectedResult,
		float ExpectedEnergy,
		const TCHAR* Checkpoint)
	{
		const bool bResultArrived = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetDamageIntentResultSerial()
				> GASM6IntentResultSerialBefore;
		const bool bResultMatches = bResultArrived
			&& AbilitySystemComponent->GetLastDamageIntentResult()
				== ExpectedResult;
		const bool bStateSettled = AbilitySystemComponent != nullptr
			&& FMath::IsNearlyEqual(
				AbilitySystemComponent->GetNumericAttribute(
					UmultiplayerAttributeSet::GetEnergyAttribute()),
				ExpectedEnergy,
				0.1f)
			&& (ExpectedResult == EmultiplayerDamageIntentResult::Accepted
				|| AbilitySystemComponent->GetTagCount(
					MultiplayerGameplayTags::Cooldown_Ability_Damage) == 0);
		if (!bResultMatches || !bStateSettled)
		{
			if (IsGASM6AutomationTimedOut())
			{
				UE_LOG(
					LogMultiplayerGAS,
					Error,
					TEXT("GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Fail Reason=ResultTimeout Expected=%s Actual=%s SerialBefore=%u SerialNow=%u Energy=%.1f Cooldown=%d"),
					GetMultiplayerDamageIntentResultName(ExpectedResult),
					AbilitySystemComponent != nullptr
						? GetMultiplayerDamageIntentResultName(
							AbilitySystemComponent->GetLastDamageIntentResult())
						: TEXT("NoASC"),
					GASM6IntentResultSerialBefore,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetDamageIntentResultSerial()
						: 0,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetNumericAttribute(
							UmultiplayerAttributeSet::GetEnergyAttribute())
						: -1.0f,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetTagCount(
							MultiplayerGameplayTags::Cooldown_Ability_Damage)
						: -1);
				GASM6IntentAutomationStep = 1000;
				return false;
			}
			--GASM6IntentAutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			return false;
		}
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT_AUTO Phase=Checkpoint Name=%s ShotId=%u Result=%s Energy=%.1f Cooldown=%d"),
			Checkpoint,
			AbilitySystemComponent->GetLastDamageIntentResultShotId(),
			GetMultiplayerDamageIntentResultName(
				AbilitySystemComponent->GetLastDamageIntentResult()),
			AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute()),
			AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Damage));
		return true;
	};

	switch (ExecutedStep)
	{
	case 0:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(1.5f);
		break;
	case 1:
		Submit(
			EmultiplayerDamageIntentTestMutation::None,
			EmultiplayerDamageIntentResult::Accepted);
		break;
	case 2:
		if (AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Damage) > 0)
		{
			--GASM6IntentAutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		if (WaitForResult(
			EmultiplayerDamageIntentResult::Accepted,
			90.0f,
			TEXT("ValidAccepted")))
		{
			// The legitimate one-second cooldown has expired. Rejections below are
			// therefore server semantic decisions, not local cooldown gates.
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 3:
		Submit(
			EmultiplayerDamageIntentTestMutation::DuplicateLastShotId,
			EmultiplayerDamageIntentResult::Duplicate);
		break;
	case 4:
		if (WaitForResult(EmultiplayerDamageIntentResult::Duplicate, 90.0f, TEXT("DuplicateRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 5:
		Submit(EmultiplayerDamageIntentTestMutation::ForgedOrigin, EmultiplayerDamageIntentResult::InvalidOrigin);
		break;
	case 6:
		if (WaitForResult(EmultiplayerDamageIntentResult::InvalidOrigin, 90.0f, TEXT("OriginRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 7:
		Submit(EmultiplayerDamageIntentTestMutation::OppositeDirection, EmultiplayerDamageIntentResult::InvalidDirection);
		break;
	case 8:
		if (WaitForResult(EmultiplayerDamageIntentResult::InvalidDirection, 90.0f, TEXT("DirectionRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 9:
		Submit(EmultiplayerDamageIntentTestMutation::TooOld, EmultiplayerDamageIntentResult::InvalidTime);
		break;
	case 10:
		if (WaitForResult(EmultiplayerDamageIntentResult::InvalidTime, 90.0f, TEXT("StaleTimeRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 11:
		Submit(EmultiplayerDamageIntentTestMutation::Future, EmultiplayerDamageIntentResult::InvalidTime);
		break;
	case 12:
		if (WaitForResult(EmultiplayerDamageIntentResult::InvalidTime, 90.0f, TEXT("FutureTimeRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 13:
		Submit(EmultiplayerDamageIntentTestMutation::CleanMiss, EmultiplayerDamageIntentResult::Miss);
		break;
	case 14:
		if (WaitForResult(EmultiplayerDamageIntentResult::Miss, 90.0f, TEXT("MissRejected")))
		{
			ScheduleNextGASM5AutomationStep(0.05f);
		}
		break;
	case 15:
		Submit(
			EmultiplayerDamageIntentTestMutation::None,
			EmultiplayerDamageIntentResult::Accepted);
		break;
	case 16:
		if (WaitForResult(
			EmultiplayerDamageIntentResult::Accepted,
			80.0f,
			TEXT("RecoveryAccepted")))
		{
			UE_LOG(
				LogMultiplayerGAS,
				Display,
				TEXT("GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Pass"));
		}
		break;
	default:
		break;
	}
#endif
}

void AmultiplayerCharacter::FailGASM6Automation(const TCHAR* Reason)
{
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().ClearTimer(GASM5AutomationTimer);
	UE_LOG(
		LogMultiplayerGAS,
		Error,
		TEXT("GAS_M6_AUTO Phase=SequenceComplete Result=Fail TrialId=%u Ability=Immunity Reason=%s"),
		GASM6AutomationTrialId,
		Reason);
#endif
}

bool AmultiplayerCharacter::IsGASM6AutomationTimedOut() const
{
#if !UE_BUILD_SHIPPING
	return GetWorld() == nullptr
		|| GetWorld()->GetTimeSeconds() >= GASM6AutomationDeadlineSeconds;
#else
	return true;
#endif
}

void AmultiplayerCharacter::LogGASM6Snapshot(const TCHAR* Phase) const
{
#if !UE_BUILD_SHIPPING
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (ASC == nullptr)
	{
		return;
	}

	auto CountEffectsWithEffectTag = [ASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return ASC->GetActiveEffectsWithAllTags(Tags).Num();
	};
	auto CountEffectsWithOwningTag = [ASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return ASC->GetActiveEffects(
			FGameplayEffectQuery::MakeQuery_MatchAllOwningTags(Tags)).Num();
	};

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_SNAPSHOT Phase=%s TrialId=%u RejectedKey=%d CaughtUpKey=%d EnergyBase=%.1f EnergyCurrent=%.1f CostGECount=%d CooldownGECount=%d ImmunityCooldown=%d PersistentGECount=%d ImmuneCount=%d PendingGECount=%d PendingCue=%d PendingVisual=%d"),
		Phase,
		GASM6AutomationTrialId,
		static_cast<int32>(AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabRejectedKey()
			: 0),
		static_cast<int32>(AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabCaughtUpKey()
			: 0),
		ASC->GetNumericAttributeBase(UmultiplayerAttributeSet::GetEnergyAttribute()),
		ASC->GetNumericAttribute(UmultiplayerAttributeSet::GetEnergyAttribute()),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Cost_Immunity),
		CountEffectsWithOwningTag(MultiplayerGameplayTags::Cooldown_Ability_Immunity),
		ASC->GetTagCount(MultiplayerGameplayTags::Cooldown_Ability_Immunity),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Positive_Immunity),
		ASC->GetTagCount(MultiplayerGameplayTags::State_Immune),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Debug_PredictionPending),
		ASC->GetTagCount(MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending),
		bGameplayCuePredictionPendingActive ? 1 : 0);
#endif
}

bool AmultiplayerCharacter::AimGASM5AutomationAtTarget()
{
#if !UE_BUILD_SHIPPING
	APlayerController* PlayerController = Cast<APlayerController>(Controller);
	UWorld* World = GetWorld();
	if (PlayerController == nullptr || World == nullptr)
	{
		return false;
	}

	AmultiplayerGASTargetDummy* ClosestTarget = nullptr;
	float ClosestDistanceSquared = TNumericLimits<float>::Max();
	for (TActorIterator<AmultiplayerGASTargetDummy> It(World); It; ++It)
	{
		AmultiplayerGASTargetDummy* Candidate = *It;
		if (!IsValid(Candidate) || Candidate->GetHealth() <= 0.0f)
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared(
			GetActorLocation(),
			Candidate->GetActorLocation());
		if (DistanceSquared < ClosestDistanceSquared)
		{
			ClosestTarget = Candidate;
			ClosestDistanceSquared = DistanceSquared;
		}
	}

	if (ClosestTarget == nullptr)
	{
		UE_LOG(LogMultiplayerGAS, Warning, TEXT("GAS_M5_AUTO Phase=AimFailed Reason=NoLiveTarget"));
		return false;
	}

	FVector ViewLocation;
	FRotator ViewRotation;
	PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	const FVector AimLocation = ClosestTarget->GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);
	const FRotator AimRotation = (AimLocation - ViewLocation).Rotation();
	PlayerController->SetControlRotation(AimRotation);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M5_AUTO Phase=Aimed Target=%s Distance=%.1f Rotation=%s"),
		*ClosestTarget->GetName(),
		FMath::Sqrt(ClosestDistanceSquared),
		*AimRotation.ToCompactString());
	return true;
#else
	return false;
#endif
}

void AmultiplayerCharacter::ClientConfirmServerAction_Implementation(
	int32 ConfirmedCount)
{
	OnServerActionConfirmed.Broadcast(ConfirmedCount);

	UE_LOG(
		LogTemplateCharacter,
		Log,
		TEXT("%s received owner-only confirmation: count=%d"),
		*GetName(),
		ConfirmedCount
	);
}

void AmultiplayerCharacter::MulticastPlayNetworkActionEffect_Implementation(
	FVector_NetQuantize EffectLocation)
{
	OnNetworkActionEffect.Broadcast(EffectLocation);

	UE_LOG(
		LogTemplateCharacter,
		Verbose,
		TEXT("%s received multicast effect at %s"),
		*GetName(),
		*EffectLocation.ToCompactString()
	);
}

void AmultiplayerCharacter::RequestSpawnReplicatedCube()
{
	const FVector SpawnLocation =
		GetActorLocation()
		+ GetActorForwardVector() * 200.0f
		+ FVector(0.0f, 0.0f, 100.0f);

	ServerSpawnReplicatedCube(SpawnLocation);
}

void AmultiplayerCharacter::RequestRestartCoopGame()
{
	ServerRestartCoopGame();
}

bool AmultiplayerCharacter::ServerRestartCoopGame_Validate()
{
	return !IsActorBeingDestroyed();
}

void AmultiplayerCharacter::ServerRestartCoopGame_Implementation()
{
	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld() != nullptr ? GetWorld()->GetAuthGameMode<AmultiplayerGameMode>() : nullptr)
	{
		CoopGameMode->RestartCoopGame();
	}
}

bool AmultiplayerCharacter::ServerSpawnReplicatedCube_Validate(
	FVector_NetQuantize SpawnLocation)
{
	const bool bLocationIsFinite =
		FMath::IsFinite(SpawnLocation.X)
		&& FMath::IsFinite(SpawnLocation.Y)
		&& FMath::IsFinite(SpawnLocation.Z);

	const bool bLocationIsNearCharacter =
		FVector::DistSquared(SpawnLocation, GetActorLocation())
		<= FMath::Square(500.0f);

	return bLocationIsFinite && bLocationIsNearCharacter;
}

void AmultiplayerCharacter::ServerSpawnReplicatedCube_Implementation(
	FVector_NetQuantize SpawnLocation)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = this;
	SpawnParameters.Instigator = this;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	World->SpawnActor<AmultiplayerReplicatedCube>(
		AmultiplayerReplicatedCube::StaticClass(),
		SpawnLocation,
		FRotator::ZeroRotator,
		SpawnParameters
	);
}

void AmultiplayerCharacter::OnRep_NetworkActionCount()
{
	BroadcastNetworkActionCount();
}

void AmultiplayerCharacter::BroadcastNetworkActionCount()
{
	OnNetworkActionCountChanged.Broadcast(NetworkActionCount);

	const FString Message = FString::Printf(
		TEXT("%s received NetworkActionCount=%d"),
		*GetName(),
		NetworkActionCount
	);
	UE_LOG(LogTemplateCharacter, Log, TEXT("%s"), *Message);

	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, Message);
	}
}

void AmultiplayerCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = Controller->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	
		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AmultiplayerCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}
