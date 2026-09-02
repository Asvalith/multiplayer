// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerMovingPlatform.generated.h"

class AmultiplayerPressurePlate;
class UArrowComponent;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;
class UmultiplayerTransporterComponent;
class USceneComponent;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EMovingPlatformActivationSource : uint8
{
	// 监听关卡中指定压力板的权威激活状态。
	ExternalPressurePlate,
	// 使用平台自身触发区中的不同玩家数量。
	PlatformOccupancy
};

/**
 * 由服务器驱动并复制位置的移动平台。
 *
 * PlayerOccupancy 负责区域人数，Transporter 负责移动，本 Actor 只选择激活来源。
 * 这样把通用能力拆开后，人数检测和移动逻辑都能被其他机关复用。
 *
 * (*) 平台会承载玩家，连续位置必须以服务器为准，因此选择 ReplicateMovement；
 * 门和压力板只是视觉过渡，所以只复制离散状态并在各端播放。
 * (*) Transporter 只在 Authority 上改变 Actor 位置，客户端不自行插值同一条业务运动曲线，
 * 避免平台与其承载角色在不同机器上产生两套碰撞结果。
 * (**) StartPoint/TargetPoint 是平台子组件，BeginPlay 必须先缓存世界坐标；如果移动过程中继续
 * 读取子组件位置，目标会跟随平台一起移动，平台将永远无法到达终点。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerMovingPlatform : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerMovingPlatform();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleActivationPlateChanged(
		AmultiplayerPressurePlate* Plate,
		bool bIsActive);

private:
	// 根据配置的激活来源计算一次目标状态，再交给 Transporter 开始或结束运动。
	void RefreshActivation();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<USceneComponent> PlatformRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UStaticMeshComponent> PlatformMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UBoxComponent> ActivationVolume;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Platform")
	TObjectPtr<UmultiplayerTransporterComponent> Transporter;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> StartPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Platform", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> TargetPoint;

	// 两种来源互斥，EditCondition 只显示当前模式真正使用的参数。
	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation")
	EMovingPlatformActivationSource ActivationSource =
		EMovingPlatformActivationSource::ExternalPressurePlate;

	// 外部压力板模式下的关卡实例引用；为空时平台保持非激活。
	UPROPERTY(EditInstanceOnly, Category = "Coop|Platform|Activation", meta = (EditCondition = "ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate", EditConditionHides))
	TObjectPtr<AmultiplayerPressurePlate> ActivationPlate;

	// 自身占用模式要求的不同玩家数，而不是碰撞组件数量。
	UPROPERTY(EditAnywhere, Category = "Coop|Platform|Activation", meta = (ClampMin = "1", EditCondition = "ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy", EditConditionHides))
	int32 RequiredPlayers = 1;

};
