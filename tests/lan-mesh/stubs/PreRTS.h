#pragma once
// Minimal stand-ins for the engine types LanMesh.cpp uses, so the real file compiles on Linux for testing.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

typedef int32_t Int;
typedef uint32_t UnsignedInt;
typedef uint16_t UnsignedShort;
typedef uint8_t UnsignedByte;
typedef float Real;
typedef int Bool;
typedef wchar_t WideChar;
#define TRUE 1
#define FALSE 0

#define GENERALS_ONLINE 1
#define GENERALS_ONLINE_HIGH_FPS_LIMIT 60

inline UnsignedInt timeGetTime()
{
	using namespace std::chrono;
	return (UnsignedInt)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class AsciiString
{
public:
	AsciiString() {}
	AsciiString(const char* s) : m_s(s) {}
	const char* str() const { return m_s.c_str(); }
private:
	std::string m_s;
};

class UnicodeString
{
public:
	UnicodeString() {}
	UnicodeString(const WideChar* s) : m_s(s) {}
	const WideChar* str() const { return m_s.c_str(); }
	void format(const WideChar* fmt, ...)
	{
		WideChar buf[1024];
		va_list args;
		va_start(args, fmt);
		vswprintf(buf, 1024, fmt, args);
		va_end(args);
		m_s = buf;
	}
private:
	std::wstring m_s;
};
