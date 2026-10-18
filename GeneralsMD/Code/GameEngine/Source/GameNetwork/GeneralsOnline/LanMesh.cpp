#include "PreRTS.h" // This must go first in EVERY cpp file int the GameEngine

#include "Common/CRC.h"
#include "Common/GlobalData.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/LanMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NetworkMesh.h"

#include <algorithm>
#include <random>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

LanMesh* LanMesh::s_pInstance = nullptr;

namespace
{
	// First byte of every message on a LAN mesh connection.
	enum LanMeshMessageType : UnsignedByte
	{
		LANMESH_MSG_GAME = 0x47,   // 'G' game packet: TransportMessageHeader followed by its payload
		LANMESH_MSG_HELLO = 0x48,  // 'H' who we are and which build we run
		LANMESH_MSG_STATUS = 0x53, // 'S' which slots we are connected to, for the host's full mesh check
	};

	constexpr UnsignedInt LANMESH_MAGIC = 0x4E4C4F47; // "GOLN"

#pragma pack(push, 1)
	struct LanMeshHello
	{
		UnsignedByte type;
		UnsignedInt magic;
		UnsignedShort protocol;
		UnsignedInt slotIP;
		UnsignedInt exeCRC;
		UnsignedInt iniCRC;
		UnsignedShort logicHz;
		UnsignedInt instance;
	};

	struct LanMeshStatus
	{
		UnsignedByte type;
		UnsignedInt magic;
		UnsignedByte count;
		UnsignedInt connected[MAX_SLOTS];
	};
#pragma pack(pop)

	constexpr Int DIAL_RETRY_MS = 2000;
	constexpr Int DIAL_RETRY_MAX_MS = 30000;
	constexpr Int DIAL_RETRY_WHEN_CONNECTED_MS = 15000;
	constexpr Int START_RETRY_MS = 2000;
	constexpr Int UNIDENTIFIED_TIMEOUT_MS = 10000;
	// A player still unreachable after this long most likely runs another build or is blocked by a firewall.
	constexpr Int UNREACHABLE_HINT_MS = 10000;
	constexpr Int STATUS_INTERVAL_MS = 1000;
	constexpr Int STATUS_STALE_MS = 5000;
	constexpr Int LATENCY_SAMPLE_INTERVAL_MS = 500;
	// 40 samples at 500 ms = 20 seconds, the same window as the online service's network_mesh_histogram_duration.
	constexpr size_t LATENCY_HISTORY_LENGTH = 40;
	constexpr size_t MAX_QUEUED_GAME_PACKETS = 8192;
	constexpr Int MAX_MESSAGES_PER_RECEIVE = 64;

	// The library's default is a fixed 256 KB/s. A range turns on its bandwidth estimate, so custom map
	// transfers can use a real LAN while a slow virtual LAN still gets measured down.
	constexpr int SEND_RATE_MIN = 256 * 1024;
	constexpr int SEND_RATE_MAX = 4 * 1024 * 1024;

	// Online games send reliable and flush every frame; NoNagle gives the same immediate send per packet.
	constexpr int GAME_SEND_FLAGS = k_nSteamNetworkingSend_ReliableNoNagle;

	constexpr int NUM_CONNECTION_OPTIONS = 4;

