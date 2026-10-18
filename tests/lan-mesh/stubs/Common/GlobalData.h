#pragma once
struct GlobalData
{
	UnsignedInt m_exeCRC = 0x11111111;
	UnsignedInt m_iniCRC = 0x22222222;
	Int m_networkFPSHistoryLength = 0;
	Int m_networkLatencyHistoryLength = 0;
	Int m_networkRunAheadSlack = 0;
	Int m_networkRunAheadMetricsTime = 0;
};
extern GlobalData g_globalData;
#define TheGlobalData (&g_globalData)
#define TheWritableGlobalData (&g_globalData)
