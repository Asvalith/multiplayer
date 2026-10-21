// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Core/multiplayerLog.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatPresentation.h"

// 第三人称输入与镜头适配层：本地输入进入父类 GAS 或 CharacterMovement，远端只消费复制。
// 角色不结算机关与胜负；观察方向、移动朝向和具体施法朝向各按对应系统规则处理。

/*
 * 构造阶段只搭建角色的固定组件和默认移动参数，不读取 World，也不执行任何联网规则。
 * 这些子对象会同时存在于服务器和客户端；真正属于本地玩家的输入映射要等控制器确定后再安装。
 */
AmultiplayerCharacter::AmultiplayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	//胶囊体负责角色移动碰撞，尺寸与默认第三人称模型匹配。
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	//身体不直接跟随鼠标旋转
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	//身体朝向由移动组件控制，移动时自动旋转到输入方向。
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// 手感参数保留在构造默认值中，也可以在角色蓝图里覆盖，调试时无需反复编译 C++。
	Movement->JumpZVelocity = 700.f;
	Movement->AirControl = 0.35f;
	Movement->MaxWalkSpeed = 500.f;
	Movement->MinAnalogWalkSpeed = 20.f;
	Movement->BrakingDecelerationWalking = 2000.f;
	// (**) 平台起跳会继承基座水平速度；空中无输入时强制刹车会很快抵消这份惯性，
	// 即使网络完全正常也会落在平台后方。保留惯性，方向调整仍由 AirControl 处理。
	Movement->BrakingDecelerationFalling = 0.0f;
	GetCoopComponent()->bEnableDowned = true;

	//弹簧臂会在遮挡时自动收缩，比直接把摄像机挂到角色上更适合第三人称视角。
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	//让弹簧臂随控制器旋转，摄像机位置会跟随旋转，但不直接叠加到角色本身。
	CameraBoom->bUsePawnControlRotation = true;

	// 摄像机跟随弹簧臂末端，本身不再叠加控制器旋转。
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// 网格体和动画蓝图由派生角色蓝图配置，避免 C++ 直接依赖可替换的美术资源。
}

/*
 * 控制器变更时，为“拥有 LocalPlayer 的本地控制器”安装输入映射。
 * 服务器上的远端角色和客户端看到的其他玩家也会有 Character 实例，但没有本地输入资格。
 * (*) 本地控制权与 Actor 是否存在是两回事，联机输入必须按 LocalPlayer 判断。
 */
void AmultiplayerCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// (*) 输入映射属于本地玩家配置，因此添加到 LocalPlayer 子系统，而不是放到服务器逻辑中。
	// (**) 服务器或非本地角色没有 LocalPlayer，必须逐层判空。
	if (DefaultMappingContext == nullptr)
	{
		return;
	}

	APlayerController* PlayerController = Cast<APlayerController>(Controller);
	if (PlayerController == nullptr)
	{
		return;
	}

	ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (LocalPlayer == nullptr)
	{
		return;
	}

	//从本地玩家子系统中获取 Enhanced Input 子系统，并添加默认输入映射上下文。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer))
	{
		Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}
}

/*
 * 将 Enhanced Input 的动作绑定到角色行为。调用前提是 Pawn 已被本地控制器接管；
 * 移动/视角使用 Enhanced Input，技能按键转入父类 GAS 入口；位置预测与校正由 CMC 负责。
 */
void AmultiplayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// 原有移动 Action 保持不变；基础战斗按键无需新增输入资源，也可由蓝图调用相同接口。
	PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AmultiplayerCharacter::Attack);
	PlayerInputComponent->BindKey(EKeys::Q, IE_Pressed, this, &AmultiplayerCharacter::PreviewDefense);
	PlayerInputComponent->BindKey(EKeys::Q, IE_Released, this, &AmultiplayerCharacter::ReleaseDefense);
	PlayerInputComponent->BindKey(EKeys::E, IE_Pressed, this, &AmultiplayerCharacter::PreviewHeal);
	PlayerInputComponent->BindKey(EKeys::E, IE_Released, this, &AmultiplayerCharacter::ReleaseHeal);
	PlayerInputComponent->BindKey(EKeys::R, IE_Pressed, this, &AmultiplayerCharacter::PreviewArea);
	PlayerInputComponent->BindKey(EKeys::R, IE_Released, this, &AmultiplayerCharacter::ReleaseArea);
	PlayerInputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AmultiplayerCharacter::CancelCast);
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Pressed, this, &AmultiplayerCharacter::PreviewDash);
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Released, this, &AmultiplayerCharacter::ReleaseDash);
	PlayerInputComponent->BindKey(EKeys::F, IE_Pressed, this, &AmultiplayerCharacter::Rescue);
	PlayerInputComponent->BindKey(EKeys::F, IE_Released, this, &AmultiplayerCharacter::CancelRescue);
	PlayerInputComponent->BindKey(EKeys::B, IE_Pressed, this, &AmultiplayerCharacter::ToggleProgressionMenu);
	PlayerInputComponent->BindKey(EKeys::F5, IE_Pressed, this, &AmultiplayerCharacter::SaveProgress);
	PlayerInputComponent->BindKey(EKeys::F9, IE_Pressed, this, &AmultiplayerCharacter::LoadProgress);

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

/*
 * 把二维移动输入转换到控制器朝向的水平坐标系，再交给 ACharacter 累积输入。
 * 这里不直接改 Transform，否则会绕开 CharacterMovement 的碰撞、预测和服务器校正。
 */
void AmultiplayerCharacter::Move(const FInputActionValue& Value)
{
	// 二维输入分别表示左右和前后移动。
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr && IsCombatReady() && !IsMovementBlocked() && !IsCasting())
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

/*
 * 只修改本地控制器的观察输入；SpringArm 随控制器旋转，角色朝向仍由移动组件决定。
 */
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

// 成长面板是独立附加层；原 HUD、准星与蓝图事件图不重建。
void AmultiplayerCharacter::ToggleProgressionMenu()
{
	CancelSkillTargeting();
	if (ProgressionMenu && ProgressionMenu->IsInViewport()) { ProgressionMenu->RemoveFromParent(); return; }
	if (auto* PC = Cast<APlayerController>(GetController()); PC && PC->IsLocalController())
	{
		ProgressionMenu = CreateWidget<UmultiplayerCombatProgressionWidget>(PC, UmultiplayerCombatProgressionWidget::StaticClass());
		if (ProgressionMenu) ProgressionMenu->AddToViewport(20);
	}
}

// 松键只确认自己开始的预览，切换到另一技能或右键取消后不补发旧输入。
void AmultiplayerCharacter::ReleaseDefense()
{
	ReleaseTargetSlot(1);
}
// E 松键仅确认治疗槽位仍在进行的本地预览，不直接应用治疗。
void AmultiplayerCharacter::ReleaseHeal()
{
	ReleaseTargetSlot(2);
}
// R 松键确认当前范围槽位预览，范围锁定与服务器校验仍由父类负责。
void AmultiplayerCharacter::ReleaseArea()
{
	ReleaseTargetSlot(3);
}
// Shift 松键确认当前冲刺槽位预览，位移仍走 GAS 任务与 CMC。
void AmultiplayerCharacter::ReleaseDash()
{
	ReleaseTargetSlot(4);
}
// 松键只属于当前槽位；切换预览或右键取消之后，不允许旧松键确认新技能。
void AmultiplayerCharacter::ReleaseTargetSlot(int32 Slot)
{
	if (IsTargetingSkill() && GetTargetingSkill() == GetProgressionComponent()->GetSlotSkill(Slot)) ConfirmSkillTargeting();
}

// 两端都从同一春臂配置重建本次镜头原点，不接受客户端上传任意射线起点。
FVector AmultiplayerCharacter::GetCombatViewOrigin(const FRotator& Aim) const
{
	const FVector Pivot = CameraBoom->GetComponentLocation() + CameraBoom->TargetOffset;
	const FVector Desired = Pivot - Aim.Vector() * CameraBoom->TargetArmLength + Aim.RotateVector(CameraBoom->SocketOffset);
	FHitResult Hit;
	if (CameraBoom->bDoCollisionTest && GetWorld()->SweepSingleByChannel(Hit, Pivot, Desired, FQuat::Identity,
		CameraBoom->ProbeChannel, FCollisionShape::MakeSphere(CameraBoom->ProbeSize),
		FCollisionQueryParams(SCENE_QUERY_STAT(CombatCameraOrigin), false, this))) return Hit.Location;
	return Desired;
}
