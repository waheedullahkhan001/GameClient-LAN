#pragma once
// Values copied from Core/GameEngine/Include/GameNetwork/NetworkDefs.h for a GENERALS_ONLINE build.
static constexpr const Int MAX_SLOTS = 8;
static constexpr const Int MAX_UDP_PAYLOAD = 1194 - 96;
static constexpr const Int MAX_PACKET_SIZE = MAX_UDP_PAYLOAD;
static constexpr const Int MAX_MESSAGE_LEN = 1104;
static constexpr const Int MAX_MESSAGES = 256;
#define MAX_TRANSPORT_STATISTICS_SECONDS 30
static constexpr const UnsignedShort GENERALS_MAGIC_NUMBER = 0xF00D;

#pragma pack(push, 1)
struct TransportMessageHeader
{
	UnsignedInt crc;
	UnsignedShort magic;
};
#pragma pack(pop)

struct TransportMessage
{
	TransportMessageHeader header;
	UnsignedByte data[MAX_PACKET_SIZE];
	Int length;
	UnsignedInt addr;
	UnsignedShort port;
};

class NetworkInterface;
extern NetworkInterface* TheNetwork;

#define PRINTF_IP_AS_4_INTS(ip) ((ip) >> 24) & 0xff, ((ip) >> 16) & 0xff, ((ip) >> 8 ) & 0xff, (ip) & 0xff
