#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Player/multiplayerCharacter.h"
#include "CoopRideTestCharacter.generated.h"

/**
 * 载人测试专用的移动组件：只观察服务器校正，不改变校正、预测或移动容差。
 * 校正次数包含位置误差为零的状态校正，因此次数不直接等于可见拉回次数。
 */
UCLASS(Transient, NotBlueprintable)
class UCoopRideTestMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	void ResetMetrics();

	int32 CorrectionCount = 0;
	// 只有同一参考系下可比较的校正才参与距离统计，避免基座切换污染最大值。
	int32 ComparableCorrectionCount = 0;
	int32 BaseChangeCorrectionCount = 0;
	float MaxCorrectionCm = 0.0f;

protected:
	virtual void OnClientCorrectionReceived(
		FNetworkPredictionData_Client_Character& ClientData,
		float TimeStamp,
		FVector NewLocation,
		FVector NewVelocity,
		UPrimitiveComponent* NewBase,
		FName NewBaseBoneName,
		bool bHasBase,
		bool bBaseRelativePosition,
		uint8 ServerMovementMode,
		FVector ServerGravityDirection) override;
};

/**
 * 仅由自动化载人场景生成并接管的测试角色；游戏默认 Pawn 仍使用原 Character/CMC。
 * 保留原角色移动参数，通过默认子对象替换安装观测组件，输入由测试驱动提供。
 */
UCLASS(Transient, NotBlueprintable)
class ACoopRideTestCharacter : public AmultiplayerCharacter
{
	GENERATED_BODY()

public:
	ACoopRideTestCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	// 自动化逐帧注入移动输入，不依赖蓝图的 InputAction 资源，也不绑定玩家键盘。
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override {}
};
