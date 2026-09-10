// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Modules/ModuleManager.h"

// 注册主游戏模块；目前没有模块级启动/关闭工作，因此使用默认实现，玩法初始化交给 UE 对象生命周期。
IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, multiplayer, "multiplayer" );