	void fillConnectionOptions(SteamNetworkingConfigValue_t* opts)
	{
		// A per connection callback keeps LAN connections away from the online mesh's global handler.
		opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)&LanMesh::OnConnectionStatusChanged);
		// Plain IP connections between LAN players, no certificates involved.
		opts[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
		opts[2].SetInt32(k_ESteamNetworkingConfig_SendRateMin, SEND_RATE_MIN);
		opts[3].SetInt32(k_ESteamNetworkingConfig_SendRateMax, SEND_RATE_MAX);
	}

	ISteamNetworkingSockets* Sockets()
	{
		return NetworkMeshLibrary::IsInitialized() ? SteamNetworkingSockets() : nullptr;
	}

	// Milliseconds since an earlier timeGetTime() value. Signed, so a value taken a moment later reads as zero or less.
	Int msSince(UnsignedInt then)
	{
		return (Int)(timeGetTime() - then);
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::TickGlobal()
{
	LANGameInfo* game = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;

	// Keep the mesh while we sit in a LAN game room and while that match runs.
	// Once the match is over (score screen) it is no longer needed.
	const Bool inRoom = (game != nullptr) && !game->isGameInProgress();
	const Bool inMatch = (s_pInstance != nullptr) && (TheLAN != nullptr) && (TheNetwork != nullptr);
	if (!inRoom && !inMatch)
	{
		ShutdownGlobal();
		return;
	}

	if (s_pInstance == nullptr)
	{
		s_pInstance = new LanMesh();
	}

	s_pInstance->tick();
}

void LanMesh::ShutdownGlobal()
{
	if (s_pInstance != nullptr)
	{
		delete s_pInstance;
		s_pInstance = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::ApplyNetworkTuning()
{
	// Values from the Generals Online service config (use_default_config = true, ra_slack_override_percent_in_default = 30),
	// applied the same way NGMPGame::launchGame applies them to online games.
	TheWritableGlobalData->m_networkFPSHistoryLength = 30;
	TheWritableGlobalData->m_networkLatencyHistoryLength = 200;
	TheWritableGlobalData->m_networkRunAheadSlack = 30;
	TheWritableGlobalData->m_networkRunAheadMetricsTime = 5000;
}

//-------------------------------------------------------------------------------------------------
LanMesh::LanMesh()
{
	m_localIP = (TheLAN != nullptr) ? TheLAN->GetLocalIP() : 0;

	std::random_device random;
	m_instance = random() | 1;

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Created, local IP %d.%d.%d.%d, instance %08X", PRINTF_IP_AS_4_INTS(m_localIP), m_instance);
	start();
}

LanMesh::~LanMesh()
{
	if (Sockets() != nullptr && NetworkMeshLibrary::GetGeneration() == m_libraryGeneration)
	{
		for (auto& kv : m_peers)
		{
			closePeer(kv.second);
		}
		for (auto& kv : m_unidentified)
		{
			closeConnection(kv.first, "LAN game closed");
		}
		if (m_listenSocket != k_HSteamListenSocket_Invalid)
		{
			Sockets()->CloseListenSocket(m_listenSocket);
		}
	}
	m_peers.clear();
	m_unidentified.clear();
	m_listenSocket = k_HSteamListenSocket_Invalid;

	NetworkMeshLibrary::ReleaseLAN();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Destroyed");
}

//-------------------------------------------------------------------------------------------------
Bool LanMesh::start()
{
	m_lastStartAttempt = timeGetTime();

	if (!NetworkMeshLibrary::EnsureInitializedForLAN(m_localIP))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] GameNetworkingSockets failed to initialize");
		return FALSE;
	}
	m_libraryGeneration = NetworkMeshLibrary::GetGeneration();

	if (m_listenSocket != k_HSteamListenSocket_Invalid)
	{
		return TRUE;
	}

	// Listen on every network adapter, so a PC with several adapters still accepts its peers.
	SteamNetworkingIPAddr localAddress;
	localAddress.Clear();
	localAddress.m_port = PORT;

	SteamNetworkingConfigValue_t opts[NUM_CONNECTION_OPTIONS];
	fillConnectionOptions(opts);
	m_listenSocket = Sockets()->CreateListenSocketIP(localAddress, NUM_CONNECTION_OPTIONS, opts);

	if (m_listenSocket == k_HSteamListenSocket_Invalid)
	{
		// The dual stack IPv6 socket can fail on PCs with IPv6 turned off, so try plain IPv4 too.
		localAddress.SetIPv4(0, PORT);
		m_listenSocket = Sockets()->CreateListenSocketIP(localAddress, NUM_CONNECTION_OPTIONS, opts);
	}

	if (m_listenSocket == k_HSteamListenSocket_Invalid)
	{
		// We can still dial the others, so this is only fatal if they cannot dial us either.
		NetworkLog(m_announcedPortError ? ELogVerbosity::LOG_DEBUG : ELogVerbosity::LOG_RELEASE, "[LAN MESH] Could not listen on UDP port %d", PORT);
		if (!m_announcedPortError)
		{
			m_announcedPortError = TRUE;
			UnicodeString text;
			text.format(L"Network warning: UDP port %d is busy on this PC. Close other copies of the game.", PORT);
			systemMessage(text);
		}
		return FALSE;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Listening on UDP port %d", PORT);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void LanMesh::tick()
{
	// If the online services restarted the library, every handle we hold died with it.
	if (m_libraryGeneration != 0 && (!NetworkMeshLibrary::IsInitialized() || NetworkMeshLibrary::GetGeneration() != m_libraryGeneration))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] GameNetworkingSockets was restarted, reconnecting");
		forgetAllConnections();
	}

	const Bool needsStart = !NetworkMeshLibrary::IsInitialized() || m_libraryGeneration == 0 || m_listenSocket == k_HSteamListenSocket_Invalid;
	if (needsStart && msSince(m_lastStartAttempt) >= START_RETRY_MS)
	{
		start();
	}

	if (Sockets() == nullptr || m_libraryGeneration == 0)
	{
		return;
	}

	LANGameInfo* game = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;
	const Bool gameRunning = (TheNetwork != nullptr);

	// Drop anything left over from a match that just ended so it can never leak into the next one.
	if (m_wasGameRunning && !gameRunning)
	{
		m_gamePackets.clear();
	}
	m_wasGameRunning = gameRunning;

	// Learn about room changes first, so a player who just joined is known when their connection arrives.
	reconcilePeers(game, gameRunning);
	pump();
	attachPendingHellos();
	dialPeers();

	// Connections that never said who they are get dropped.
	std::vector<HSteamNetConnection> expired;
	for (auto& kv : m_unidentified)
	{
		if (msSince(kv.second.since) > UNIDENTIFIED_TIMEOUT_MS)
		{
			expired.push_back(kv.first);
		}
	}
	for (HSteamNetConnection conn : expired)
	{
		m_unidentified.erase(conn);
		closeConnection(conn, "No hello");
	}

	if (msSince(m_lastStatusSent) >= STATUS_INTERVAL_MS)
	{
		m_lastStatusSent = timeGetTime();
		broadcastStatus();
	}

	if (msSince(m_lastLatencySample) >= LATENCY_SAMPLE_INTERVAL_MS)
	{
		m_lastLatencySample = timeGetTime();
		sampleLatency();
	}

	if (!gameRunning)
	{
		announceChanges();
	}
}

void LanMesh::forgetAllConnections()
{
	for (auto& kv : m_peers)
	{
		Peer& peer = kv.second;
		peer.outgoing = k_HSteamNetConnection_Invalid;
		peer.incoming = k_HSteamNetConnection_Invalid;
		peer.outgoingVerified = FALSE;
		peer.incomingVerified = FALSE;
		peer.lastDialTime = 0;
	}
	m_unidentified.clear();
	m_listenSocket = k_HSteamListenSocket_Invalid;
	m_libraryGeneration = 0;
}

// Lobby feedback, like the connection indicators in the online lobby.
void LanMesh::announceChanges()
{
	for (auto& kv : m_peers)
	{
		Peer& peer = kv.second;
		const Bool usable = isPeerUsable(peer);
		if (usable && !peer.announcedConnected)
		{
			peer.announcedConnected = TRUE;
			UnicodeString text;
			text.format(L"Connected to %ls.", peer.name.str());
			systemMessage(text);
		}
		else if (!usable && peer.announcedConnected)
		{
			peer.announcedConnected = FALSE;
			UnicodeString text;
			text.format(L"Lost connection to %ls, reconnecting...", peer.name.str());
			systemMessage(text);
		}

		if (isLongUnreachable(peer) && !peer.announcedUnreachable)
		{
			peer.announcedUnreachable = TRUE;
			UnicodeString text;
			text.format(L"Cannot reach %ls. Everyone needs the same GeneralsOnlineZH_LAN.exe, allowed through Windows Firewall.", peer.name.str());
			systemMessage(text);
		}

		if (peer.incompatible && !peer.announcedIncompatible)
		{
			peer.announcedIncompatible = TRUE;
			UnicodeString text;
			text.format(L"%ls is running a different game version. Everyone must use the same game files.", peer.name.str());
			systemMessage(text);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::pump()
{
	if (!handlesValid())
	{
		return;
	}

	Sockets()->RunCallbacks();
	receiveAll();
}

//-------------------------------------------------------------------------------------------------
void LanMesh::reconcilePeers(LANGameInfo* game, Bool gameRunning)
{
	std::vector<UnsignedInt> present;

	if (game != nullptr)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const LANGameSlot* slot = game->getConstLANSlot(i);
			if (slot == nullptr || !slot->isHuman())
			{
				continue;
			}

			const UnsignedInt ip = slot->getIP();
			if (ip == 0 || ip == m_localIP)
			{
				continue;
			}

			Peer& peer = m_peers[ip];
			if (peer.slotIP == 0)
			{
				peer.slotIP = ip;
				peer.joinedTime = timeGetTime();
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Peer %d.%d.%d.%d joined the room", PRINTF_IP_AS_4_INTS(ip));
			}
			peer.name = slot->getName();
			present.push_back(ip);
		}
	}

	// While the match runs the game handles players leaving itself, so only prune in the lobby.
	if (gameRunning)
	{
		return;
	}

	for (auto it = m_peers.begin(); it != m_peers.end(); )
	{
		if (std::find(present.begin(), present.end(), it->first) == present.end())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Peer %d.%d.%d.%d left the room", PRINTF_IP_AS_4_INTS(it->first));
			closePeer(it->second);
			it = m_peers.erase(it);
		}
		else
		{
			++it;
		}
	}
}

// Hellos can arrive a moment before the room lists their sender; match them up once it does.
void LanMesh::attachPendingHellos()
{
	std::vector<std::pair<HSteamNetConnection, HelloInfo>> ready;
	for (auto& kv : m_unidentified)
	{
		if (kv.second.hasHello && findPeer(kv.second.hello.slotIP) != nullptr)
		{
			ready.push_back(std::make_pair(kv.first, kv.second.hello));
		}
	}

	for (auto& entry : ready)
	{
		attachConnection(entry.first, entry.second);
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::dialPeers()
{
	ISteamNetworkingSockets* sockets = Sockets();

	// Both sides dial each other. Whichever direction gets through is used, which copes with a firewall
	// that only allows one direction. activeConnection() makes both sides agree when both get through.
	for (auto& kv : m_peers)
	{
		Peer& peer = kv.second;
		if (peer.outgoing != k_HSteamNetConnection_Invalid)
		{
			continue;
		}

		const Int backoff = std::min(DIAL_RETRY_MS << std::min<Int>(peer.dialFailures, 4), DIAL_RETRY_MAX_MS);
		const Int retry = isPeerUsable(peer) ? DIAL_RETRY_WHEN_CONNECTED_MS : backoff;
		if (peer.lastDialTime != 0 && msSince(peer.lastDialTime) < retry)
		{
			continue;
		}
		peer.lastDialTime = timeGetTime();

		SteamNetworkingIPAddr address;
		address.Clear();
		address.SetIPv4(peer.slotIP, PORT);

		SteamNetworkingConfigValue_t opts[NUM_CONNECTION_OPTIONS];
		fillConnectionOptions(opts);
		peer.outgoing = sockets->ConnectByIPAddress(address, NUM_CONNECTION_OPTIONS, opts);
		peer.outgoingVerified = FALSE;

		NetworkLog(ELogVerbosity::LOG_DEBUG, "[LAN MESH] Dialling %d.%d.%d.%d:%d", PRINTF_IP_AS_4_INTS(peer.slotIP), PORT);
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
	if (info == nullptr)
	{
		return;
	}

	if (s_pInstance != nullptr)
	{
		s_pInstance->onStatusChanged(info);
	}
	else if (Sockets() != nullptr)
	{
		Sockets()->CloseConnection(info->m_hConn, 0, nullptr, false);
	}
}

void LanMesh::onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
	const HSteamNetConnection conn = info->m_hConn;

	switch (info->m_info.m_eState)
	{
		case k_ESteamNetworkingConnectionState_Connecting:
		{
			// Someone dialled our listen socket. We learn who it is from their hello.
			if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid && info->m_eOldState == k_ESteamNetworkingConnectionState_None)
			{
				if (Sockets()->AcceptConnection(conn) != k_EResultOK)
				{
					closeConnection(conn, "Accept failed");
					break;
				}

				Unidentified entry;
				entry.since = timeGetTime();
				m_unidentified[conn] = entry;

				char addr[SteamNetworkingIPAddr::k_cchMaxString];
				info->m_info.m_addrRemote.ToString(addr, sizeof(addr), true);
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Incoming connection from %s", addr);
			}
			break;
		}

		case k_ESteamNetworkingConnectionState_Connected:
		{
			sendHello(conn);
			break;
		}

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Connection %u closed (%d): %s", conn, info->m_info.m_eEndReason, info->m_info.m_szEndDebug);
			onConnectionLost(conn);
			closeConnection(conn, nullptr);
			break;
		}

		default:
			break;
	}
}

void LanMesh::onConnectionLost(HSteamNetConnection conn)
{
	m_unidentified.erase(conn);

	Peer* peer = findPeerByConnection(conn);
	if (peer == nullptr)
	{
		return;
	}

	if (peer->outgoing == conn)
	{
		if (!peer->outgoingVerified)
		{
			++peer->dialFailures;
		}
		peer->outgoing = k_HSteamNetConnection_Invalid;
		peer->outgoingVerified = FALSE;
	}
	if (peer->incoming == conn)
	{
		peer->incoming = k_HSteamNetConnection_Invalid;
		peer->incomingVerified = FALSE;
	}
}

//-------------------------------------------------------------------------------------------------
void LanMesh::sendHello(HSteamNetConnection conn)
{
	LanMeshHello hello;
	hello.type = LANMESH_MSG_HELLO;
	hello.magic = LANMESH_MAGIC;
	hello.protocol = PROTOCOL_VERSION;
	hello.slotIP = m_localIP;
	hello.exeCRC = TheGlobalData->m_exeCRC;
	hello.iniCRC = TheGlobalData->m_iniCRC;
	hello.logicHz = GENERALS_ONLINE_HIGH_FPS_LIMIT;
	hello.instance = m_instance;

	Sockets()->SendMessageToConnection(conn, &hello, sizeof(hello), k_nSteamNetworkingSend_Reliable, nullptr);
}

void LanMesh::handleHello(HSteamNetConnection conn, const UnsignedByte* data, UnsignedInt len)
{
	if (len < sizeof(LanMeshHello))
	{
		return;
	}

	LanMeshHello hello;
	memcpy(&hello, data, sizeof(hello));
	if (hello.magic != LANMESH_MAGIC)
	{
		return;
	}

	HelloInfo info;
	info.slotIP = hello.slotIP;
	info.instance = hello.instance;
	info.compatible = hello.protocol == PROTOCOL_VERSION
		&& hello.exeCRC == TheGlobalData->m_exeCRC
		&& hello.iniCRC == TheGlobalData->m_iniCRC
		&& hello.logicHz == GENERALS_ONLINE_HIGH_FPS_LIMIT;

	if (!info.compatible)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE,
			"[LAN MESH] Build mismatch with %d.%d.%d.%d: protocol %d/%d exe %08X/%08X ini %08X/%08X logic %dHz/%dHz (theirs/ours)",
			PRINTF_IP_AS_4_INTS(hello.slotIP), hello.protocol, PROTOCOL_VERSION, hello.exeCRC, TheGlobalData->m_exeCRC,
			hello.iniCRC, TheGlobalData->m_iniCRC, hello.logicHz, GENERALS_ONLINE_HIGH_FPS_LIMIT);
	}

	Peer* owner = findPeerByConnection(conn);
	Peer* peer = findPeer(info.slotIP);
	if (owner != nullptr && owner != peer)
	{
		// We dialled one player and someone else answered.
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Dialled %d.%d.%d.%d but %d.%d.%d.%d answered, closing",
			PRINTF_IP_AS_4_INTS(owner->slotIP), PRINTF_IP_AS_4_INTS(info.slotIP));
		onConnectionLost(conn);
		closeConnection(conn, "Wrong player");
		return;
	}

	if (peer == nullptr)
	{
		// Not in our room yet. Keep the connection and match it up when the room update arrives.
		auto it = m_unidentified.find(conn);
		if (it != m_unidentified.end())
		{
			it->second.hasHello = TRUE;
			it->second.hello = info;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Hello from %d.%d.%d.%d who is not in the room yet", PRINTF_IP_AS_4_INTS(info.slotIP));
		}
		return;
	}

	attachConnection(conn, info);
}

