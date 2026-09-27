#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Network/multiplayerGameInstance.h"
#include "CoopNetTestDriver.generated.h"

class FJsonObject;
class UNetDriver;
class AmultiplayerMovingPlatform;

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
	UPROPERTY(Replicated) FVector Endpoint = FVector::ZeroVector;
	UPROPERTY(Replicated) TArray<TObjectPtr<AActor>> Subjects;
	UFUNCTION(Server, Reliable) void ServerReceipt(const FString& Name, bool bPassed, const FString& Detail);
};

UCLASS(Transient)
class UCoopNetTestDriver : public UObject
{
	GENERATED_BODY()
public:
	void Start();
	void Stop();
	void Receipt(const FString& Name, bool bPassed, const FString& Detail);
private:
	bool Tick(float DeltaSeconds);
	UWorld* FindWorld() const;
	void SetPhase(const FString& Value);
	void Emit(const FString& Status, const FString& Detail = FString(), TSharedPtr<FJsonObject> Extra = nullptr) const;
	void Assert(const FString& Name, bool bPassed, const FString& Detail = FString());
	void Finish(bool bPassed, const FString& Detail = FString());
	void HostTick(UWorld* World);
	void ClientTick(UWorld* World);
	void ScaleTick(UWorld* World);
	void BeginSample(UWorld* World);
	bool EndSample(UWorld* World);
	void SpawnScale(UWorld* World);
	bool CheckScaleLoad(UWorld* World, bool bRequireObservedMovement);
	// 回归场景单独实现，避免把测试步骤混进 Session 和机关业务类。
	void PrepareLateJoin(UWorld* World);
	void ScenarioHostTick(UWorld* World);
	void ScenarioClientTick(UWorld* World);
	void MotionHostTick(UWorld* World);
	void MotionClientTick(UWorld* World);
	void SetCommand(const FString& Command);
	void SendReceipt(const FString& Name, bool bPassed, const FString& Detail);
	void CollectMapKeys(UWorld* World, bool bTwoPlayers);
	void CreateGateFixture(UWorld* World);
	void NetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type Type, const FString& Error);
	UFUNCTION() void FoundGames(bool bSuccess, const TArray<FmultiplayerSessionInfo>& Results);
	UFUNCTION() void SessionOperation(EMultiplayerSessionOperation Operation);

	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<UmultiplayerGameInstance> GameInstance;
	TWeakObjectPtr<ACoopNetTestProbe> Probe;
	TWeakObjectPtr<UNetDriver> SampleDriver;
	TArray<TWeakObjectPtr<AmultiplayerMovingPlatform>> MovingActors;
	TArray<FVector> MovingStarts;
	TArray<bool> MovingForward;
	TSet<FString> Receipts;
	TSet<FString> SentReceipts;
	FString Mode, Role, Token, Phase, Optimization, Matrix, CsvRequestedPath;
	EMultiplayerSessionOperation Operation = EMultiplayerSessionOperation::None;
	double StartedAt = 0, PhaseAt = 0, NextAttemptAt = 0, SampleAt = 0, SampleElapsed = 0;
	float WarmupSeconds = 10, SampleSeconds = 30, TimeoutSeconds = 180;
	int32 StaticCount = 0, MovingCount = 1, Attempts = 0;
	uint32 StartBytes = 0, StartPackets = 0, SampleBytes = 0, SamplePackets = 0;
	bool bHost = false, bDone = false, bSampleEnding = false, bMetricEmitted = false;
	TSharedFuture<FString> CsvFinished;
	// 压测就绪证据：保存客户端真正收到的对象及位移，不用启动参数冒充实测数量。
	TMap<TWeakObjectPtr<AmultiplayerMovingPlatform>, FVector> ScaleObservedStarts;
	TSet<TWeakObjectPtr<AmultiplayerMovingPlatform>> ScaleMovedActors;
	FString ScaleLoadDetail;
	int32 ScaleObservedStaticCount = 0, ScaleObservedMovingCount = 0;
	TWeakObjectPtr<UWorld> PreviousWorld;
	TWeakObjectPtr<APlayerController> PreviousRemote;
	TArray<TWeakObjectPtr<AActor>> Fixture;
	FVector RideStart = FVector::ZeroVector;
	float OutageSeconds = 25, RideMaxOffset = 0;
	int32 RideSamples = 0, RideBasedSamples = 0;
	bool bSawNetworkFailure = false, bSessionFailureChecked = false;
};

void StartCoopNetTests();
void StopCoopNetTests();
