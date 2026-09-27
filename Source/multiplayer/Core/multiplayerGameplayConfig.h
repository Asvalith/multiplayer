// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** 启动时读取的纯 C++ 玩法参数；默认值也是配置无效时的完整回退值。 */
struct MULTIPLAYER_API FmultiplayerGameplayConfig
{
	int32 SessionMaxPlayers = 2;
	int32 WinRequiredPlayers = 2;
	int32 PlatformRequiredPlayers = 1;
	float PlatformMoveSpeed = 150.f;
	float DoorMoveSpeed = 250.f;
	float PlateMoveSpeed = 80.f;
	TArray<float> ReconnectDelaysSeconds = {1.f, 2.f, 4.f};

	/** 完整校验 JSON 后一次性应用；任何读取或校验失败都会恢复整套默认值。 */
	bool LoadFromFile(const FString& FilePath);

	/** 获取当前 GameInstance 的配置；没有有效世界或实例时返回只读默认值。 */
	static const FmultiplayerGameplayConfig& Get(const UObject* WorldContext);
};
