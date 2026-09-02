// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerCoopCarryComponent.generated.h"

class AmultiplayerCoopKey;

/**
 * 服务器维护的单钥匙携带槽。
 *
 * 组件是角色上的规则缓存：插槽可以 O(1) 找到玩家携带的钥匙，而不必扫描场景或依赖附着层级。
 * 它本身不调用 SetIsReplicatedByDefault，CarriedKey 也不进入 GetLifetimeReplicatedProps；客户端
 * 真正需要观察的是钥匙 Actor 上复制的 Holder。这样“谁拿着钥匙”只有一份网络真相。
 *
 * (*) 组件不复制自己的指针，钥匙的 Holder 才是网络状态的唯一来源，避免两份状态不一致。
 * (**) 清理时传入 ExpectedKey 做“比较后清除”，防止旧钥匙的销毁回调误清掉刚拾取的新钥匙。
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerCoopCarryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerCoopCarryComponent();

	// 只用于服务器规则查询；返回值仍需由调用者用 IsValid 和 Key::IsHeldBy 交叉校验。
	AmultiplayerCoopKey* GetCarriedKey() const { return CarriedKey; }

	/**
	 * 在服务器上尝试占用携带槽。
	 * @return Key 有效且当前槽为空时返回 true；同一把钥匙的重复请求也不会覆盖其他状态。
	 */
	bool TryCarryKey(AmultiplayerCoopKey* Key);
	/**
	 * 仅当槽内仍是 ExpectedKey 时清空。
	 * “比较后清除”用于抵抗销毁/解绑回调迟到，不能无条件把 CarriedKey 设为空。
	 */
	void ClearCarriedKey(const AmultiplayerCoopKey* ExpectedKey);

private:
	// 非复制缓存。BlueprintReadOnly 只代表编辑器/蓝图可读，不代表该属性会自动进行网络复制。
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Coop|Carry", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<AmultiplayerCoopKey> CarriedKey;
};
