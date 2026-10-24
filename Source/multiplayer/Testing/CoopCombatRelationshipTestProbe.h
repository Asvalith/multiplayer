// Copyright Epic Games, Inc. All Rights Reserved.

// 为自动化回归提供动态委托所需的反射接收器，不进入生产玩法链路。
// 回调可故意触发同步取消或切换，用于检查真实状态通知和动画开始时的重入边界。

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CoopCombatRelationshipTestProbe.generated.h"

class UAnimMontage;

// 动态事件需要真实反射接收器；只记录测试通知，不参与技能资格或业务结算。
UCLASS(Transient, NotBlueprintable)
class UCoopCombatRelationshipTestProbe final : public UObject
{
	GENERATED_BODY()
public:
	TFunction<void()> StateCallback;
	TFunction<void(UAnimMontage*)> MontageCallback;
	TArray<FText> Messages;

	// 复制本轮回调再执行，允许回调修改接收器而不破坏正在执行的函数对象。
	UFUNCTION()
	void HandleState()
	{
		const TFunction<void()> Callback = StateCallback;
		if (Callback) Callback();
	}

	// 记录正式失败提示，用例据此断言消息次数与内容，不重新推导拒绝原因。
	UFUNCTION()
	void HandleMessage(FText Message)
	{
		Messages.Add(Message);
	}

	// 接收真实蒙太奇开始通知，复制回调后执行可控的取消／替换操作。
	UFUNCTION()
	void HandleMontage(UAnimMontage* Montage)
	{
		const TFunction<void(UAnimMontage*)> Callback = MontageCallback;
		if (Callback) Callback(Montage);
	}
};
