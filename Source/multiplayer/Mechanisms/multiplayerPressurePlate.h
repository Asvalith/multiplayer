// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerPressurePlate.generated.h"

class ACharacter;
class AmultiplayerCoopGameState;
class AmultiplayerPressurePlate;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;
class USceneComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnPressurePlateActiveChanged,
	AmultiplayerPressurePlate*,
	Plate,
	bool,
	bIsActive);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnPressurePlateOccupancyChanged,
	AmultiplayerPressurePlate*,
	Plate,
	int32,
	PlayerCount);

/**
 * PlayerOccupancy 统计不同角色，服务器结合人数、目标条件和锁存配置决定激活状态。
 * 只复制开关及初始速度，各端播放网格按压过渡，静止时关闭 Tick。
 * 本地网格有碰撞，但激活规则不依赖客户端按压进度。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerPressurePlate : public AActor
{
	GENERATED_BODY()

public:
	// 创建触发器和人数组件，网格只在状态过渡期间 Tick。
	AmultiplayerPressurePlate();

	// 返回本机当前状态：服务器为规则真相，客户端为最近一次收到的复制快照。
	bool IsPlateActive() const { return bPlateActive; }

	// 追加有效不同角色，供门合并多个压力板的玩家集合。
	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	// 各端播放本地按压过渡，到位关闭 Tick。
	virtual void Tick(float DeltaSeconds) override;
	// 同步最终开关与初始速度，不同步每帧网格位置。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 状态变化的本机事件。依赖权威结果的机关只在服务器绑定，客户端可用于非规则表现。
	FOnPressurePlateActiveChanged OnPlateActiveChanged;

	// 服务器占用人数变化事件。人数改变但激活布尔值不变时，门仍需重新统计不同玩家。
	FOnPressurePlateOccupancyChanged OnPlateOccupancyChanged;

protected:
	// 校准初始网格位置，服务器绑定当前占用和可选目标条件。
	virtual void BeginPlay() override;
	// 先解除本板监听，再释放人数记录和 GameState 订阅。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 只扩展材质、音效等表现，不允许蓝图从这里反向修改 bPlateActive 或共享进度。
	UFUNCTION(BlueprintImplementableEvent, Category = "Pressure Plate", meta = (DisplayName = "On Plate Visual State Changed"))
	void ReceivePlateVisualStateChanged(bool bIsActive);

	// 先更新开关再广播人数，使合作门读取到一致的最新事实。
	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	// 玩家已站在板上时，完成钥匙目标也应立即触发重算。
	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	// 接收复制开关或服务器本地提交，统一启动视觉过渡与本机通知。
	UFUNCTION()
	void OnRep_PlateActive();

private:
	// 只由服务器事件入口调用，组合人数、目标与锁存并忽略同值更新。
	void EvaluatePlateState();
	// 从初始相对位置与按压偏移计算网格目标，不影响 Actor 根位置。
	FVector GetMeshTargetLocation() const;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pressure Plate|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> PlateMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<UBoxComponent> ActivationTrigger;

	UPROPERTY(VisibleAnywhere, Category = "Pressure Plate|Components")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Movement")
	FVector PressedOffset = FVector(0.0f, 0.0f, -8.0f);

	// 新运行期字段不加载旧蓝图 PressMoveSpeed；0 为未就绪，确保服务器的正速度必定进入初始复制。
	UPROPERTY(VisibleInstanceOnly, Transient, Replicated, Category = "Pressure Plate|Movement")
	float RuntimePressMoveSpeed = 0.0f;

	// 开启后只统计玩家控制的 Character，排除 AI 或其他角色类型误触发合作机关。
	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequirePlayerControlledCharacter = true;

	// 锁存模式首次满足后永久保持激活；普通模式会在玩家离开时恢复。
	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bLatchOnceActivated = false;

	// 可选的钥匙目标前置条件；GameState 变化也会触发重新判定。
	UPROPERTY(EditAnywhere, Category = "Pressure Plate|Rules")
	bool bRequireObjectiveComplete = false;

	UPROPERTY(ReplicatedUsing = OnRep_PlateActive)
	bool bPlateActive = false;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	// BeginPlay 保存的初始相对位置，避免把编辑器摆放结果硬编码成坐标。
	FVector ReleasedRelativeLocation = FVector::ZeroVector;
};
