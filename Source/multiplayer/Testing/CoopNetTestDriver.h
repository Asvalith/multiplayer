#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Network/multiplayerGameInstance.h"
#include "Combat/multiplayerCombatAbility.h"
#include "CoopNetTestDriver.generated.h"

class FJsonObject;
class AmultiplayerCombatCharacter;
class AmultiplayerCombatEnemy;
class AmultiplayerCombatProjectile;
class UNetDriver;
class USceneComponent;
struct FKey;
enum class EMultiplayerCombatSkill : uint8;

// 仅测试回执通道，不给业务类增加测试 RPC 或权限后门。
// UHT 在 Shipping 仍生成类型，但测试入口和 RPC 实现均不会激活。
UCLASS(NotBlueprintable, Transient)
class ACoopNetTestProbe : public AActor
{
	GENERATED_BODY()
public:
	// 创建仅开发模式启用的常相关探针，正式关卡流程不生成。
	ACoopNetTestProbe();
	// 同步测试协调字段，不替代角色/机关的生产复制字段。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated) FString Stage;
	UPROPERTY(Replicated) TObjectPtr<AActor> Subject;
	UPROPERTY(Replicated) TObjectPtr<AActor> CombatTarget;
	// 所属客户端提交观察回执，服务器驱动继续核对来源与唯一名称。
	UFUNCTION(Server, Reliable) void ServerReceipt(const FString& Name, bool bPassed, const FString& Detail);
};

