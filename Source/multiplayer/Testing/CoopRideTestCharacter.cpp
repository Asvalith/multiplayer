#include "Testing/CoopRideTestCharacter.h"

void UCoopRideTestMovement::ResetMetrics()
{
	CorrectionCount = 0;
	ComparableCorrectionCount = 0;
	BaseChangeCorrectionCount = 0;
	MaxCorrectionCm = 0.0f;
}

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
	// CMC 在调用本事件前已确认对应时间戳的 Move；不能用当前角色位置减历史校正位置。
	const FSavedMovePtr& SavedMove = ClientData.LastAckedMove;
	if (SavedMove.IsValid())
	{
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
			++ComparableCorrectionCount;
			MaxCorrectionCm = FMath::Max(MaxCorrectionCm, FVector::Distance(ClientLocation, ServerLocation));
		}
		else
		{
			// 起跳/落地等基座切换单独计数；没有历史基座变换时，不伪造世界空间误差。
			++BaseChangeCorrectionCount;
		}
	}
#endif

	// 保留引擎日志/调试行为；真正的位置校正和后续 Move 重演仍完全由 CMC 执行。
	Super::OnClientCorrectionReceived(ClientData, TimeStamp, NewLocation, NewVelocity,
		NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode, ServerGravityDirection);
}

ACoopRideTestCharacter::ACoopRideTestCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UCoopRideTestMovement>(ACharacter::CharacterMovementComponentName))
{
}
