// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCharacter.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "Developer/multiplayerGASDeveloperHarnessComponent.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "Input/multiplayerInputConfig.h"
#include "multiplayer.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "multiplayerGameMode.h"
#include "Player/multiplayerGASPlayerState.h"
#include "UI/multiplayerGASCuePresenterComponent.h"
#include "UI/multiplayerGASHUDPresenterComponent.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

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
	GASCuePresenter = CreateDefaultSubobject<UmultiplayerGASCuePresenterComponent>(
		TEXT("GASCuePresenter"));
	GASDeveloperHarness = CreateDefaultSubobject<UmultiplayerGASDeveloperHarnessComponent>(
		TEXT("GASDeveloperHarness"));

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
	GASCuePresenter->BindLights(GameplayCueFlashLight, GameplayCueStateLight);

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

void AmultiplayerCharacter::GameplayCueDefaultHandler(
	EGameplayCueEvent::Type EventType,
	const FGameplayCueParameters& Parameters)
{
	if (GASCuePresenter == nullptr
		|| !GASCuePresenter->HandleGameplayCue(EventType, Parameters))
	{
		IGameplayCueInterface::GameplayCueDefaultHandler(EventType, Parameters);
	}
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

		if (GASDeveloperHarness != nullptr)
		{
			GASDeveloperHarness->BindDeveloperInput(PlayerInputComponent);
		}
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AmultiplayerCharacter::ApplyDeathState(bool bNewDeadState)
{
	if (GASCuePresenter != nullptr)
	{
		GASCuePresenter->ApplyDeathState(bNewDeadState);
	}

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

	// Bind presentation before publishing initialization so synchronous ability
	// activation from listeners cannot outrun prediction reconciliation.
	GASCuePresenter->BindAbilitySystem(AbilitySystemComponent);

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
	VictoryPresenter->RefreshBinding();
	ApplyDeathState(GASPlayerState->IsDead());
	if (GASDeveloperHarness != nullptr)
	{
		GASDeveloperHarness->OnAbilitySystemReady(AbilitySystemComponent);
	}
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