void LanMesh::attachConnection(HSteamNetConnection conn, const HelloInfo& hello)
{
	Peer* peer = findPeer(hello.slotIP);
	if (peer == nullptr)
	{
		return;
	}

	// A different instance id means the player restarted the game, so connections to the old copy are dead.
	if (peer->instance != 0 && peer->instance != hello.instance)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] %d.%d.%d.%d restarted their game, dropping old connections", PRINTF_IP_AS_4_INTS(peer->slotIP));
		if (peer->outgoing != k_HSteamNetConnection_Invalid && peer->outgoing != conn)
		{
			closeConnection(peer->outgoing, "Peer restarted");
			peer->outgoing = k_HSteamNetConnection_Invalid;
			peer->outgoingVerified = FALSE;
		}
		if (peer->incoming != k_HSteamNetConnection_Invalid && peer->incoming != conn)
		{
			closeConnection(peer->incoming, "Peer restarted");
			peer->incoming = k_HSteamNetConnection_Invalid;
			peer->incomingVerified = FALSE;
		}
		peer->lastDialTime = 0;
		peer->dialFailures = 0;
		peer->reportedConnections.clear();
		peer->latencyHistory.clear();
	}
	peer->instance = hello.instance;
	peer->incompatible = !hello.compatible;

	const Bool isOutgoing = (peer->outgoing == conn);
	if (isOutgoing)
	{
		peer->outgoingVerified = hello.compatible;
		peer->dialFailures = 0;
	}
	else
	{
		m_unidentified.erase(conn);
		if (peer->incoming != k_HSteamNetConnection_Invalid && peer->incoming != conn)
		{
			closeConnection(peer->incoming, "Replaced");
		}
		peer->incoming = conn;
		peer->incomingVerified = hello.compatible;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Hello from %d.%d.%d.%d on %s connection, compatible = %d",
		PRINTF_IP_AS_4_INTS(hello.slotIP), isOutgoing ? "outgoing" : "incoming", hello.compatible);
}

