// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// 项目注释约定：
// (*)  表示需要重点理解的 UE 原理或架构取舍，例如 GameMode/GameState 分工、RPC 与属性复制的选择。
// (**) 表示实现时容易出错的边界条件，例如重复 Overlap、RepNotify 两端差异和 Delegate 生命周期。
// 普通注释说明职责和“为什么这样选”，不逐行翻译代码，避免注释与实现重复后逐渐失真。
