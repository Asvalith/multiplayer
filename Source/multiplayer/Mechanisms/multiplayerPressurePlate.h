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
 * 由服务器判定激活状态的压力板。
 *
 * 完整链路为：PlayerOccupancy 在服务器统计不同角色 -> EvaluatePlateState 组合人数与目标条件 ->
 * 服务器写 bPlateActive -> RepNotify 让各客户端根据同一个离散状态播放压下/弹起表现。
 * 人数统计交给 PlayerOccupancy，本 Actor 只负责激活规则、状态复制和压下表现。
 * (*) 运行中仅复制 bPlateActive，速度在初始复制时另行提供；客户端插值网格，减少持续同步位置的开销。
 * 这种策略适合只需同步开关结果、过渡时序要求较低的机关；本地网格仍有碰撞，
 * 但服务器不根据客户端的压下进度判定激活。持续承载玩家的平台采用位置复制。
 * (**) Tick 只在视觉过渡期间开启，到达目标后立即关闭，避免静止机关长期空转。
 * (**) 玩家进入和钥匙目标完成的先后顺序不固定，所以人数变化和目标变化都要重新求值，
 * 不能假设一定先完成目标再踩板。
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerPressurePlate : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerPressurePlate();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 返回本机当前状态：服务器为规则真相，客户端为最近一次收到的复制快照。
	bool IsPlateActive() const { return bPlateActive; }

	// 从共享 Occupancy 组件取得不同角色列表，供门进一步验证“不同玩家”数量。
	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	// 状态变化的本机事件。依赖权威结果的机关只在服务器绑定，客户端可用于非规则表现。
	FOnPressurePlateActiveChanged OnPlateActiveChanged;

	// 服务器占用人数变化事件。人数改变但激活布尔值不变时，门仍需重新统计不同玩家。
	FOnPressurePlateOccupancyChanged OnPlateOccupancyChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void OnRep_PlateActive();

	// 只扩展材质、音效等表现，不允许蓝图从这里反向修改 bPlateActive 或共享进度。
	UFUNCTION(BlueprintImplementableEvent, Category = "Pressure Plate", meta = (DisplayName = "On Plate Visual State Changed"))
	void ReceivePlateVisualStateChanged(bool bIsActive);

private:
	// 服务器唯一判定入口：组合锁存、目标前置条件和区域是否有人。
	void EvaluatePlateState();
	// 服务器直接写入与客户端 OnRep 的公共出口，保证两端触发相同表现和事件。
	void HandlePlateActiveChanged();
	// bSnapToTarget 用于初始状态恢复；运行期变化则打开 Tick 做平滑过渡。
	void ApplyPlateState(bool bSnapToTarget);

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

	// 运行中复制的开关状态；网格位置不复制，速度另外在初始复制时提供。
	UPROPERTY(ReplicatedUsing = OnRep_PlateActive)
	bool bPlateActive = false;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	// BeginPlay 保存的初始相对位置，避免把编辑器摆放结果硬编码成坐标。
	FVector ReleasedRelativeLocation = FVector::ZeroVector;
};