//-------------------------------------------------------------------------------------------------
void LanMesh::broadcastStatus()
{
	LanMeshStatus status;
	memset(&status, 0, sizeof(status));
	status.type = LANMESH_MSG_STATUS;
	status.magic = LANMESH_MAGIC;

	for (auto& kv : m_peers)
	{
		if (isPeerUsable(kv.second) && status.count < MAX_SLOTS)
		{
			status.connected[status.count++] = kv.first;
		}
	}

	for (auto& kv : m_peers)
	{
		const HSteamNetConnection conn = activeConnection(kv.second);
		if (conn != k_HSteamNetConnection_Invalid)
		{
			Sockets()->SendMessageToConnection(conn, &status, sizeof(status), k_nSteamNetworkingSend_Reliable, nullptr);
		}
	}
}

void LanMesh::handleStatus(HSteamNetConnection conn, const UnsignedByte* data, UnsignedInt len)
{
	if (len < sizeof(LanMeshStatus))
	{
		return;
	}

	LanMeshStatus status;
	memcpy(&status, data, sizeof(status));
	if (status.magic != LANMESH_MAGIC)
	{
		return;
	}

	Peer* peer = findPeerByConnection(conn);
	if (peer == nullptr)
	{
		return;
	}

	peer->reportedConnections.assign(status.connected, status.connected + std::min<Int>(status.count, MAX_SLOTS));
	peer->lastStatusTime = timeGetTime();
}

