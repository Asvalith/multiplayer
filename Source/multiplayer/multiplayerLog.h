// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// 项目统一日志分类：会话、重连和合作机关共享同一可过滤标签，便于双实例日志对照与自动测试检索。
// 声明放在头文件、定义放在 multiplayerLog.cpp，避免在多个翻译单元重复定义链接符号。
DECLARE_LOG_CATEGORY_EXTERN(LogMultiplayer, Log, All);
