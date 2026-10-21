#include "Testing/CoopRideTestCharacter.h"
#include "Engine/World.h"

// 载人实验观测层：在原生 CMC 钩子旁记录历史 Move/校正，不改误差容差或重演行为。
// 只由显式测试角色使用，数组容量有上限，不能将本地观察指标当作普通玩家功能。

// 新采样阶段清除观测统计与历史记录，不重置 CMC 本身的预测/校正状态。
void UCoopRideTestMovement::ResetMetrics()
{
	CorrectionCount = 0;
	ComparableCorrectionCount = 0;
	BaseChangeCorrectionCount = 0;
	MaxCorrectionCm = 0.0f;
	CorrectionEvents.Reset();
	DroppedCorrectionEvents = 0;
	ClientMoveEvents.Reset();
	ServerMoveEvents.Reset();
	DroppedClientMoves = DroppedServerMoves = 0;
}

// 保留客户端原生预测和发送后记录最新 SavedMove，以同一 Move 时间戳供两端对照。
void UCoopRideTestMovement::ReplicateMoveToServer(float DeltaTime, const FVector& NewAcceleration)
{
	Super::ReplicateMoveToServer(DeltaTime, NewAcceleration);
#if !UE_BUILD_SHIPPING
	if (!ObservedPlatform.IsValid()) return;
	const FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (!ClientData || ClientData->SavedMoves.IsEmpty()) return;
	const FSavedMovePtr& Move = ClientData->SavedMoves.Last();
	if (!Move.IsValid() || (ClientMoveEvents.Num() && ClientMoveEvents.Last().MoveTimeStamp == Move->TimeStamp)) return;
	FCoopRideClientMoveEvent Event;
	Event.MoveTimeStamp = Move->TimeStamp;
	Event.ClientWorldSeconds = GetWorld()->GetTimeSeconds();
	Event.Start = Move->StartLocation;
	Event.End = Move->SavedLocation;
	Event.Relative = Move->SavedRelativeLocation;
	Event.StartVelocity = Move->StartVelocity;
	Event.EndVelocity = Move->SavedVelocity;
	Event.Platform = ObservedPlatform->GetActorLocation();
	Event.PlatformVelocity = ObservedPlatform->GetVelocity();
	Event.DeltaSeconds = Move->DeltaTime;
	Event.StartMovementMode = Move->StartPackedMovementMode;
	Event.EndMovementMode = Move->EndPackedMovementMode;
	Event.bStartBased = Move->StartBase.IsValid();
	Event.bEndBased = Move->EndBase.IsValid();
	Event.bJumpPressed = Move->bPressedJump;
	Event.StartBase = GetNameSafe(Move->StartBase.Get());
	Event.EndBase = GetNameSafe(Move->EndBase.Get());
	if (ClientMoveEvents.Num() < 512) ClientMoveEvents.Add(MoveTemp(Event));
	else ++DroppedClientMoves;
#endif
}

// 在原生误差检查前保存服务器完成模拟的位置/基座，随后原样交给 Super 决定是否校正。
void UCoopRideTestMovement::ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime,
	const FVector& Accel, const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase,
	FName ClientBaseBoneName, uint8 ClientMovementMode)
{
#if !UE_BUILD_SHIPPING
	if (ObservedPlatform.IsValid() && CharacterOwner)
	{
		FCoopRideServerMoveEvent Event;
		Event.MoveTimeStamp = ClientTimeStamp;
		Event.ServerWorldSeconds = GetWorld()->GetTimeSeconds();
		Event.Server = CharacterOwner->GetActorLocation();
		Event.ServerVelocity = Velocity;
		Event.Platform = ObservedPlatform->GetActorLocation();
		Event.PlatformVelocity = ObservedPlatform->GetVelocity();
		Event.ServerRelative = Event.Server - Event.Platform;
		Event.Reported = RelativeClientLocation;
		Event.DeltaSeconds = DeltaTime;
		Event.bServerBased = GetMovementBase() != nullptr;
		Event.bReportedBased = ClientMovementBase != nullptr;
		Event.bServerFalling = IsFalling();
		TEnumAsByte<EMovementMode> ReportedMode, GroundMode;
		uint8 CustomMode;
		UnpackNetworkMovementMode(ClientMovementMode, ReportedMode, CustomMode, GroundMode);
		Event.bReportedFalling = ReportedMode == MOVE_Falling;
		Event.ServerBase = GetNameSafe(GetMovementBase());
		Event.ReportedBase = GetNameSafe(ClientMovementBase);
		if (ServerMoveEvents.Num() < 512) ServerMoveEvents.Add(MoveTemp(Event));
		else ++DroppedServerMoves;
	}
#endif
	// 保持原来的误差判断、容差和校正发送路径不变。
	Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel, RelativeClientLocation,
		ClientMovementBase, ClientBaseBoneName, ClientMovementMode);
}