//-------------------------------------------------------------------------------------------------
void LanMesh::receiveAll()
{
	// Copy the handles first: handling a hello can move or close connections.
	std::vector<HSteamNetConnection> conns;
	for (auto& kv : m_peers)
	{
		if (kv.second.outgoing != k_HSteamNetConnection_Invalid)
		{
			conns.push_back(kv.second.outgoing);
		}
		if (kv.second.incoming != k_HSteamNetConnection_Invalid)
		{
			conns.push_back(kv.second.incoming);
		}
	}
	for (auto& kv : m_unidentified)
	{
		conns.push_back(kv.first);
	}

	for (HSteamNetConnection conn : conns)
	{
		receiveOn(conn);
	}
}

void LanMesh::receiveOn(HSteamNetConnection conn)
{
	ISteamNetworkingSockets* sockets = Sockets();
	SteamNetworkingMessage_t* msgs[MAX_MESSAGES_PER_RECEIVE];

	for (;;)
	{
		const int count = sockets->ReceiveMessagesOnConnection(conn, msgs, MAX_MESSAGES_PER_RECEIVE);
		if (count <= 0)
		{
			return;
		}

		for (int i = 0; i < count; ++i)
		{
			SteamNetworkingMessage_t* msg = msgs[i];
			const UnsignedByte* data = static_cast<const UnsignedByte*>(msg->GetData());
			const UnsignedInt len = (UnsignedInt)msg->GetSize();

			if (len >= 1)
			{
				switch (data[0])
				{
					case LANMESH_MSG_HELLO:
						handleHello(conn, data, len);
						break;

					case LANMESH_MSG_STATUS:
						handleStatus(conn, data, len);
						break;

					case LANMESH_MSG_GAME:
					{
						Peer* peer = findPeerByConnection(conn);
						if (peer == nullptr || peer->incompatible)
						{
							NetworkLog(ELogVerbosity::LOG_DEBUG, "[LAN MESH] Dropping game packet from an unknown or incompatible connection");
							break;
						}

						if (m_gamePackets.size() >= MAX_QUEUED_GAME_PACKETS)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Game packet queue full, dropping the oldest packet");
							m_gamePackets.pop_front();
						}

						QueuedPacket queued;
						queued.slotIP = peer->slotIP;
						queued.data.assign(data + 1, data + len);
						m_gamePackets.push_back(std::move(queued));
						break;
					}

					default:
						break;
				}
			}

			msg->Release();
		}
	}
}

