// Simulates one LAN player: drives the real LanMesh.cpp like the game does (TickGlobal every frame,
// the host's start check, then LanMeshTransport during the match) and verifies every game packet.
#include "PreRTS.h"
#include "Common/GlobalData.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/LanMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NetworkMesh.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <thread>

GlobalData g_globalData;
LANAPI* TheLAN = nullptr;
NetworkInterface* TheNetwork = nullptr;

static bool g_verbose = false;
static UnsignedInt g_me = 0;
static UnsignedInt g_start = 0;

void NetworkLog(ELogVerbosity logVerbosity, const char* fmt, ...)
{
	if (logVerbosity == ELogVerbosity::LOG_DEBUG && !g_verbose)
		return;
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("[%u.%u.%u.%u +%5ums] %s\n", PRINTF_IP_AS_4_INTS(g_me), timeGetTime() - g_start, buf);
	fflush(stdout);
}

//-------------------------------------------------------------------------------------------------
// The LAN part of NetworkMeshLibrary, copied from GeneralsMD/Code/GameEngine/Source/GameNetwork/GeneralsOnline/NetworkMesh.cpp
// (the real file pulls in all of the online services).
bool NetworkMeshLibrary::s_bInitialized = false;
bool NetworkMeshLibrary::s_bInitializedForLAN = false;
UnsignedInt NetworkMeshLibrary::s_generation = 0;

bool NetworkMeshLibrary::EnsureInitialized(int64_t) { return false; }

void NetworkMeshLibrary::Shutdown()
{
	if (!s_bInitialized)
		return;
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
	GameNetworkingSockets_Kill();
	s_bInitialized = false;
	s_bInitializedForLAN = false;
}

bool NetworkMeshLibrary::EnsureInitializedForLAN(UnsignedInt localIP)
{
	if (s_bInitialized)
		return true;

	char identityName[32];
	snprintf(identityName, sizeof(identityName), "lan_%08X_%08X", localIP, (UnsignedInt)timeGetTime());

	SteamNetworkingIdentity identityLocal;
	identityLocal.Clear();
	identityLocal.SetGenericString(identityName);

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(&identityLocal, errMsg))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init failed. %s", errMsg);
		return false;
	}

	s_bInitialized = true;
	s_bInitializedForLAN = true;
	++s_generation;
	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, [](ESteamNetworkingSocketsDebugOutputType, const char* pszMsg)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING LOGFUNC] %s", pszMsg);
		});
	return true;
}

void NetworkMeshLibrary::ReleaseLAN()
{
	if (s_bInitialized && s_bInitializedForLAN)
		Shutdown();
}

//-------------------------------------------------------------------------------------------------
static UnsignedInt parseIP(const std::string& text)
{
	in_addr addr;
	if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
	{
		fprintf(stderr, "bad ip %s\n", text.c_str());
		exit(2);
	}
	return ntohl(addr.s_addr);
}

static bool fileExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

static void touch(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "w");
	if (f)
		fclose(f);
}

static void frame()
{
	LanMesh::TickGlobal();
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

static std::string ipName(UnsignedInt ip)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", PRINTF_IP_AS_4_INTS(ip));
	return buf;
}

