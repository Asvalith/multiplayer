// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "multiplayerCombatEncounter.generated.h"

class AmultiplayerCombatEnemy;
class AmultiplayerCoopGameState;
class AmultiplayerEnemySpawner;
class AmultiplayerGameMode;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

// 只编排关卡波次；每波继续复用现有出生器、BT、GAS和CMC，不建立另一套战斗逻辑。
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatEncounter : public AActor
{
	GENERATED_BODY()
public:
	AmultiplayerCombatEncounter();
	int32 GetCurrentWave() const { return CurrentWave; }
	int32 GetLivingEnemyCount() const;
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditAnywhere, Category="Encounter") TSubclassOf<AmultiplayerCombatEnemy> EnemyClass;
	UPROPERTY(EditAnywhere, Category="Encounter") TArray<int32> WaveSizes{3, 4, 5};
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="5")) float WaveInterval = 30.f;
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="1", ClampMax="20")) int32 MaxLivingEnemies = 8;
	UPROPERTY(EditAnywhere, Category="Encounter", meta=(ClampMin="1", ClampMax="4")) int32 StartPlayers = 2;
private:
	void TryStart();
	void StartNextWave();
	void RegisterEnemy(AmultiplayerCombatEnemy* Enemy);
	void PublishState();
	UFUNCTION() void HandleEnemyChanged();
	UFUNCTION() void HandleEnemyDestroyed(AActor* Actor);
	UFUNCTION() void StopEncounter();
	FTimerHandle WaveTimer;
	FDelegateHandle AcceptedHandle;
	TWeakObjectPtr<AmultiplayerGameMode> BoundGameMode;
	TWeakObjectPtr<AmultiplayerCoopGameState> BoundGameState;
	TArray<TWeakObjectPtr<AmultiplayerCombatEnemy>> Enemies;
	TArray<TWeakObjectPtr<AmultiplayerEnemySpawner>> Spawners;
	int32 CurrentWave = 0;
	bool bStarted = false;
	bool bStopped = false;
};

// 补给经同一个GAS属性入口生效；满资源不消耗，服务器先占用再施加，避免双人重复领取。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatSupplyEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerCombatSupplyEffect();
};

UCLASS()
class MULTIPLAYER_API AmultiplayerCombatSupply : public AActor
{
	GENERATED_BODY()
public:
	AmultiplayerCombatSupply();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	bool IsConsumed() const { return bConsumed; }
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditAnywhere, Category="Supply", meta=(ClampMin="0")) float HealthAmount = 35.f;
	UPROPERTY(EditAnywhere, Category="Supply", meta=(ClampMin="0")) float EnergyAmount = 40.f;
private:
	UFUNCTION() void HandleOverlap(UPrimitiveComponent* Component, AActor* Other, UPrimitiveComponent* OtherComponent,
		int32 BodyIndex, bool bFromSweep, const FHitResult& Hit);
	UFUNCTION() void OnRep_Consumed();
	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Trigger;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> Label;
	UPROPERTY(ReplicatedUsing=OnRep_Consumed) bool bConsumed = false;
};
