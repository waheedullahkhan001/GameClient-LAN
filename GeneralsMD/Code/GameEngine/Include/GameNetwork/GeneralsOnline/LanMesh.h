#pragma once

// GO_LAN: LAN games use the same Valve GameNetworkingSockets connections as online games.
// Peers connect straight to each other by IP address, so no server, signalling, STUN or TURN is involved.
// The mesh lives while the local player sits in a LAN game room and while that game is running,
// so connections (and latency data) are ready before the match starts, like the online lobby.

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/Transport.h"
#include <steam/steamnetworkingtypes.h>
#include <deque>
#include <map>
#include <vector>

class LANGameInfo;

class LanMesh
{
public:
	// Same UDP port the legacy LAN game traffic used, so existing firewall rules keep working.
	static constexpr UnsignedShort PORT = 8088;

	// Bumped whenever the LAN mesh wire format changes, so mismatched builds refuse each other.
	static constexpr UnsignedShort PROTOCOL_VERSION = 1;

	enum SendResult
	{
		SEND_OK,
		SEND_RETRY,   // the connection's send queue is full, try again next frame
		SEND_DROPPED, // no usable connection to that player
	};

	// Creates or destroys the mesh to follow TheLAN's current game, then services it. Call once per frame.
	static void TickGlobal();
	// Closes everything, for when the game exits.
	static void ShutdownGlobal();
	static LanMesh* Get() { return s_pInstance; }

	// Applies the network tuning the Generals Online service hands to online games (its default config).
	static void ApplyNetworkTuning();

	// Host side: true when every human in the game is connected to every other human and all builds match.
	// Otherwise fills outProblems with lines for the lobby chat.
	Bool isReadyToStart(LANGameInfo* game, std::vector<UnicodeString>& outProblems);

	// Latency helpers, all in milliseconds (round trip, as reported by GameNetworkingSockets).
	Int getMaximumLatency() const;
	Int getMaximumHistoricalLatency() const;
	Int getAverageLatency() const;
	Int getNumConnectedPeers() const;
	Int getNumPeers() const { return (Int)m_peers.size(); }

	struct PeerConnection
	{
		UnsignedInt slotIP;
		UnicodeString name;
		HSteamNetConnection connection;
	};
	// The connection in use to each reachable player, for the network stats overlay.
	void getPeerConnections(std::vector<PeerConnection>& out) const;

	// Used by LanMeshTransport.
	SendResult sendGamePacket(UnsignedInt slotIP, const TransportMessageHeader& header, const UnsignedByte* payload, UnsignedInt payloadLen);
	Bool popGamePacket(UnsignedInt& outSlotIP, std::vector<UnsignedByte>& outData);
	void pump();

	static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

private:
	LanMesh();
	~LanMesh();

	struct Peer
	{
		UnsignedInt slotIP = 0;
		UnicodeString name;
		HSteamNetConnection outgoing = k_HSteamNetConnection_Invalid; // we dialled them
		HSteamNetConnection incoming = k_HSteamNetConnection_Invalid; // they dialled us
		Bool outgoingVerified = FALSE;
		Bool incomingVerified = FALSE;
		Bool incompatible = FALSE;
		Bool announcedConnected = FALSE;
		Bool announcedIncompatible = FALSE;
		Bool announcedUnreachable = FALSE;
		UnsignedInt joinedTime = 0;
		UnsignedInt lastDialTime = 0;
		// Dials in a row that never got through; spaces out retries to a player who has gone.
		Int dialFailures = 0;
		// Random id of the peer's running game, so we notice when they restart and drop the dead connections.
		UnsignedInt instance = 0;
		// Slot IPs this peer reports being connected to, used by the host for the full mesh check.
		std::vector<UnsignedInt> reportedConnections;
		UnsignedInt lastStatusTime = 0;
		std::vector<Int> latencyHistory;
	};

	struct HelloInfo
	{
		UnsignedInt slotIP = 0;
		UnsignedInt instance = 0;
		Bool compatible = FALSE;
	};

	// A connection that dialled us but that we cannot match to a player in the room yet.
	struct Unidentified
	{
		UnsignedInt since = 0;
		Bool hasHello = FALSE;
		HelloInfo hello;
	};

	struct QueuedPacket
	{
		UnsignedInt slotIP;
		std::vector<UnsignedByte> data;
	};

	Bool start();
	void tick();
	void forgetAllConnections();
	void reconcilePeers(LANGameInfo* game, Bool gameRunning);
	void attachPendingHellos();
	void dialPeers();
	void receiveAll();
	void receiveOn(HSteamNetConnection conn);
	void handleHello(HSteamNetConnection conn, const UnsignedByte* data, UnsignedInt len);
	void attachConnection(HSteamNetConnection conn, const HelloInfo& hello);
	void handleStatus(HSteamNetConnection conn, const UnsignedByte* data, UnsignedInt len);
	void onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
	void onConnectionLost(HSteamNetConnection conn);
	void sendHello(HSteamNetConnection conn);
	void broadcastStatus();
	void sampleLatency();
	void announceChanges();
	void closeConnection(HSteamNetConnection conn, const char* reason);
	void closePeer(Peer& peer);
	void systemMessage(const UnicodeString& text);

	Bool isLongUnreachable(const Peer& peer) const;
	Bool handlesValid() const;
	Peer* findPeer(UnsignedInt slotIP);
	Peer* findPeerByConnection(HSteamNetConnection conn);
	HSteamNetConnection activeConnection(const Peer& peer) const;
	Bool isPeerUsable(const Peer& peer) const { return activeConnection(peer) != k_HSteamNetConnection_Invalid; }

	static LanMesh* s_pInstance;

	UnsignedInt m_localIP = 0;
	UnsignedInt m_instance = 0;
	// Library generation our handles belong to; the online services can restart the library.
	UnsignedInt m_libraryGeneration = 0;
	HSteamListenSocket m_listenSocket = k_HSteamListenSocket_Invalid;
	std::map<UnsignedInt, Peer> m_peers;
	std::map<HSteamNetConnection, Unidentified> m_unidentified;
	std::deque<QueuedPacket> m_gamePackets;
	std::vector<UnsignedByte> m_sendBuffer;
	UnsignedInt m_lastStartAttempt = 0;
	UnsignedInt m_lastStatusSent = 0;
	UnsignedInt m_lastLatencySample = 0;
	Bool m_wasGameRunning = FALSE;
	Bool m_announcedPortError = FALSE;
};

// Transport used by ConnectionManager for LAN games. It replaces the legacy UDPTransport.
class LanMeshTransport : public Transport
{
public:
	LanMeshTransport();
	virtual ~LanMeshTransport() override;

	virtual Bool init(AsciiString ip, UnsignedShort port) override;
	virtual Bool init(UnsignedInt ip, UnsignedShort port) override;
	virtual void reset(void) override;
	virtual Bool update(void) override;

	virtual Bool doRecv(void) override;
	virtual Bool doSend(void) override;

	virtual Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len) override;

	virtual Bool allowBroadcasts(Bool val) override { return FALSE; }

private:
	void advanceStatistics();

	std::vector<UnsignedByte> m_recvScratch;
	std::vector<UnsignedInt> m_blockedPeers;
};
