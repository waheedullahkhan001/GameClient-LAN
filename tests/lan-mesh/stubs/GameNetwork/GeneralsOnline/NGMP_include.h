#pragma once
enum class ELogVerbosity
{
	LOG_DEBUG = 0,
	LOG_RELEASE = 1
};
void NetworkLog(ELogVerbosity logVerbosity, const char* fmt, ...);
