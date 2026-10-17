#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Network/multiplayerGameInstance.h"
#include "CoopNetTestDriver.generated.h"

class FJsonObject;
class AmultiplayerCombatCharacter;
class AmultiplayerCombatEnemy;
class UNetDriver;

// 仅测试回执通道，不给业务类增加测试 RPC 或权限后门。
// UHT 在 Shipping 仍生成类型，但测试入口和 RPC 实现均不会激活。
UCLASS(NotBlueprintable, Transient)
class ACoopNetTestProbe : public AActor
{
	GENERATED_BODY()
public:
	ACoopNetTestProbe();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated) FString Stage;
	UPROPERTY(Replicated) TObjectPtr<AActor> Subject;
	UPROPERTY(Replicated) TObjectPtr<AActor> CombatTarget;
	UFUNCTION(Server, Reliable) void ServerReceipt(const FString& Name, bool bPassed, const FString& Detail);
};

UCLASS(Transient)
class UCoopNetTestDriver : public UObject
{
	GENERATED_BODY()
public:
	void Start();
	void Stop();
	void Receipt(const FString& Name, bool bPassed, const FString& Detail, ACoopNetTestProbe* Source = nullptr);
private:
	bool Tick(float DeltaSeconds);
	UWorld* FindWorld() const;
	void ServerTick(UWorld* World);
	void ClientTick(UWorld* World);
	void PartnerTick(UWorld* World);
	APlayerController* PartnerPlayer(UWorld* World) const;
	APlayerController* PrimaryPlayer(UWorld* World) const;
	void MotionServerTick(UWorld* World);
	void MotionClientTick(UWorld* World);
	// 复用现有双连接回执通道，战斗断言必须分别由两个客户端观察。
	void CombatServerTick(UWorld* World);
	void CombatClientTick(UWorld* World);
	bool CombatPredictionServerTick(UWorld* World);
	bool CombatPredictionClientTick(UWorld* World);
	void PredictCombatSkill(AmultiplayerCombatCharacter* Character, const FString& Skill,
		const FString& CaseName, float EnergyBefore, bool bExpectRejection);
	// 只暂停已建立 Actor 通道的属性发送，真实 GAS 请求/拒绝 RPC 继续通过。
	bool HoldCombatStateReplication(UWorld* World, AmultiplayerCombatCharacter* Character);
	void ReleaseCombatStateReplication(AmultiplayerCombatCharacter* Character);
	void ClearCombatStateReplicationHolds();
	void TrafficServerTick(UWorld* World);
	void TrafficClientTick(UWorld* World);
	void ReconnectServerTick(UWorld* World);
	void ReconnectClientTick(UWorld* World);
	// 独立覆盖异步施法预测、取消和控制；不向玩法类增加测试 RPC。
	void PrepareLifecycleLateJoin(UWorld* World);
	void LifecycleServerTick(UWorld* World);
	void LifecycleClientTick(UWorld* World);
	void SetPhase(const FString& Value);
	void SetCommand(const FString& Command);
	void SendReceipt(const FString& Name, bool bPassed, const FString& Detail);
	void Emit(const FString& Status, const FString& Detail = FString(), TSharedPtr<FJsonObject> Extra = nullptr) const;
	void Assert(const FString& Name, bool bPassed, const FString& Detail = FString());
	void Finish(bool bPassed, const FString& Detail = FString());

	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<UmultiplayerGameInstance> GameInstance;
	TWeakObjectPtr<ACoopNetTestProbe> Probe;
	TWeakObjectPtr<ACoopNetTestProbe> PartnerProbe;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CombatPrimary;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CombatPartner;
	TWeakObjectPtr<AmultiplayerCombatEnemy> CombatEnemy;
	TWeakObjectPtr<APlayerController> CombatPreviousController;
	TWeakObjectPtr<APlayerController> ReconnectPartnerController;
	double ReconnectCooldownAt = 0;
	double CombatLifecycleAt = 0;
	// Lifecycle 的真实 W 键回归：服务器保存两端起点，客户端保存自己的输入起点。
	FVector CombatMovementOrigin = FVector::ZeroVector;
	FVector CombatPartnerMovementOrigin = FVector::ZeroVector;
	double CombatMovementAt = 0;
	bool bCombatMovementInputHeld = false;
	TMap<FString, int32> CombatPredictionKeys;
	TSet<TWeakObjectPtr<AmultiplayerCombatCharacter>> CombatReplicationHolds;
	FDelegateHandle CombatReplicationHoldTick;
	TSet<FString> Receipts;
	TSet<FString> SentReceipts;
	FString Mode, Role, Token, Phase;
	FString ServerAddress;
	FString PartnerPlayerId;
	double StartedAt = 0, PhaseAt = 0;
	float TimeoutSeconds = 180;
	// 流量采样使用独立墙钟窗口，不随阶段切换或回执早晚伸缩。
	TWeakObjectPtr<UNetDriver> TrafficDriver;
	double TrafficStartedAt = 0, TrafficWarmupSeconds = 0, TrafficMaxScheduleDelay = 0;
	uint32 TrafficStartOutBytes = 0, TrafficStartOutPackets = 0;
	uint64 TrafficStartFrame = 0;
	int32 TrafficProjectileSpawns = 0, TrafficEnergyUpdates = 0;
	bool bTrafficProjectileObserved = false;
	bool bServer = false, bPartner = false, bDone = false;
};

void StartCoopNetTests();
void StopCoopNetTests();
