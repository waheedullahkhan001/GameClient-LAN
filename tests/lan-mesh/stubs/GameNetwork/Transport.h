#pragma once
// Same members and packet check as Core/GameEngine/Include/GameNetwork/Transport.h.
#include "GameNetwork/NetworkDefs.h"
#include "Common/CRC.h"

class Transport
{
public:
	Transport() {}
	virtual ~Transport() {}

	virtual Bool init(AsciiString ip, UnsignedShort port) = 0;
	virtual Bool init(UnsignedInt ip, UnsignedShort port) = 0;
	virtual void reset(void) = 0;
	virtual Bool update(void) = 0;
	virtual Bool doRecv(void) = 0;
	virtual Bool doSend(void) = 0;
	virtual Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len) = 0;
	virtual Bool allowBroadcasts(Bool val) = 0;

	TransportMessage m_outBuffer[MAX_MESSAGES];
	TransportMessage m_inBuffer[MAX_MESSAGES];

protected:
	Bool m_useLatency;
	Bool m_usePacketLoss;
	UnsignedInt m_incomingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_incomingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	Int m_statisticsSlot;
	UnsignedInt m_lastSecond;

	Bool isGeneralsPacket(TransportMessage* msg)
	{
		if (!msg)
			return false;
		if (msg->length < 0 || msg->length > MAX_MESSAGE_LEN)
			return false;
		CRC crc;
		crc.computeCRC((unsigned char*)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt));
		if (crc.get() != msg->header.crc)
			return false;
		if (msg->header.magic != GENERALS_MAGIC_NUMBER)
			return false;
		return true;
	}
};
