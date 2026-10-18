#pragma once
// Only the NetworkMeshLibrary part of the real NetworkMesh.h; the test implements it like the real one.
class NetworkMeshLibrary
{
public:
	static bool EnsureInitialized(int64_t userID);
	static void Shutdown();
	static bool IsInitialized() { return s_bInitialized; }
	static bool EnsureInitializedForLAN(UnsignedInt localIP);
	static void ReleaseLAN();
	static UnsignedInt GetGeneration() { return s_generation; }

private:
	static bool s_bInitialized;
	static bool s_bInitializedForLAN;
	static UnsignedInt s_generation;
};
