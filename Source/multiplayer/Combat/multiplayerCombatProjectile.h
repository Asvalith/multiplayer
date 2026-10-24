// Copyright Epic Games, Inc. All Rights Reserved.
// 弹体模块只接受服务器技能发射并权威扫掠，客户端按分段直线快照重建飞行表现。
// 弹反改变来源与轨迹段但不延寿；命中、异步特效和定时器分别在自己的生命周期清理。

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Actor.h"
#include "multiplayerCombatProjectile.generated.h"

class AmultiplayerCombatCharacter;
class AGameStateBase;
class UPrimitiveComponent;
class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UParticleSystem;
class UParticleSystemComponent;
struct FStreamableHandle;

// 本 Demo 的每段弹道恒速直线飞行；初次相关或弹反改变轨迹时发送快照，不持续发送位置。
USTRUCT()
struct FMultiplayerCombatProjectileLaunchState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize10 Origin = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize10 Velocity = FVector::ZeroVector;

	UPROPERTY()
	double ServerLaunchTime = 0.0;

	// 弹反只开启新轨迹段，不延长最初的服务器寿命；晚加入也按同一个截止时刻隐藏。
	UPROPERTY()
	double ServerExpiryTime = 0.0;

	UPROPERTY()
	uint16 Revision = 0;

	UPROPERTY()
	bool bInitialized = false;
};

// 服务器扫掠和结算伤害，客户端按服务器时钟重建显示；生命周期和销毁仍由服务器决定。
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatProjectile : public AActor
{
	GENERATED_BODY()

public:
	// 配置复制快照和服务器扫掠组件，客户端 Tick 在发射状态就绪后才启用。
	AmultiplayerCombatProjectile();

	// 普攻的服务器出手入口：检查真实弹源并延迟生成，不接受客户端起点、命中对象或伤害 RPC。
	static bool FireFromSkill(AmultiplayerCombatCharacter& Source, float Damage, float Speed,
		const FRotator* AimSnapshot = nullptr);

	// 配合延迟生成，在 FinishSpawning 前设置来源、伤害和速度。
	void Initialize(AmultiplayerCombatCharacter* Source, float Damage, float Speed, float MaxDistance = 0.f);

	// 只读观测初始状态，测试和统计不需要向客户端开放发射或伤害入口。
	const FMultiplayerCombatProjectileLaunchState& GetLaunchState() const { return LaunchState; }

	// 仅服务器碰撞业务可调用；防御窗口、方向、敌我与次数仍在此处重新检查，不开放 RPC。
	bool TryReflectFrom(AmultiplayerCombatCharacter* Defender, const FHitResult& Hit);

protected:
	// 服务器启用真实碰撞，客户端关闭模拟并等待轨迹和服务器时钟。
	virtual void BeginPlay() override;
	// 客户端只按时钟更新显示位置，不在 Tick 中命中或结算。
	virtual void Tick(float DeltaSeconds) override;
	// 取消重试、弹反恢复和异步加载，销毁本地特效组件。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 新通道或弹反后复制最新轨迹段，不逐帧复制位置。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// Owner 与发射状态可能分批到达；所属角色就绪后补齐表现，不新增网络字段。
	virtual void OnRep_Owner() override;

private:
	// RepNotify 与 BeginPlay 汇合到一个入口，兼容状态早于 BeginPlay 或 GameState 到达。
	UFUNCTION()
	void OnRep_LaunchState();
	// 时钟尚未到达时短时重试；就绪后开启客户端轨迹显示。
	void StartClientTrajectory();
	// 由服务器段起点、速度和弹龄定位；到期只隐藏，不自行权威销毁。
	void UpdateClientTrajectory();
	// 原扫掠的重叠/阻挡通知可能仍在派发；离开该栈后才恢复新轨迹，不能处理旧方向的命中。
	void ResumeReflectedTrajectory();
	// 已预载直接绑定，否则只发一次异步请求；不在 Tick 中查找或同步加载资产。
	void RequestProjectileEffect();
	// 资源到达时复查弹龄并附着特效，过期或已命中的弹体不补播。
	void AttachProjectileEffect();

	// 场景用阻挡命中，角色用重叠命中，两种引擎回调共用结算入口。
	UFUNCTION()
	void HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

	// 仅敌方角色重叠进入命中入口，出生重叠用当前弹体位置补足表现上下文。
	UFUNCTION()
	void HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	// 在进入 GameplayEffect 的外部通知前锁定命中，避免同一次碰撞重复结算。
	void ResolveImpact(AActor* OtherActor, const FHitResult& Hit);

	UPROPERTY(VisibleAnywhere, Category = "Combat|Projectile")
	TObjectPtr<USphereComponent> CollisionSphere;

	UPROPERTY(VisibleAnywhere, Category = "Combat|Projectile")
	TObjectPtr<UStaticMeshComponent> ProjectileMesh;

	// 粒子跟随同一个碰撞根组件，位置仍由服务器运动或客户端初始弹道驱动。
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> ProjectileEffect;
	UPROPERTY(Transient)
	TObjectPtr<UParticleSystemComponent> ProjectileCascadeEffect;

	UPROPERTY(VisibleAnywhere, Category = "Combat|Projectile")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	UPROPERTY(ReplicatedUsing = OnRep_LaunchState)
	FMultiplayerCombatProjectileLaunchState LaunchState;

	TWeakObjectPtr<AmultiplayerCombatCharacter> SourceCharacter;
	TWeakObjectPtr<AGameStateBase> ClientGameState;
	FTimerHandle ClientLaunchRetry;
	FTimerHandle ReflectionResume;
	TSoftObjectPtr<UNiagaraSystem> ProjectileEffectAsset;
	TSoftObjectPtr<UParticleSystem> ProjectileCascadeAsset;
	TSharedPtr<FStreamableHandle> ProjectileEffectLoad;
	float DamageAmount = 0.0f;
	int32 ReflectionCount = 0;
	int32 ReflectionLimit = 0;
	bool bHitResolved = false;
	bool bReflectionPending = false;
	bool bResolvingImpact = false;
	bool bPresentationEnded = false;
};
