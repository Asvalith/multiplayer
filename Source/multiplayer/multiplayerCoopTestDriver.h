// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopTestDriver.generated.h"

class ACharacter;
class AmultiplayerCoopGate;
class AmultiplayerMovingPlatform;
class AmultiplayerPressurePlate;

/**
 * 由开发构建命令行启用的双进程自动化驱动，负责服务器侧的主流程检查。
 *
 * 钥匙阶段直接调用插槽登记接口；机关阶段把服务器上的真实玩家移到触发体，读取权威状态。
 * 这覆盖状态登记、重复请求保护与主要机关规则，没有完整覆盖玩家碰撞钥匙后的生产路径。
 * 客户端 UI 和网络错误还要由外部脚本检查两端日志，不能由本 Actor 的服务端通过标记替代。
 * GameMode 只在非 Shipping 构建且显式参数存在时生成它，正常游玩不承担这些扫描和移动成本。
 */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class MULTIPLAYER_API AmultiplayerCoopTestDriver : public AActor
{
	GENERATED_BODY()

public:
	/** 以 0.1 秒间隔推进测试阶段，不把测试进度复制给客户端。 */
	AmultiplayerCoopTestDriver();
	/** 等待两名角色与各阶段结果；发生失败时停止 Tick，并留下脚本可检索的错误日志。 */
	virtual void Tick(float DeltaSeconds) override;

protected:
	/** 区分初次测试与重开后的退出验证，收集当前关卡已有机关并选择带宽对照参数。 */
	virtual void BeginPlay() override;

private:
	// 用单一阶段表示当前测试任务，避免多个等待标记组合出“同时测试两个机关”的状态。
	enum class ETestPhase : uint8
	{
		WaitForPlayers,
		CompleteKeys,
		WaitForPlate,
		WaitForGate,
		WaitForPlatform,
		WaitForVictory,
		Finished,
		PostRestartLeave,
		Failed
	};

	/** 重建控制器已拥有的 Character 列表；至少两名就绪返回 true，重开后也重新获取 Pawn。 */
	bool RefreshPlayers();
	/** 按名称排序钥匙和插槽，测试一次登记与重复拒绝，成功后进入平台阶段。 */
	void CompleteKeyObjectives();
	/** 移动首名玩家到当前板的触发中心，遍历完成后开始门的规则检查。 */
	void ActivateNextPlate();
	/** 检查当前板已经激活，成功则推进索引；依赖本测试关卡的单玩家可触发配置。 */
	void ValidateCurrentPlate();
	/** 从门读取实际依赖，把不同玩家放到所需板上，检查配置可由两人满足。 */
	void ActivateNextGate();
	/** 读取门的服务器开关状态；该检查不测量客户端门动画或碰撞表现。 */
	void ValidateCurrentGate();
	/** 按自身占用或外部板两种模式启动当前平台，并重置本阶段位移记录。 */
	void ActivateNextPlatform();
	/** 同时检查本阶段产生位移且到达活动端点；超过平台等待上限则报失败。 */
	void ValidateCurrentPlatform();
	/** 选取关卡第一个胜利区，将两名角色分别放在中心两侧以触发人数统计。 */
	void EnterWinArea();
	/** 检查 GameState 的权威胜利，结束流量采样，进入可选的重开验证阶段。 */
	void ValidateVictory();
	/** 记录测试时段内服务器总出站字节/秒；统计包含全部复制流量，不只包含移动平台。 */
	void FinishBandwidthSample();
	/** 记录失败原因并关闭 Tick；进程退出和超时回收由外部 PowerShell 脚本负责。 */
	void Fail(const FString& Reason);
	/** 在服务器直接传送角色以驱动 Overlap；不验证输入、寻路或角色移动预测。 */
	void MovePlayerTo(ACharacter* Character, const FVector& Location) const;

	ETestPhase Phase = ETestPhase::WaitForPlayers;
	// 服务端测试期间缓存现有关卡对象；这些数组不是运行时查找服务，也不是网络同步数据。
	TArray<TObjectPtr<ACharacter>> Players;
	TArray<TObjectPtr<AmultiplayerPressurePlate>> Plates;
	TArray<TObjectPtr<AmultiplayerCoopGate>> Gates;
	TArray<TObjectPtr<AmultiplayerMovingPlatform>> Platforms;
	// 只保留当前被测平台的阶段基线，避免此前移动让下一次激活检查误通过。
	FVector CurrentPlatformStartLocation = FVector::ZeroVector;
	float CurrentPlatformMaxDistance = 0.0f;
	int32 CurrentPlateIndex = 0;
	int32 CurrentGateIndex = 0;
	int32 CurrentPlatformIndex = 0;
	// 阶段等待和带宽采样分别计时；平台另有 15 秒上限，其余整体超时由启动脚本控制。
	float PhaseStartedAt = 0.0f;
	float BandwidthStartedAt = 0.0f;
	uint32 BandwidthStartBytes = 0;
	FString BandwidthProfile;
};