//-------------------------------------------------------------------------------------------------
LanMesh::SendResult LanMesh::sendGamePacket(UnsignedInt slotIP, const TransportMessageHeader& header, const UnsignedByte* payload, UnsignedInt payloadLen)
{
	ISteamNetworkingSockets* sockets = Sockets();
	Peer* peer = findPeer(slotIP);
	if (!handlesValid() || peer == nullptr)
	{
		return SEND_DROPPED;
	}

	const HSteamNetConnection conn = activeConnection(*peer);
	if (conn == k_HSteamNetConnection_Invalid)
	{
		return SEND_DROPPED;
	}

	m_sendBuffer.resize(1 + sizeof(TransportMessageHeader) + payloadLen);
	m_sendBuffer[0] = LANMESH_MSG_GAME;
	memcpy(m_sendBuffer.data() + 1, &header, sizeof(TransportMessageHeader));
	memcpy(m_sendBuffer.data() + 1 + sizeof(TransportMessageHeader), payload, payloadLen);

	const EResult result = sockets->SendMessageToConnection(conn, m_sendBuffer.data(), (uint32)m_sendBuffer.size(), GAME_SEND_FLAGS, nullptr);
	if (result == k_EResultOK)
	{
		return SEND_OK;
	}
	if (result == k_EResultLimitExceeded)
	{
		return SEND_RETRY;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Send to %d.%d.%d.%d failed (%d)", PRINTF_IP_AS_4_INTS(slotIP), (Int)result);
	return SEND_DROPPED;
}

Bool LanMesh::popGamePacket(UnsignedInt& outSlotIP, std::vector<UnsignedByte>& outData)
{
	if (m_gamePackets.empty())
	{
		return FALSE;
	}

	outSlotIP = m_gamePackets.front().slotIP;
	outData.swap(m_gamePackets.front().data);
	m_gamePackets.pop_front();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void LanMesh::sampleLatency()
{
	ISteamNetworkingSockets* sockets = Sockets();
	for (auto& kv : m_peers)
	{
		Peer& peer = kv.second;
		const HSteamNetConnection conn = activeConnection(peer);
		if (conn == k_HSteamNetConnection_Invalid)
		{
			continue;
		}

		SteamNetConnectionRealTimeStatus_t status;
		if (sockets->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK || status.m_nPing < 0)
		{
			continue;
		}

		peer.latencyHistory.push_back(status.m_nPing);
		if (peer.latencyHistory.size() > LATENCY_HISTORY_LENGTH)
		{
			peer.latencyHistory.erase(peer.latencyHistory.begin());
		}
	}
}

Int LanMesh::getMaximumLatency() const
{
	Int highest = 0;
	for (const auto& kv : m_peers)
	{
		if (isPeerUsable(kv.second) && !kv.second.latencyHistory.empty())
		{
			highest = std::max(highest, kv.second.latencyHistory.back());
		}
	}
	return highest;
}

Int LanMesh::getMaximumHistoricalLatency() const
{
	Int highest = 0;
	for (const auto& kv : m_peers)
	{
		for (Int latency : kv.second.latencyHistory)
		{
			highest = std::max(highest, latency);
		}
	}
	return highest;
}

Int LanMesh::getAverageLatency() const
{
	Int total = 0;
	Int count = 0;
	for (const auto& kv : m_peers)
	{
		if (isPeerUsable(kv.second) && !kv.second.latencyHistory.empty())
		{
			total += kv.second.latencyHistory.back();
			++count;
		}
	}
	return (count > 0) ? total / count : 0;
}

void LanMesh::getPeerConnections(std::vector<PeerConnection>& out) const
{
	out.clear();
	if (!handlesValid())
	{
		return;
	}

	for (const auto& kv : m_peers)
	{
		const HSteamNetConnection conn = activeConnection(kv.second);
		if (conn != k_HSteamNetConnection_Invalid)
		{
			PeerConnection entry;
			entry.slotIP = kv.first;
			entry.name = kv.second.name;
			entry.connection = conn;
			out.push_back(entry);
		}
	}
}

Int LanMesh::getNumConnectedPeers() const
{
	Int count = 0;
	for (const auto& kv : m_peers)
	{
		if (isPeerUsable(kv.second))
		{
			++count;
		}
	}
	return count;
}

//-------------------------------------------------------------------------------------------------
Bool LanMesh::isReadyToStart(LANGameInfo* game, std::vector<UnicodeString>& outProblems)
{
	std::vector<const LANGameSlot*> humans;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot* slot = game->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() != 0)
		{
			humans.push_back(slot);
		}
	}

	// Nobody to connect to, for example a game against the AI.
	if (humans.size() <= 1)
	{
		return TRUE;
	}

	if (!handlesValid())
	{
		outProblems.push_back(UnicodeString(L"Cannot start: the LAN connection service could not start on this PC."));
		return FALSE;
	}

	// Refresh our view before judging it.
	pump();

	for (size_t a = 0; a < humans.size(); ++a)
	{
		for (size_t b = a + 1; b < humans.size(); ++b)
		{
			const UnsignedInt ipA = humans[a]->getIP();
			const UnsignedInt ipB = humans[b]->getIP();
			UnicodeString text;

			if (ipA == m_localIP || ipB == m_localIP)
			{
				const LANGameSlot* other = (ipA == m_localIP) ? humans[b] : humans[a];
				Peer* peer = findPeer(other->getIP());
				if (peer != nullptr && peer->incompatible)
				{
					text.format(L"Cannot start: %ls is running a different game version.", other->getName().str());
					outProblems.push_back(text);
				}
				else if (peer != nullptr && isLongUnreachable(*peer))
				{
					text.format(L"Cannot start: cannot reach %ls. Is %ls running the same LAN build, allowed through Windows Firewall?",
						other->getName().str(), other->getName().str());
					outProblems.push_back(text);
				}
				else if (peer == nullptr || !isPeerUsable(*peer))
				{
					text.format(L"Cannot start: still connecting to %ls.", other->getName().str());
					outProblems.push_back(text);
				}
				continue;
			}

			// Two other players: trust what either of them last reported.
			Bool linked = FALSE;
			Peer* peerA = findPeer(ipA);
			Peer* peerB = findPeer(ipB);
			if (peerA != nullptr && peerA->lastStatusTime != 0 && msSince(peerA->lastStatusTime) < STATUS_STALE_MS)
			{
				linked |= std::find(peerA->reportedConnections.begin(), peerA->reportedConnections.end(), ipB) != peerA->reportedConnections.end();
			}
			if (peerB != nullptr && peerB->lastStatusTime != 0 && msSince(peerB->lastStatusTime) < STATUS_STALE_MS)
			{
				linked |= std::find(peerB->reportedConnections.begin(), peerB->reportedConnections.end(), ipA) != peerB->reportedConnections.end();
			}

			if (!linked)
			{
				text.format(L"Cannot start: %ls and %ls are not connected to each other yet.", humans[a]->getName().str(), humans[b]->getName().str());
				outProblems.push_back(text);
			}
		}
	}

	return outProblems.empty();
}

