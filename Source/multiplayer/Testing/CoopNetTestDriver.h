#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Network/multiplayerGameInstance.h"
#include "CoopNetTestDriver.generated.h"

class FJsonObject;
class AmultiplayerCombatCharacter;
class AmultiplayerCombatEnemy;
class AmultiplayerCombatProjectile;
class UNetDriver;
class UCapsuleComponent;
class USceneComponent;
struct FKey;
enum class EMultiplayerCombatSkill : uint8;

// 仅开发验收的导航意图；路线来自真实 Recast，所属客户端仍以输入驱动 CMC。
USTRUCT()
struct FCoopEscapeRoute
{
	GENERATED_BODY()
	UPROPERTY() FString Command;
	UPROPERTY() TArray<FVector> Points;
	UPROPERTY() int32 Revision = 0;
};

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
	UPROPERTY(Replicated) FCoopEscapeRoute EscapeRoute;
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
	// 新能力仍走所属连接与生产组件 RPC；Probe 仅协调三进程观察和返回断言。
	void ExpansionServerTick(UWorld* World);
	void ExpansionClientTick(UWorld* World);
	void EscapeServerTick(UWorld* World);
	void EscapeClientTick(UWorld* World);
	bool SetEscapeMove(UWorld* World, const FString& Command, const FVector& PrimaryGoal, const FVector& PartnerGoal);
	bool CaptureEscapeView(const FString& Name);
	bool CaptureSkillView(const FString& Name, AActor* Subject, AActor* Other = nullptr);
	bool CaptureTargetingInput(UWorld* World, const FString& Name, const FKey& Key,
		EMultiplayerCombatSkill Skill, AmultiplayerCombatCharacter* ExpectedTarget = nullptr);
	void PrepareCombatSelfHealObservation(UWorld* World, AmultiplayerCombatCharacter* Character);
	void ClearCombatSelfHealFixture();
	bool PrepareVisualFixture(UWorld* World);
	bool PrepareVisualFixtureSurface(UWorld* World, AmultiplayerCombatCharacter* Character);
	UClass* CombatFixtureEnemyClass() const;
	bool ValidateVisualFixturePawn(AmultiplayerCombatCharacter* Character);
	void SetEscapeWalking(APlayerController* Controller, bool bWalk);
	void ClearEscapeInput();
	void BindEscapeHitDiagnostics(AmultiplayerCombatCharacter* Character);
	UFUNCTION() void HandleEscapeCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
	// 独立覆盖异步施法预测、取消和控制；不向玩法类增加测试 RPC。
	void PrepareLifecycleLateJoin(UWorld* World);
	void LifecycleServerTick(UWorld* World);
	void LifecycleClientTick(UWorld* World);
	// 观察真实出弹和血量变化；退出或断言失败时也解除测试委托。
	void ClearCombatAimFixture();
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
	TWeakObjectPtr<UWorld> CombatAimWorld;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CombatAimTarget;
	TWeakObjectPtr<AmultiplayerCombatProjectile> CombatAimProjectile;
	FDelegateHandle CombatAimControlTick, CombatAimSpawned, CombatAimHealthChanged;
	// R 按下/松开必须经过输入帧，再在该帧网络接收之前检查真实预测提交。
	FDelegateHandle CombatAreaInputTick;
	int32 CombatAreaInputStep = 0;
	FVector CombatAreaReleaseCenter = FVector::ZeroVector;
	bool bCombatAreaReleaseCenter = false;
	TSet<TWeakObjectPtr<USceneComponent>> CombatAreaEffectsBefore;
	// 自疗必须经过真实 E 输入；两端记录旧组件与独立生命变化，不能沿用上次队友治疗的特效。
	TWeakObjectPtr<UWorld> CombatSelfHealWorld;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CombatSelfHealObserved;
	TWeakObjectPtr<USceneComponent> CombatSelfHealEffect;
	FDelegateHandle CombatSelfHealInputTick, CombatSelfHealHealthChanged;
	TSet<TWeakObjectPtr<USceneComponent>> CombatSelfHealEffectsBefore;
	FVector CombatSelfHealPartnerOrigin = FVector::ZeroVector;
	int32 CombatSelfHealInputStep = 0, CombatSelfHealHealthChanges = 0, CombatSelfHealPreviousKey = 0;
	bool bCombatSelfHealExactHealthChange = false, bCombatSelfHealPartnerMoved = false;
	FVector CombatAimTargetOrigin = FVector::ZeroVector;
	FVector CombatAimExpectedDirection = FVector::ZeroVector;
	FString CombatPreviewInputName;
	uint64 CombatPreviewInputFrame = 0;
	FRotator CombatPreviewInputRotation = FRotator::ZeroRotator;
	float CombatPreviewInputEnergy = 0, CombatPreviewInputHealth = 0;
	TMap<FString, int32> CombatPreviewInputKeys;
	FDelegateHandle CombatPreviewFixtureTick;
	int32 CombatPreviewFixtureWorldTicks = 0;
	double CombatAimHitTime = 0;
	int32 CombatAimProjectileCount = 0, CombatAimDamageCount = 0;
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
	TWeakObjectPtr<UWorld> EscapeRoundWorld;
	FString EscapeLocalCommand;
	int32 EscapeRouteRevision = -1, EscapeRoutePoint = 0;
	int32 EscapeEnemyBirths = 0;
	FDelegateHandle EscapeSpawnObserver;
	FVector EscapePrimaryOrigin = FVector::ZeroVector, EscapePartnerOrigin = FVector::ZeroVector;
	FVector EscapePrimaryGoal = FVector::ZeroVector, EscapePartnerGoal = FVector::ZeroVector;
	double EscapeLastRepathAt = 0, EscapeLastAttackAt = 0, EscapeLastDefenseAt = 0, EscapeLastHealAt = 0;
	double EscapeLastSampleAt = 0;
	double EscapeFirstEnemyAt = 0;
	double EscapeLastHitAt = 0;
	TMap<TWeakObjectPtr<UCapsuleComponent>, bool> EscapeHitSources;
	TMap<FString, double> EscapeCaptureWaitStarted;
	double EscapeEmptySince = 0;
	float EscapePrimaryMinHealth = 100.f, EscapePartnerMinHealth = 100.f;
	bool bEscapeWalking = false, bEscapeRescueHeld = false, bEscapeEnterHeld = false;
	bool bTrafficProjectileObserved = false;
	bool bVisualAcceptance = false;
	bool bEscapeMovementDiagnostic = false;
	bool bServer = false, bPartner = false, bDone = false;
};

void StartCoopNetTests();
void StopCoopNetTests();
