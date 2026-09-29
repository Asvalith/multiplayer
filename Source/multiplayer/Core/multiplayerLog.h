// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// 项目统一日志分类：连接、重连和合作机关共享标签，便于 DS 与两名客户端的日志对照。
// 声明放在头文件、定义放在 multiplayerLog.cpp，避免在多个翻译单元重复定义链接符号。
DECLARE_LOG_CATEGORY_EXTERN(LogMultiplayer, Log, All);
