#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Network/multiplayerGameInstance.h"
#include "CoopNetTestDriver.generated.h"

class FJsonObject;
class AmultiplayerCombatCharacter;
class AmultiplayerCombatEnemy;

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
	TSet<FString> Receipts;
	TSet<FString> SentReceipts;
	FString Mode, Role, Token, Phase;
	FString ServerAddress;
	FString PartnerPlayerId;
	double StartedAt = 0, PhaseAt = 0;
	float TimeoutSeconds = 180;
	bool bServer = false, bPartner = false, bDone = false;
};

void StartCoopNetTests();
void StopCoopNetTests();