//-------------------------------------------------------------------------------------------------
Bool LanMesh::isLongUnreachable(const Peer& peer) const
{
	// Never connected at all since joining; a peer that connected and then dropped gets the "Lost connection" line instead.
	return !peer.announcedConnected && !peer.incompatible && peer.instance == 0
		&& !isPeerUsable(peer) && peer.joinedTime != 0 && msSince(peer.joinedTime) > UNREACHABLE_HINT_MS;
}

// False when the library restarted under us and our handles are not refreshed yet.
Bool LanMesh::handlesValid() const
{
	return m_libraryGeneration != 0 && NetworkMeshLibrary::IsInitialized() && NetworkMeshLibrary::GetGeneration() == m_libraryGeneration;
}

LanMesh::Peer* LanMesh::findPeer(UnsignedInt slotIP)
{
	auto it = m_peers.find(slotIP);
	return (it != m_peers.end()) ? &it->second : nullptr;
}

LanMesh::Peer* LanMesh::findPeerByConnection(HSteamNetConnection conn)
{
	for (auto& kv : m_peers)
	{
		if (kv.second.outgoing == conn || kv.second.incoming == conn)
		{
			return &kv.second;
		}
	}
	return nullptr;
}

HSteamNetConnection LanMesh::activeConnection(const Peer& peer) const
{
	if (peer.incompatible)
	{
		return k_HSteamNetConnection_Invalid;
	}

	const Bool outgoingOk = peer.outgoing != k_HSteamNetConnection_Invalid && peer.outgoingVerified;
	const Bool incomingOk = peer.incoming != k_HSteamNetConnection_Invalid && peer.incomingVerified;

	// Both sides prefer the connection dialled by the lower IP, so they agree when both directions got through.
	if (m_localIP < peer.slotIP)
	{
		return outgoingOk ? peer.outgoing : (incomingOk ? peer.incoming : k_HSteamNetConnection_Invalid);
	}
	return incomingOk ? peer.incoming : (outgoingOk ? peer.outgoing : k_HSteamNetConnection_Invalid);
}

void LanMesh::closeConnection(HSteamNetConnection conn, const char* reason)
{
	if (conn != k_HSteamNetConnection_Invalid && Sockets() != nullptr)
	{
		Sockets()->CloseConnection(conn, 0, reason, false);
	}
}

void LanMesh::closePeer(Peer& peer)
{
	closeConnection(peer.outgoing, "Left LAN game");
	closeConnection(peer.incoming, "Left LAN game");
	peer.outgoing = k_HSteamNetConnection_Invalid;
	peer.incoming = k_HSteamNetConnection_Invalid;
	peer.outgoingVerified = FALSE;
	peer.incomingVerified = FALSE;
}

