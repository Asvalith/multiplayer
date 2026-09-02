// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "multiplayerCharacter.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UmultiplayerCoopCarryComponent;
class USpringArmComponent;
struct FInputActionValue;

/**
 * 第三人称联机角色，负责本地输入、摄像机以及携带钥匙的能力。
 *
 * 本类只把本地 Enhanced Input 转换为 AddMovementInput / 控制器旋转，不自行发送位置 RPC。
 * ACharacter 内置的 CharacterMovement 会让 AutonomousProxy 做移动预测、把输入结果提交给服务器，
 * 再由服务器校正并向其他 SimulatedProxy 同步。合作玩法规则则留在服务器机关 Actor 中，
 * 避免角色类同时承担移动、目标进度和机关判定。
 *
 * (*) 继承 ACharacter 是因为 CharacterMovement 已提供客户端移动预测和服务器校正，
 * 无需再手写角色位置同步。
 * (**) “本机存在这个 Character”不等于“本地拥有它”。输入映射只应安装到持有 LocalPlayer 的
 * 本地控制器；服务器上的远端 Pawn 和客户端看到的其他玩家都不能绑定本地输入。
 */
UCLASS(config = Game)
class AmultiplayerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AmultiplayerCharacter();

	// 摄像机组件只负责本地观察，不参与服务器玩法判定和网络同步。
	FORCEINLINE USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	// 供服务器钥匙/插槽逻辑快速访问该角色的单钥匙携带槽。
	UmultiplayerCoopCarryComponent* GetCarryComponent() const { return CarryComponent; }

protected:
	// 控制器发生变化时重新判断本地所有权，并为本地玩家安装输入映射上下文。
	virtual void NotifyControllerChanged() override;
	// 将输入 Action 绑定到移动、观察和 ACharacter 自带跳跃接口。
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	// 将二维输入投影到仅含控制器 Yaw 的水平前/右方向，避免镜头俯仰影响地面移动。
	void Move(const FInputActionValue& Value);
	// 修改 Controller 的 Yaw/Pitch；镜头通过 SpringArm 使用控制器旋转。
	void Look(const FInputActionValue& Value);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Carry", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerCoopCarryComponent> CarryComponent;

	// 输入映射和动作保留为可配置资源，便于在蓝图中替换按键方案而不改 C++。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> LookAction;
};

