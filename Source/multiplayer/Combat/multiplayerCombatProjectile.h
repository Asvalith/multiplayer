// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCombatProjectile.generated.h"

class AmultiplayerCombatCharacter;
class UPrimitiveComponent;
class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;

// 服务器移动和结算命中，客户端接收位置与销毁；球形网格提供无需额外资产的基础表现。
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatProjectile : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCombatProjectile();

	// 配合延迟生成，在 FinishSpawning 前设置来源、伤害和速度。
	void Initialize(AmultiplayerCombatCharacter* Source, float Damage, float Speed);

protected:
	virtual void BeginPlay() override;

private:
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

	UPROPERTY(VisibleAnywhere, Category = "Combat|Projectile")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	TWeakObjectPtr<AmultiplayerCombatCharacter> SourceCharacter;
	float DamageAmount = 0.0f;
	bool bHitResolved = false;
};