// 显式命令行启用的三进程验收驱动，负责夹具、输入和证据，不作为正式玩法管理器。
// 服务端断言与两客户端观察分别完成；测试准备改写只能发生在标明的隔离夹具中。
UCLASS(Transient)
class UCoopNetTestDriver : public UObject
{
	GENERATED_BODY()
public:
	// 读取场景参数并安装开发期轮询，校验测试组合与唯一标识。
	void Start();
	// 幂等撤销测试 Tick、监听、复制暂缓与模拟按键。
	void Stop();
	// DS 只接受预期所属 Probe 的一次回执，避免将同一端算成两端。
	void Receipt(const FString& Name, bool bPassed, const FString& Detail, ACoopNetTestProbe* Source = nullptr);
private:
	// 管理测试时限、角色分派和完成后的延迟进程退出。
	bool Tick(float DeltaSeconds);
	// 找到本进程已开始运行的 Game 世界，排除编辑器预览。
	UWorld* FindWorld() const;
	// DS 等待双端接纳与探针就绪，再推进选定场景。
	void ServerTick(UWorld* World);
	// 主测试客户端负责正式业务请求并独立回报本端观察。
	void ClientTick(UWorld* World);
	// 首个 Partner 客户端独立观察，不能沿用主端结果。
	void PartnerTick(UWorld* World);
	// 依据稳定网络 ID 找到 Partner，不依赖重新登录顺序。
	APlayerController* PartnerPlayer(UWorld* World) const;
	// Partner 确定后选择另一远端玩家为主测试对象。
	APlayerController* PrimaryPlayer(UWorld* World) const;
	// DS 布置动态载人夹具并核对实际运动，不关闭原生校正。
	void MotionServerTick(UWorld* World);
	// 所属端观察真实基座、行走和起落，再发送本端载人回执。
	void MotionClientTick(UWorld* World);
	// 复用现有双连接回执通道，战斗断言必须分别由两个客户端观察。
	void CombatServerTick(UWorld* World);
	// 双客户端各查自己的复制结果，仅主端提交技能请求。
	void CombatClientTick(UWorld* World);
	// 服务器构造拒绝条件，等待真实预测/拒绝/撤销后恢复合法重试。
	bool CombatPredictionServerTick(UWorld* World);
	// 所属端在属性暂缓期间确认原生预测与拒绝撤销，旁观端验证独立状态。
	bool CombatPredictionClientTick(UWorld* World);
	// 同一输入调用栈记录真实预测键和 GE，不直接替客户端扣费或移除效果。
	void PredictCombatSkill(AmultiplayerCombatCharacter* Character, const FString& Skill,
		const FString& CaseName, float EnergyBefore, bool bExpectRejection);
	// 只暂停已建立 Actor 通道的属性发送，真实 GAS 请求/拒绝 RPC 继续通过。
	bool HoldCombatStateReplication(UWorld* World, AmultiplayerCombatCharacter* Character);
	// 场景完成预测断言后恢复指定 Actor 的正常属性发送。
	void ReleaseCombatStateReplication(AmultiplayerCombatCharacter* Character);
	// 退出或失败时撤销全部测试复制暂缓，不留永久冻结。
	void ClearCombatStateReplicationHolds();
	// DS 在固定墙钟窗口生成相同业务负载并采样流量，不把测试 RPC 当优化结果。
	void TrafficServerTick(UWorld* World);
	// 双端验证真实弹体及最终属性到达；这里只作契约观察，不统计服务器包数。
	void TrafficClientTick(UWorld* World);
	// DS 断开真实连接，检验本局服务器快照、票据和离线经过时间。
	void ReconnectServerTick(UWorld* World);
	// 客户端经正式重连握手恢复并核对结果，不自行重新授予能力或恢复资源。
	void ReconnectClientTick(UWorld* World);
	// 新能力仍走所属连接与生产组件 RPC；Probe 仅协调三进程观察和返回断言。
	void ExpansionServerTick(UWorld* World);
	// 所属端发冲刺/成长/救援业务请求，并独立验证同步与预测结果。
	void ExpansionClientTick(UWorld* World);
	// 释放 Expansion 视觉验收实际按住的 F；正常完成、失败和退出共用同一输入清理。
	void ClearCombatRescueInput();
	// 请求本轮 token 下的截图并等待非空 PNG，NullRHI 明确标注无视觉证据。
	bool CaptureEscapeView(const FString& Name);
	// 保留原相机与 FOV，正常调整观察方向后保存技能画面。
	bool CaptureSkillView(const FString& Name, AActor* Subject, AActor* Other = nullptr);
	// 经真实按住/松键检查预览与预测；截图后先取消，避免额外确认施法。
	bool CaptureTargetingInput(UWorld* World, const FString& Name, const FKey& Key,
		EMultiplayerCombatSkill Skill, AmultiplayerCombatCharacter* ExpectedTarget = nullptr);
	// 记录真实 E 输入、生命通知和新建治疗特效，不自行播放 Cue。
	void PrepareCombatSelfHealObservation(UWorld* World, AmultiplayerCombatCharacter* Character);
	// 解除自疗观察并复原该夹具临时挪开的队友。
	void ClearCombatSelfHealFixture();
	// 核对已保存正式角色/HUD/配置身份，不能换测试 HUD 冒充视觉通过。
	bool PrepareVisualFixture(UWorld* World);
	// 视觉模式用正式敌人蓝图，纯数值模式用原生测试敌人。
	UClass* CombatFixtureEnemyClass() const;
	// 双客户端核对实际 Pawn、HUD、动画和配置，等待必要表现资源就绪。
	bool ValidateVisualFixturePawn(AmultiplayerCombatCharacter* Character);
	// 独立覆盖异步施法预测、取消和控制；不向玩法类增加测试 RPC。
	void PrepareLifecycleLateJoin(UWorld* World);
	// DS 组织出手、取消、控制与瞄准夹具，并等待双连接观察。
	void LifecycleServerTick(UWorld* World);
	// 所属端通过真实 R 输入帧及业务入口检验预测/取消/移动恢复。
	void LifecycleClientTick(UWorld* World);
	// 观察真实出弹和血量变化；退出或断言失败时也解除测试委托。
	void ClearCombatAimFixture();
	// 只变更测试阶段并记录本进程阶段时钟。
	void SetPhase(const FString& Value);
	// 服务器复制两个 Probe 的当前测试指令。
	void SetCommand(const FString& Command);
	// 客户端对命名断言去重，并经所属 Probe 回报。
	void SendReceipt(const FString& Name, bool bPassed, const FString& Detail);
	// 写带角色、token、阶段的结构化测试日志。
	void Emit(const FString& Status, const FString& Detail = FString(), TSharedPtr<FJsonObject> Extra = nullptr) const;
	// 写断言证据，失败即统一结束本次测试。
	void Assert(const FString& Name, bool bPassed, const FString& Detail = FString());
	// 清理临时观察/输入并只发布一次 DONE，退出由 Tick 延迟执行。
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
	bool bCombatRescueInputHeld = false;
	TWeakObjectPtr<UWorld> CombatAimWorld;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CombatAimTarget;
	TWeakObjectPtr<AmultiplayerCombatProjectile> CombatAimProjectile;
	FDelegateHandle CombatAimControlTick, CombatAimSpawned, CombatAimHealthChanged;
	// R 按下/松开必须经过输入帧，再在该帧网络接收之前检查真实预测提交。
	FDelegateHandle CombatAreaInputTick;
	FDelegateHandle CombatAreaCueTrace;
	int32 CombatAreaInputStep = 0;
	FVector CombatAreaReleaseCenter = FVector::ZeroVector;
	bool bCombatAreaReleaseCenter = false;
	TSet<TWeakObjectPtr<USceneComponent>> CombatAreaEffectsBefore;
	// 视觉专项仅观察两次真实 R 的身份与组件，不自行产生结果或取消消息。
	FmultiplayerCombatActivationIdentity CombatAreaIdentityA, CombatAreaIdentityB;
	TWeakObjectPtr<USceneComponent> CombatAreaIdentityEffectA, CombatAreaIdentityEffectB;
	double CombatAreaIdentityCooldownDeadline = 0;
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
	TMap<FString, double> EscapeCaptureWaitStarted;
	bool bTrafficProjectileObserved = false;
	bool bVisualAcceptance = false;
	bool bServer = false, bPartner = false, bDone = false;
};

// 模块启动钩子，仅显式开发命令行启用测试驱动。
void StartCoopNetTests();
// 引擎退出/模块卸载钩子，对称释放开发驱动。
void StopCoopNetTests();
