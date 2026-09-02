// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "multiplayerCoopCarryComponent.h"
#include "multiplayerLog.h"

AmultiplayerCharacter::AmultiplayerCharacter()
{
	// 胶囊体负责角色移动碰撞，尺寸与默认第三人称模型匹配。
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// 控制器旋转只驱动镜头，角色朝向交给移动方向，避免移动时被镜头强行带转。
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// (*) 使用 CharacterMovement 的移动预测与服务器校正，不手动复制角色位置。
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// 手感参数保留在构造默认值中，也可以在角色蓝图里覆盖，调试时无需反复编译 C++。
	GetCharacterMovement()->JumpZVelocity = 700.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// 弹簧臂会在遮挡时自动收缩，比直接把摄像机挂到角色上更适合第三人称视角。
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	// 摄像机跟随弹簧臂末端，本身不再叠加控制器旋转。
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	CarryComponent = CreateDefaultSubobject<UmultiplayerCoopCarryComponent>(
		TEXT("CarryComponent"));

	// 网格体和动画蓝图由派生角色蓝图配置，避免 C++ 直接依赖可替换的美术资源。
}

void AmultiplayerCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// (*) 输入映射属于本地玩家配置，因此添加到 LocalPlayer 子系统，而不是放到服务器逻辑中。
	// (**) 服务器或非本地角色没有 LocalPlayer，必须逐层判空。
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
	// Enhanced Input 把“动作”与具体按键解耦，同一套角色代码可复用不同输入方案。
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AmultiplayerCharacter::Move);

		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AmultiplayerCharacter::Look);

	}
	else
	{
		UE_LOG(
			LogMultiplayer,
			Error,
			TEXT("Character '%s' requires an Enhanced Input component."),
			*GetNameSafe(this));
	}
}

void AmultiplayerCharacter::Move(const FInputActionValue& Value)
{
	// 二维输入分别表示左右和前后移动。
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// 只取控制器的 Yaw，避免镜头俯仰导致角色产生向上或向下的移动分量。
		const FRotator Rotation = Controller->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AmultiplayerCharacter::Look(const FInputActionValue& Value)
{
	// 二维输入分别控制水平和垂直观察。
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}
