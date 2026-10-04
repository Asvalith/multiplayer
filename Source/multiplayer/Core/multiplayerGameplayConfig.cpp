// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerGameplayConfig.h"

#include "Core/multiplayerLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Network/multiplayerGameInstance.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/// <summary>
/// **写入口**—— 从外部 JSON 文件加载配置，做层层严格校验，成功则原子生效，失败则回退默认值
/// </summary>
bool FmultiplayerGameplayConfig::LoadFromFile(const FString& FilePath)
{
	//失败路径集中记录一次警告，并清除旧配置，避免混用新旧字段。
	const auto Reject = [this, &FilePath](const FString& Reason)
	{
		*this = FmultiplayerGameplayConfig();
		UE_LOG(LogMultiplayer, Warning,
			TEXT("Gameplay config '%s' rejected: %s. Using all defaults."), *FilePath, *Reason);
		return false;
	};

	//检查文件大小
	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
	if (FileSize <= 0 || FileSize > 64 * 1024)
	{
		return Reject(TEXT("file is missing, empty, or larger than 64 KiB"));
	}
	//读取文件内容
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return Reject(TEXT("cannot read file"));
	}
	//解析 JSON
	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return Reject(FString::Printf(TEXT("invalid JSON object: %s"), *Reader->GetErrorMessage()));
	}
	//检查未知字段，避免拼写错误或多余字段被忽略。
	static const TSet<FString> KnownFields = {
		TEXT("SchemaVersion"), TEXT("SessionMaxPlayers"), TEXT("RequiredKeys"), TEXT("WinRequiredPlayers"),
		TEXT("PlatformRequiredPlayers"), TEXT("PlatformMoveSpeed"), TEXT("DoorMoveSpeed"),
		TEXT("PlateMoveSpeed"), TEXT("ReconnectDelaysSeconds")
	};
	for (const auto& Field : JsonObject->Values)
	{
		if (!KnownFields.Contains(Field.Key))
		{
			return Reject(FString::Printf(TEXT("unknown field '%s'"), *Field.Key));
		}
	}
	//存储错误信息
	FString Error;
	//数值读取器，检查存在性、类型、范围和有限性。
	const auto ReadNumber = [&JsonObject, &Error](const TCHAR* Name, double Min, double Max, double& Out)
	{
		const TSharedPtr<FJsonValue>* Value = JsonObject->Values.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Number)
		{
			Error = FString::Printf(TEXT("'%s' is required and must be a number"), Name);
			return false;
		}
		if (!(*Value)->TryGetNumber(Out) || !FMath::IsFinite(Out) || Out < Min || Out > Max)
		{
			Error = FString::Printf(TEXT("'%s' must be finite and in [%g, %g]"), Name, Min, Max);
			return false;
		}
		return true;
	};
	//整数读取器，先用 ReadNumber 验证范围，再拒绝小数。
	const auto ReadInteger = [&ReadNumber, &Error](const TCHAR* Name, int32 Min, int32 Max, int32& Out)
	{
		double Number = 0;
		if (!ReadNumber(Name, Min, Max, Number))
		{
			return false;
		}
		// 范围先验证，保证转换安全；再比较原值，拒绝被截断的小数。
		if (Number != static_cast<double>(static_cast<int32>(Number)))
		{
			Error = FString::Printf(TEXT("'%s' must be an integer"), Name);
			return false;
		}
		Out = static_cast<int32>(Number);
		return true;
	};
	//速度读取器，允许小数，范围 [1, 5000] cm/s
	const auto ReadSpeed = [&ReadNumber](const TCHAR* Name, float& Out)
	{
		double Number = 0;
		if (!ReadNumber(Name, 1, 5000, Number))
		{
			return false;
		}
		Out = static_cast<float>(Number);
		return true;
	};

	//读取并验证所有字段，先构造候选配置，全部通过后再一次性应用。
	FmultiplayerGameplayConfig Candidate;
	int32 SchemaVersion = 0;
	if (!ReadInteger(TEXT("SchemaVersion"), 1, 1, SchemaVersion)
		|| !ReadInteger(TEXT("SessionMaxPlayers"), 2, 8, Candidate.SessionMaxPlayers)
		|| !ReadInteger(TEXT("RequiredKeys"), 1, MAX_int32, Candidate.RequiredKeys)
		|| !ReadInteger(TEXT("WinRequiredPlayers"), 1, Candidate.SessionMaxPlayers, Candidate.WinRequiredPlayers)
		|| !ReadInteger(TEXT("PlatformRequiredPlayers"), 1, Candidate.SessionMaxPlayers, Candidate.PlatformRequiredPlayers)
		|| !ReadSpeed(TEXT("PlatformMoveSpeed"), Candidate.PlatformMoveSpeed)
		|| !ReadSpeed(TEXT("DoorMoveSpeed"), Candidate.DoorMoveSpeed)
		|| !ReadSpeed(TEXT("PlateMoveSpeed"), Candidate.PlateMoveSpeed))
	{
		return Reject(Error);
	}
	//读取重连延迟数组，最多 5 个元素，非空时按秒配置非递减退避。
	const TSharedPtr<FJsonValue>* DelaysValue = JsonObject->Values.Find(TEXT("ReconnectDelaysSeconds"));
	if (!DelaysValue || !DelaysValue->IsValid() || (*DelaysValue)->Type != EJson::Array)
	{
		return Reject(TEXT("'ReconnectDelaysSeconds' is required and must be an array"));
	}
	const TArray<TSharedPtr<FJsonValue>>& Delays = (*DelaysValue)->AsArray();
	if (Delays.Num() > 5)
	{
		return Reject(TEXT("'ReconnectDelaysSeconds' must contain at most 5 entries"));
	}
	//空数组明确表示禁用自动重连；非空数组按秒配置非递减退避。
	Candidate.ReconnectDelaysSeconds.Reset();
	double PreviousDelay = 0;
	for (int32 Index = 0; Index < Delays.Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = Delays[Index];
		double Delay = 0;
		if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Delay)
			|| !FMath::IsFinite(Delay) || Delay < 0.1 || Delay > 60 || Delay < PreviousDelay)
		{
			return Reject(FString::Printf(
				TEXT("'ReconnectDelaysSeconds[%d]' must be a finite number in [0.1, 60], in nondecreasing order"), Index));
		}
		Candidate.ReconnectDelaysSeconds.Add(static_cast<float>(Delay));
		PreviousDelay = Delay;
	}
	// 全部字段通过后才原子提交；任何读取或校验失败都会恢复整套默认值。
	*this = MoveTemp(Candidate);
	UE_LOG(LogMultiplayer, Log,
		TEXT("Gameplay config loaded: '%s'; session=%d, required keys=%d, win=%d, platform=%d, speeds=%.2f/%.2f/%.2f cm/s, reconnect attempts=%d"),
		*FilePath, SessionMaxPlayers, RequiredKeys, WinRequiredPlayers, PlatformRequiredPlayers,
		PlatformMoveSpeed, DoorMoveSpeed, PlateMoveSpeed, ReconnectDelaysSeconds.Num());
	return true;
}

/// <summary>
/// 读入口：获取当前世界的游戏配置；如果无法获取则返回默认配置
/// </summary>
const FmultiplayerGameplayConfig& FmultiplayerGameplayConfig::Get(const UObject* WorldContext)
{
	if (const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr)
	{
		if (const UmultiplayerGameInstance* GameInstance = World->GetGameInstance<UmultiplayerGameInstance>())
		{
			return GameInstance->GetGameplayConfig();
		}
	}
	static const FmultiplayerGameplayConfig Defaults;
	return Defaults;
}
