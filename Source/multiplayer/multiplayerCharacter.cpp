// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCharacter.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "multiplayerReplicatedCube.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "multiplayerGameMode.h"
#include "Net/UnrealNetwork.h"
#include "Player/multiplayerGASPlayerState.h"

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
	InitializeAbilitySystem();

	// Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
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
	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(
			-1,
			5.0f,
			HasAuthority() ? FColor::Green : FColor::Cyan,
			Message
		);
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

void AmultiplayerCharacter::AbilityInputTagPressed(const FGameplayTag& InputTag)
{
	if (UmultiplayerAbilitySystemComponent* ASC = GetMultiplayerAbilitySystemComponent())
	{
		ASC->AbilityInputTagPressed(InputTag);
	}
}

void AmultiplayerCharacter::AbilityInputTagReleased(const FGameplayTag& InputTag)
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

	GASPlayerState->InitializeAbilityActorInfo(this);
	AbilitySystemComponent = GASPlayerState->GetMultiplayerAbilitySystemComponent();
	AttributeSet = GASPlayerState->GetAttributeSet();

	if (HasAuthority())
	{
		GASPlayerState->GrantStartupAbilities(StartupAbilitySet);
	}

	OnAbilitySystemInitialized.Broadcast();
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
