// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopCarryComponent.h"

#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.h"

UmultiplayerCoopCarryComponent::UmultiplayerCoopCarryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	// 钥匙的 Holder 已是唯一复制状态，携带槽只做服务器快速查询，避免重复同步同一关系。
	SetIsReplicatedByDefault(false);
}

bool UmultiplayerCoopCarryComponent::TryCarryKey(AmultiplayerCoopKey* Key)
{
	// 这是服务器内部规则缓存，不接受客户端直接写入；真正复制给客户端的是 Key::Holder。
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority() || Key == nullptr)
	{
		return false;
	}

	if (CarriedKey != nullptr && !IsValid(CarriedKey))
	{
		// (**) UObject 指针非空也可能已经 PendingKill，使用 IsValid 处理延迟销毁窗口。
		CarriedKey = nullptr;
	}

	if (CarriedKey != nullptr && CarriedKey != Key)
	{
		// 单槽设计明确拒绝第二把钥匙，避免一个角色的附着表现与服务器库存关系不一致。
		return false;
	}

	// 对同一把钥匙重复调用保持成功，方便上层事务在状态重检后安全收口。
	CarriedKey = Key;
	return true;
}

void UmultiplayerCoopCarryComponent::ClearCarriedKey(
	const AmultiplayerCoopKey* ExpectedKey)
{
	// ExpectedKey 是轻量的“比较后清除”条件：旧对象的迟到回调不能清掉刚写入的新钥匙。
	AActor* Owner = GetOwner();
	if (Owner != nullptr
		&& Owner->HasAuthority()
		&& CarriedKey == ExpectedKey)
	{
		// (**) 只清理由调用方声明的旧钥匙，避免迟到回调覆盖更新后的携带状态。
		CarriedKey = nullptr;
	}
}
