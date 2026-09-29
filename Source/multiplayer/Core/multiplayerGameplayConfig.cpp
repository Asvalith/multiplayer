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

bool FmultiplayerGameplayConfig::LoadFromFile(const FString& FilePath)
{
	// 失败路径集中记录一次警告，并清除旧配置，避免混用新旧字段。
	const auto Reject = [this, &FilePath](const FString& Reason)
	{
		*this = FmultiplayerGameplayConfig();
		UE_LOG(LogMultiplayer, Warning,
			TEXT("Gameplay config '%s' rejected: %s. Using all defaults."), *FilePath, *Reason);
		return false;
	};

	// IFileManager 同时支持普通文件和打包后的 UFS 文件。
	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
	if (FileSize <= 0 || FileSize > 64 * 1024)
	{
		return Reject(TEXT("file is missing, empty, or larger than 64 KiB"));
	}
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return Reject(TEXT("cannot read file"));
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return Reject(FString::Printf(TEXT("invalid JSON object: %s"), *Reader->GetErrorMessage()));
	}

	static const TSet<FString> KnownFields = {
		TEXT("SchemaVersion"), TEXT("SessionMaxPlayers"), TEXT("WinRequiredPlayers"),
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

	FString Error;
	// 先检查 JSON 类型，禁止把字符串或布尔值隐式转换为数字。
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

	FmultiplayerGameplayConfig Candidate;
	int32 SchemaVersion = 0;
	if (!ReadInteger(TEXT("SchemaVersion"), 1, 1, SchemaVersion)
		|| !ReadInteger(TEXT("SessionMaxPlayers"), 2, 8, Candidate.SessionMaxPlayers)
		|| !ReadInteger(TEXT("WinRequiredPlayers"), 1, Candidate.SessionMaxPlayers, Candidate.WinRequiredPlayers)
		|| !ReadInteger(TEXT("PlatformRequiredPlayers"), 1, Candidate.SessionMaxPlayers, Candidate.PlatformRequiredPlayers)
		|| !ReadSpeed(TEXT("PlatformMoveSpeed"), Candidate.PlatformMoveSpeed)
		|| !ReadSpeed(TEXT("DoorMoveSpeed"), Candidate.DoorMoveSpeed)
		|| !ReadSpeed(TEXT("PlateMoveSpeed"), Candidate.PlateMoveSpeed))
	{
		return Reject(Error);
	}

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
	// 空数组明确表示禁用自动重连；非空数组按秒配置非递减退避。
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

	// 全部字段通过后才提交，调用方不会观察到部分生效的配置。
	*this = MoveTemp(Candidate);
	UE_LOG(LogMultiplayer, Log,
		TEXT("Gameplay config loaded: '%s'; session=%d, win=%d, platform=%d, speeds=%.2f/%.2f/%.2f cm/s, reconnect attempts=%d"),
		*FilePath, SessionMaxPlayers, WinRequiredPlayers, PlatformRequiredPlayers,
		PlatformMoveSpeed, DoorMoveSpeed, PlateMoveSpeed, ReconnectDelaysSeconds.Num());
	return true;
}

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