void LanMesh::systemMessage(const UnicodeString& text)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] %ls", text.str());
	if (TheLAN != nullptr)
	{
		TheLAN->OnChat(UnicodeString(L"SYSTEM"), TheLAN->GetLocalIP(), text, LANAPIInterface::LANCHAT_SYSTEM);
	}
}

//=================================================================================================
// LanMeshTransport
//=================================================================================================
LanMeshTransport::LanMeshTransport()
{
	m_useLatency = FALSE;
	m_usePacketLoss = FALSE;
	reset();
}

LanMeshTransport::~LanMeshTransport()
{
}

Bool LanMeshTransport::init(AsciiString ip, UnsignedShort port)
{
	return init((UnsignedInt)0, port);
}

Bool LanMeshTransport::init(UnsignedInt ip, UnsignedShort port)
{
	reset();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Game transport started, mesh %s", LanMesh::Get() != nullptr ? "ready" : "MISSING");
	return LanMesh::Get() != nullptr;
}

void LanMeshTransport::reset(void)
{
	for (Int i = 0; i < MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (Int i = 0; i < MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();
}

Bool LanMeshTransport::update(void)
{
	Bool retval = TRUE;
	if (doRecv() == FALSE)
	{
		retval = FALSE;
	}
	if (doSend() == FALSE)
	{
		retval = FALSE;
	}
	return retval;
}

void LanMeshTransport::advanceStatistics()
{
	const UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}
}

Bool LanMeshTransport::doRecv(void)
{
	advanceStatistics();

	LanMesh* mesh = LanMesh::Get();
	if (mesh == nullptr)
	{
		return FALSE;
	}

	mesh->pump();

	// ConnectionManager::doRelay reads m_inBuffer from the start and stops at the first empty slot,
	// so fill the empty slots in order. Whatever does not fit stays queued for the next update.
	UnsignedInt fromIP = 0;
	for (Int slot = 0; slot < MAX_MESSAGES; )
	{
		TransportMessage& message = m_inBuffer[slot];
		if (message.length != 0)
		{
			++slot;
			continue;
		}

		if (!mesh->popGamePacket(fromIP, m_recvScratch))
		{
			break;
		}

		const UnsignedInt size = (UnsignedInt)m_recvScratch.size();
		if (size <= sizeof(TransportMessageHeader) || size - sizeof(TransportMessageHeader) > sizeof(message.data))
		{
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += size;
			continue;
		}

		memcpy(&message.header, m_recvScratch.data(), sizeof(TransportMessageHeader));
		memcpy(message.data, m_recvScratch.data() + sizeof(TransportMessageHeader), size - sizeof(TransportMessageHeader));
		message.length = (Int)(size - sizeof(TransportMessageHeader));
		message.addr = fromIP;
		message.port = LanMesh::PORT;

		if (!isGeneralsPacket(&message))
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Dropping a game packet that failed its CRC or magic check");
			message.length = 0;
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += size;
			continue;
		}

		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += size;
		++slot;
	}

	return TRUE;
}

Bool LanMeshTransport::doSend(void)
{
	advanceStatistics();

	LanMesh* mesh = LanMesh::Get();
	if (mesh == nullptr)
	{
		return FALSE;
	}

	// Once a player's send queue is full, hold the rest of their packets too, so they keep their order.
	m_blockedPeers.clear();

	Bool retval = TRUE;
	for (Int i = 0; i < MAX_MESSAGES; ++i)
	{
		TransportMessage& message = m_outBuffer[i];
		if (message.length == 0)
		{
			continue;
		}

		if (std::find(m_blockedPeers.begin(), m_blockedPeers.end(), message.addr) != m_blockedPeers.end())
		{
			retval = FALSE;
			continue;
		}

		const UnsignedInt size = (UnsignedInt)message.length + sizeof(TransportMessageHeader);
		switch (mesh->sendGamePacket(message.addr, message.header, message.data, (UnsignedInt)message.length))
		{
			case LanMesh::SEND_OK:
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += size;
				message.length = 0;
				break;

			case LanMesh::SEND_RETRY:
				m_blockedPeers.push_back(message.addr);
				retval = FALSE;
				break;

			case LanMesh::SEND_DROPPED:
			default:
				// Like a lost UDP packet: the game's own resend logic will try again.
				message.length = 0;
				retval = FALSE;
				break;
		}
	}

	return retval;
}

Bool LanMeshTransport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len)
{
	if (buf == nullptr || len < 1 || len > MAX_PACKET_SIZE)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] queueSend: invalid packet length %d", len);
		return FALSE;
	}

	for (Int i = 0; i < MAX_MESSAGES; ++i)
	{
		TransportMessage& message = m_outBuffer[i];
		if (message.length != 0)
		{
			continue;
		}

		message.length = len;
		memcpy(message.data, buf, len);
		message.addr = addr;
		message.port = port;
		message.header.magic = GENERALS_MAGIC_NUMBER;

		CRC crc;
		crc.computeCRC((unsigned char*)(&(message.header.magic)), message.length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt));
		message.header.crc = crc.get();
		return TRUE;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[LAN MESH] Send queue is full, dropping a packet");
	return FALSE;
}
