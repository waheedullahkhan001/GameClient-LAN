#pragma once
#include "GameNetwork/NetworkDefs.h"

class LANGameSlot
{
public:
	Bool m_human = FALSE;
	UnsignedInt m_ip = 0;
	UnicodeString m_name;

	Bool isHuman() const { return m_human; }
	UnsignedInt getIP() const { return m_ip; }
	UnicodeString getName() const { return m_name; }
};

class LANGameInfo
{
public:
	LANGameSlot m_slots[MAX_SLOTS];
	Bool m_inProgress = FALSE;

	const LANGameSlot* getConstLANSlot(Int slotNum) const { return &m_slots[slotNum]; }
	Bool isGameInProgress() const { return m_inProgress; }
};
