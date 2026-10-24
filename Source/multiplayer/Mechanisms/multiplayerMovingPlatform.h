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
 * 门和压力板只需同步开关结果，允许各端根据结果播放本地过渡。
 * (*) Transporter 只在 Authority 上改变 Actor 位置，客户端不自行插值同一条业务运动曲线，
 * 避免本地计算的路径与收到的服务器位置互相覆盖。客户端碰撞仍依赖本地收到的位置快照，
 * 普通 Actor 位置复制不等于角色移动预测，本项目没有为平台额外实现预测回滚或快照平滑。
 * (**) StartPoint/TargetPoint 是平台子组件，BeginPlay 必须先缓存世界坐标；如果移动过程中继续
 * 读取子组件位置，目标会跟随平台一起移动，平台将永远无法到达终点。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerMovingPlatform : public AActor
{
	GENERATED_BODY()

public:
	// 装配可移动根、平台网格、触发器与两类可复用组件，不启用 Actor Tick。
	AmultiplayerMovingPlatform();

protected:
	// 服务器缓存固定世界端点并绑定唯一激活来源；客户端只关闭多余触发碰撞。
	virtual void BeginPlay() override;
	// 无论当前激活来源如何都对称解绑，支持销毁与关卡卸载。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 非物理 Scene 根不会由 AActor 默认接收函数写速度；补上供 CMC 离开平台时继承。
	virtual void PostNetReceiveVelocity(const FVector& NewVelocity) override;

	// 平台占用模式人数变化后重新计算当前激活条件。
	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	// 外部压力板变化只触发统一重算，不在回调中重复实现移动。
	UFUNCTION()
	void HandleActivationPlateChanged(
		AmultiplayerPressurePlate* Plate,
		bool bIsActive);

	// 指定压力板销毁后清引用并取消激活，是否返回由 Transporter 决定。
	UFUNCTION()
	void HandleActivationPlateDestroyed(AActor* DestroyedActor);

private:
	// 根据配置的激活来源计算一次目标状态，再交给 Transporter 开始或结束运动。
	void RefreshActivation();

	// 固定轨道平台不依赖根组件 Sweep；Scene 根只保存稳定的 Actor 坐标系。
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

	// 自身占用模式要求的不同玩家数，从 JSON 取得；外部压力板模式不使用这个人数。
	UPROPERTY(VisibleInstanceOnly, Category = "Coop|Platform|Activation")
	int32 RequiredPlayers = 1;

};