int main(int argc, char** argv)
{
	std::vector<UnsignedInt> players;
	std::string signalDir = "/tmp/lanmesh-signal";
	bool isHost = false;
	bool expectNotReady = false;
	std::wstring expectedProblem;
	bool exitWhenConnected = false;
	bool expectNoStart = false;
	int burst = 4;
	int packetsPerPeer = 3000;
	float fakeLoss = 0.0f;
	int fakeLag = 0;
	int lateMs = 0;
	int timeoutMs = 60000;
	int startAfterMs = 0;
	UnsignedInt exeCRC = g_globalData.m_exeCRC;

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() { return std::string(argv[++i]); };
		if (a == "--me") g_me = parseIP(next());
		else if (a == "--players")
		{
			std::string list = next();
			size_t pos = 0;
			while (pos != std::string::npos)
			{
				size_t comma = list.find(',', pos);
				players.push_back(parseIP(list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
				pos = (comma == std::string::npos) ? comma : comma + 1;
			}
		}
		else if (a == "--host") isHost = true;
		else if (a == "--signal") signalDir = next();
		else if (a == "--exe") exeCRC = (UnsignedInt)strtoul(next().c_str(), nullptr, 16);
		else if (a == "--packets") packetsPerPeer = atoi(next().c_str());
		else if (a == "--loss") fakeLoss = (float)atof(next().c_str());
		else if (a == "--lag") fakeLag = atoi(next().c_str());
		else if (a == "--late") lateMs = atoi(next().c_str());
		else if (a == "--timeout") timeoutMs = atoi(next().c_str());
		else if (a == "--start-after") startAfterMs = atoi(next().c_str());
		else if (a == "--expect-not-ready")
		{
			// The host must never be ready and must name this problem.
			expectNotReady = true;
			const std::string text = next();
			expectedProblem.assign(text.begin(), text.end());
		}
		else if (a == "--exit-when-connected") exitWhenConnected = true;
		else if (a == "--expect-no-start") expectNoStart = true;
		else if (a == "--burst") burst = atoi(next().c_str());
		else if (a == "--verbose") g_verbose = true;
	}
	g_start = timeGetTime();
	g_globalData.m_exeCRC = exeCRC;

	if (fakeLoss > 0.0f || fakeLag > 0)
	{
		SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, fakeLoss);
		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, fakeLag);
		NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: fake send loss %.1f%%, fake send lag %d ms", fakeLoss, fakeLag);
	}

	if (lateMs > 0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(lateMs));
	}

	// The LAN room, as LANAPI would hold it.
	LANGameInfo game;
	for (size_t i = 0; i < players.size(); ++i)
	{
		LANGameSlot& slot = game.m_slots[i];
		slot.m_human = TRUE;
		slot.m_ip = players[i];
		UnicodeString name;
		name.format(L"P%u", players[i] & 0xff);
		slot.m_name = name;
	}
	// An AI and an empty slot, which the mesh must ignore.
	game.m_slots[players.size()].m_human = FALSE;
	game.m_slots[players.size()].m_ip = 0;

	LANAPI lan;
	lan.m_localIP = g_me;
	lan.m_currentGame = &game;
	TheLAN = &lan;

	const std::string startFile = signalDir + "/start";
	NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: entering room as %s with %zu players", isHost ? "host" : "client", players.size());

	// ---- Room phase ----
	UnsignedInt lastCheck = 0;
	std::wstring lastProblems;
	bool started = false;
	while (timeGetTime() - g_start < (UnsignedInt)timeoutMs)
	{
		frame();
		LanMesh* mesh = LanMesh::Get();

		if (exitWhenConnected && mesh != nullptr && mesh->getNumConnectedPeers() == (Int)players.size() - 1)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: connected to everyone, now quitting abruptly");
			_exit(0);
		}

		if (isHost && timeGetTime() - lastCheck > 500 && timeGetTime() - g_start >= (UnsignedInt)startAfterMs)
		{
			lastCheck = timeGetTime();
			std::vector<UnicodeString> problems;
			const Bool ready = mesh->isReadyToStart(&game, problems);
			std::wstring joined;
			for (auto& p : problems)
			{
				joined += p.str();
				joined += L" | ";
			}
			if (joined != lastProblems)
			{
				lastProblems = joined;
				NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: start check: %s %ls", ready ? "READY" : "not ready:", joined.c_str());
			}
			if (ready)
			{
				if (expectNotReady)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST FAIL: became ready although it should not");
					return 1;
				}
				touch(startFile);
				started = true;
				break;
			}
		}
		else if (!isHost && fileExists(startFile))
		{
			started = true;
			break;
		}
	}

	if (expectNotReady)
	{
		const bool sawProblem = lastProblems.find(expectedProblem) != std::wstring::npos;
		NetworkLog(ELogVerbosity::LOG_RELEASE, sawProblem ? "TEST PASS: start blocked with \"%ls\"" : "TEST FAIL: start check never said \"%ls\"", expectedProblem.c_str());
		return sawProblem ? 0 : 1;
	}

	if (expectNoStart)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, started ? "TEST FAIL: match started" : "TEST PASS: match did not start");
		return started ? 1 : 0;
	}

	if (!started)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST FAIL: match never started");
		return 1;
	}

	// ---- Match phase: same order as LANAPI::OnGameStart ----
	LanMesh::ApplyNetworkTuning();
	static NetworkInterface dummyNetwork;
	TheNetwork = &dummyNetwork;
	game.m_inProgress = TRUE;

	LanMesh* mesh = LanMesh::Get();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: match starting. seed latency max historic %d ms, avg %d ms, connected peers %d, slack %d",
		mesh->getMaximumHistoricalLatency(), mesh->getAverageLatency(), mesh->getNumConnectedPeers(), g_globalData.m_networkRunAheadSlack);

	LanMeshTransport transport;
	transport.init((UnsignedInt)0, LanMesh::PORT);

	std::map<UnsignedInt, Int> nextSend;
	std::map<UnsignedInt, Int> nextRecv;
	for (UnsignedInt ip : players)
	{
		if (ip != g_me)
		{
			nextSend[ip] = 0;
			nextRecv[ip] = 0;
		}
	}

	Int errors = 0;
	Int bufferHighWater = 0;
	const UnsignedInt matchStart = timeGetTime();
	bool announcedDone = false;
	const std::string doneFile = signalDir + "/done_" + ipName(g_me);

	while (timeGetTime() - g_start < (UnsignedInt)timeoutMs)
	{
		LanMesh::TickGlobal();

		// Queue a few game packets per peer per frame, of varying size.
		for (auto& kv : nextSend)
		{
			for (int b = 0; b < burst && kv.second < packetsPerPeer; ++b)
			{
				const Int seq = kv.second;
				UnsignedByte payload[MAX_PACKET_SIZE];
				const Int len = 12 + (seq * 37) % (MAX_PACKET_SIZE - 12);
				const UnsignedInt header[3] = { 0x54455354u, g_me, (UnsignedInt)seq };
				memcpy(payload, header, sizeof(header));
				for (Int b = 12; b < len; ++b)
					payload[b] = (UnsignedByte)(seq * 7 + b);
				if (!transport.queueSend(kv.first, LanMesh::PORT, payload, len))
					break;
				++kv.second;
			}
		}

		transport.update();

		// Read like ConnectionManager::doRelay: in order, stop at the first empty slot.
		Int used = 0;
		for (Int i = 0; i < MAX_MESSAGES; ++i)
		{
			TransportMessage& msg = transport.m_inBuffer[i];
			if (msg.length <= 0)
				break;
			++used;
			UnsignedInt header[3];
			memcpy(header, msg.data, sizeof(header));
			const UnsignedInt sender = header[1];
			const Int seq = (Int)header[2];
			bool ok = header[0] == 0x54455354u && sender == msg.addr && nextRecv.count(sender) && seq == nextRecv[sender]
				&& msg.length == 12 + (seq * 37) % (MAX_PACKET_SIZE - 12);
			for (Int b = 12; ok && b < msg.length; ++b)
				ok = msg.data[b] == (UnsignedByte)(seq * 7 + b);
			if (!ok)
			{
				if (errors < 10)
					NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: bad packet from %s seq %d (expected %d) len %d addr %s",
						ipName(sender).c_str(), seq, nextRecv.count(sender) ? nextRecv[sender] : -1, msg.length, ipName(msg.addr).c_str());
				++errors;
			}
			else
			{
				++nextRecv[sender];
			}
			msg.length = 0;
		}
		bufferHighWater = std::max(bufferHighWater, used);

		bool allReceived = true;
		for (auto& kv : nextRecv)
			allReceived &= kv.second >= packetsPerPeer;

		if (allReceived && !announcedDone)
		{
			announcedDone = true;
			touch(doneFile);
			NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: received all %d packets from each of %zu peers in %u ms, %d errors, max %d slots used per frame, latency now %d ms",
				packetsPerPeer, nextRecv.size(), timeGetTime() - matchStart, errors, bufferHighWater, mesh->getMaximumLatency());
		}

		// Stay connected until everyone has everything, like players staying in the match.
		bool everyoneDone = announcedDone;
		for (UnsignedInt ip : players)
			everyoneDone &= fileExists(signalDir + "/done_" + ipName(ip));
		if (everyoneDone)
			break;

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	for (auto& kv : nextRecv)
		NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST: from %s received %d/%d", ipName(kv.first).c_str(), kv.second, packetsPerPeer);

	// Match over: like the real game, TheNetwork goes away and the mesh is torn down.
	TheNetwork = nullptr;
	LanMesh::TickGlobal();
	const bool meshGone = LanMesh::Get() == nullptr && !NetworkMeshLibrary::IsInitialized();

	const bool pass = announcedDone && errors == 0 && meshGone;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "TEST %s (errors %d, mesh torn down %d)", pass ? "PASS" : "FAIL", errors, meshGone);
	return pass ? 0 : 1;
}
