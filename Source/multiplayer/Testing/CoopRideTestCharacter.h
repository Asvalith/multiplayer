#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Player/multiplayerCharacter.h"
#include "CoopRideTestCharacter.generated.h"

/** 一次已接受的客户端校正；时间是客户端本地 World 时间，不与服务端时钟混用。 */
struct FCoopRideCorrectionEvent
{
	float ReceiptWorldSeconds = 0;
	float MoveTimeStamp = 0;
	float ErrorCm = 0;
	FVector Error = FVector::ZeroVector;
	FVector ClientVelocity = FVector::ZeroVector;
	FVector ServerVelocity = FVector::ZeroVector;
	bool bComparable = false;
	bool bSavedOnBase = false;
	bool bServerOnBase = false;
	FString SavedBase;
	FString ServerBase;
};

/** 客户端完成预测时保存的 Move 快照；与服务器记录通过 MoveTimeStamp 配对。 */
struct FCoopRideClientMoveEvent
{
	float MoveTimeStamp = 0;
	float ClientWorldSeconds = 0;
	FVector Start = FVector::ZeroVector, End = FVector::ZeroVector, Relative = FVector::ZeroVector;
	FVector StartVelocity = FVector::ZeroVector, EndVelocity = FVector::ZeroVector;
	FVector Platform = FVector::ZeroVector, PlatformVelocity = FVector::ZeroVector;
	float DeltaSeconds = 0;
	uint8 StartMovementMode = 0, EndMovementMode = 0;
	bool bStartBased = false, bEndBased = false, bJumpPressed = false;
	FString StartBase, EndBase;
};

/** 服务端完成同一条 Move 的模拟、尚未检查误差前的权威快照。 */
struct FCoopRideServerMoveEvent
{
	float MoveTimeStamp = 0;
	float ServerWorldSeconds = 0;
	FVector Server = FVector::ZeroVector, ServerRelative = FVector::ZeroVector, ServerVelocity = FVector::ZeroVector;
	FVector Reported = FVector::ZeroVector, Platform = FVector::ZeroVector, PlatformVelocity = FVector::ZeroVector;
	float DeltaSeconds = 0;
	bool bServerBased = false, bReportedBased = false, bServerFalling = false, bReportedFalling = false;
	FString ServerBase, ReportedBase;
};

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
	TArray<FCoopRideCorrectionEvent> CorrectionEvents;
	int32 DroppedCorrectionEvents = 0;
	TArray<FCoopRideClientMoveEvent> ClientMoveEvents;
	TArray<FCoopRideServerMoveEvent> ServerMoveEvents;
	int32 DroppedClientMoves = 0, DroppedServerMoves = 0;
	TWeakObjectPtr<AActor> ObservedPlatform;

protected:
	virtual void ReplicateMoveToServer(float DeltaTime, const FVector& NewAcceleration) override;
	virtual void ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime, const FVector& Accel,
		const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase,
		FName ClientBaseBoneName, uint8 ClientMovementMode) override;
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
