// Copyright Epic Games, Inc. All Rights Reserved.

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
	AmultiplayerCombatProjectile();

	// 配合延迟生成，在 FinishSpawning 前设置来源、伤害和速度。
	void Initialize(AmultiplayerCombatCharacter* Source, float Damage, float Speed, float MaxDistance = 0.f);

	// 只读观测初始状态，测试和统计不需要向客户端开放发射或伤害入口。
	const FMultiplayerCombatProjectileLaunchState& GetLaunchState() const { return LaunchState; }

	// 仅服务器碰撞业务可调用；防御窗口、方向、敌我与次数仍在此处重新检查，不开放 RPC。
	bool TryReflectFrom(AmultiplayerCombatCharacter* Defender, const FHitResult& Hit);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// Owner 与发射状态可能分批到达；所属角色就绪后补齐表现，不新增网络字段。
	virtual void OnRep_Owner() override;

private:
	// RepNotify 与 BeginPlay 汇合到一个入口，兼容状态早于 BeginPlay 或 GameState 到达。
	UFUNCTION()
	void OnRep_LaunchState();
	void StartClientTrajectory();
	void UpdateClientTrajectory();
	// 原扫掠的重叠/阻挡通知可能仍在派发；离开该栈后才恢复新轨迹，不能处理旧方向的命中。
	void ResumeReflectedTrajectory();
	// 已预载直接绑定，否则只发一次异步请求；不在 Tick 中查找或同步加载资产。
	void RequestProjectileEffect();
	void AttachProjectileEffect();

	// 场景用阻挡命中，角色用重叠命中，两种引擎回调共用结算入口。
	UFUNCTION()
	void HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

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