// 观察已被 CMC 接受的校正，只比较同一历史 Move 的可比坐标系；基座切换单独计数。
void UCoopRideTestMovement::OnClientCorrectionReceived(
	FNetworkPredictionData_Client_Character& ClientData,
	float TimeStamp,
	FVector NewLocation,
	FVector NewVelocity,
	UPrimitiveComponent* NewBase,
	FName NewBaseBoneName,
	bool bHasBase,
	bool bBaseRelativePosition,
	uint8 ServerMovementMode,
	FVector ServerGravityDirection)
{
#if !UE_BUILD_SHIPPING
	++CorrectionCount;
	FCoopRideCorrectionEvent Event;
	Event.ReceiptWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
	Event.MoveTimeStamp = TimeStamp;
	Event.ServerVelocity = NewVelocity;
	Event.bServerOnBase = bHasBase && NewBase != nullptr;
	Event.ServerBase = GetNameSafe(NewBase);
	// CMC 在调用本事件前已确认对应时间戳的 Move；不能用当前角色位置减历史校正位置。
	const FSavedMovePtr& SavedMove = ClientData.LastAckedMove;
	if (SavedMove.IsValid() && FMath::IsNearlyEqual(SavedMove->TimeStamp, TimeStamp, .001f))
	{
		Event.ClientVelocity = SavedMove->SavedVelocity;
		Event.bSavedOnBase = SavedMove->EndBase.IsValid();
		Event.SavedBase = GetNameSafe(SavedMove->EndBase.Get());
		FVector ClientLocation;
		FVector ServerLocation;
		bool bComparable = false;
		if (bHasBase && bBaseRelativePosition && NewBase != nullptr
			&& SavedMove->EndBase.Get() == NewBase && SavedMove->EndBoneName == NewBaseBoneName)
		{
			// NewLocation 已被引擎用当前基座变换还原成世界位置。先变回基座局部坐标，
			// 再与同一 Move 保存的局部位置比较，剔除等待网络响应期间平台自身的位移。
			ClientLocation = SavedMove->SavedRelativeLocation;
			bComparable = MovementBaseUtility::TransformLocationToLocal(
				NewBase, NewBaseBoneName, NewLocation, ServerLocation);
		}
		else if (!bBaseRelativePosition
			&& !MovementBaseUtility::UseRelativeLocation(SavedMove->EndBase.Get())
			&& !MovementBaseUtility::UseRelativeLocation(NewBase))
		{
			// 双方都不依赖移动基座时，可以直接比较该 Move 的世界坐标。
			ClientLocation = SavedMove->SavedLocation;
			ServerLocation = NewLocation;
			bComparable = true;
		}

		if (bComparable)
		{
			const FVector Difference = ServerLocation - ClientLocation;
			Event.bComparable = true;
			Event.ErrorCm = Difference.Size();
			Event.Error = Difference;
			++ComparableCorrectionCount;
			MaxCorrectionCm = FMath::Max(MaxCorrectionCm, Event.ErrorCm);
		}
		else
		{
			// 起跳/落地等基座切换单独计数；没有历史基座变换时，不伪造世界空间误差。
			++BaseChangeCorrectionCount;
		}
	}
	// 只在显式 RideMotion 测试角色上收集，避免逐帧日志和无限增长。
	if (CorrectionEvents.Num() < 128) CorrectionEvents.Add(MoveTemp(Event));
	else ++DroppedCorrectionEvents;
#endif

	// 保留引擎日志/调试行为；真正的位置校正和后续 Move 重演仍完全由 CMC 执行。
	Super::OnClientCorrectionReceived(ClientData, TimeStamp, NewLocation, NewVelocity,
		NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode, ServerGravityDirection);
}

// 仅替换测试 Pawn 的默认移动子对象以安装观测钩子，正式角色默认 CMC 不受影响。
ACoopRideTestCharacter::ACoopRideTestCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UCoopRideTestMovement>(ACharacter::CharacterMovementComponentName))
{
}
