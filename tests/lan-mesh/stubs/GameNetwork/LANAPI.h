#pragma once
#include "GameNetwork/LANGameInfo.h"

class LANAPIInterface
{
public:
	enum ChatType
	{
		LANCHAT_NORMAL = 0,
		LANCHAT_EMOTE,
		LANCHAT_SYSTEM,
	};
};

class LANAPI : public LANAPIInterface
{
public:
	UnsignedInt m_localIP = 0;
	LANGameInfo* m_currentGame = nullptr;
	std::vector<std::wstring> m_chat;

	LANGameInfo* GetMyGame() { return m_currentGame; }
	UnsignedInt GetLocalIP() { return m_localIP; }
	void OnChat(UnicodeString player, UnsignedInt ip, UnicodeString message, ChatType format)
	{
		m_chat.push_back(message.str());
		printf("[%u.%u.%u.%u CHAT] %ls\n", (m_localIP >> 24) & 0xff, (m_localIP >> 16) & 0xff, (m_localIP >> 8) & 0xff, m_localIP & 0xff, message.str());
		fflush(stdout);
	}
};
