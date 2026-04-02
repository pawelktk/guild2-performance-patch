#include <algorithm>
#include <cmath>
#include <fstream>
#include <intrin.h>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

// ============================================================================
// CONFIG
// ============================================================================

struct Config {
  // Feature Toggles
  static bool EnableListTraversalPrefetch;
  static bool EnableNegativeCache;
  static bool EnableObjectPool;
  static bool EnableO1ShadowCache;

  // Negative Cache Settings
  static DWORD NegativeCacheExpirationMs;
  static int DistanceTolerance;

  // Memory Settings
  static size_t MaxPathNodes;
  static int MaxGridIndex;

  static std::string GetIniPath() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string exePath(path);
    size_t pos = exePath.find_last_of("\\/");
    return exePath.substr(0, pos) + "\\pawels_optimization_path.ini";
  }

  // clang-format off
    static void LoadOrGenerate() {
        std::string iniPath = GetIniPath();
        std::ifstream infile(iniPath);

        if (!infile.good()) {
            std::ofstream outfile(iniPath);
            
            outfile << "; ==============================================================================\n";
            outfile << "; GUILD 2 PATHFINDING OPTIMIZATION\n";
            outfile << "; ==============================================================================\n";
            outfile << "; Author: pawelktk (github.com/pawelktk)\n";
            outfile << "; ==============================================================================\n";
            outfile << "; NOTE: Any changes made to this file require a game restart to take effect!\n";
            outfile << "; ==============================================================================\n\n";

            outfile << "[Features]\n";
            outfile << "; Replaces the slow list findInsert with a hardware-prefetch optimized version. (1 = ON, 0 = OFF)\n";
            outfile << "; Impact: Very High. The game spends most CPU time here. The compilers optimizations this path is compiled with also really help here. \n";
            outfile << "EnableListTraversalPrefetch=1\n\n";

            outfile << "; Drops A* SingleStep requests to unreachable areas and puts them on timeout. (1 = ON, 0 = OFF)\n";
            outfile << "; Impact: Very High-Extreme(on long-running campaigns).\n";
            outfile << "EnableNegativeCache=1\n\n";

            outfile << "; Uses a pre-allocated contiguous memory block instead of the fragmented STLPort allocator. (1 = ON, 0 = OFF)\n";
            outfile << "; Impact: High. Should be more cache-friendly.\n";
            outfile << "EnableObjectPool=1\n\n";

            outfile << "; Bypasses the engine's Red-Black tree lookups with an O(1) array cache. (1 = ON, 0 = OFF)\n";
            outfile << "; Impact: Meh. It's not on the hot path.\n";
            outfile << "EnableO1ShadowCache=1\n\n";

            outfile << "[NegativeCache]\n";
            outfile << "; How long (in milliseconds) a blocked path is blacklisted.\n";
            outfile << "; Higher = better performance when stuck, but moving objects take longer to realize a path opened up.\n";
            outfile << "ExpirationMs=60000\n\n";

            outfile << "; How many grid cells around the failed startpos/endpos pair should also be considered blocked.\n";
            outfile << "; Higher = faster quarantine of blocked areas, but might falsely block valid nearby paths.\n";
            outfile << "; IMPORTANT: With higher DistanceTolerance performance gains can be massive, but if set too high it can cause some valid paths to become unpassable. From my testing 10 is a sweet spot - lower it if you encounter pathfinding issues.\n";
            outfile << "DistanceTolerance=10\n\n";

            outfile << "[MemoryPools]\n";
            outfile << "; Maximum number of A* path nodes (0x2C size) that can exist at once.\n";
            outfile << "; Default: 100000 (Takes ~4.4MB RAM). Should be enough.\n";
            outfile << "MaxPathNodes=100000\n\n";

            outfile << "[ShadowCache]\n";
            outfile << "; Maximum grid index for the map. 8000000 is a good compomise to not run out of addressable space.\n";
            outfile << "; Default: 8000000 (Takes ~96MB RAM). ~50%% of requests get cached\n";
            outfile << "MaxGridIndex=8000000\n";
            
            outfile.close();
        }

        EnableListTraversalPrefetch = GetPrivateProfileIntA("Features", "EnableListTraversalPrefetch", 1, iniPath.c_str()) != 0;
        EnableNegativeCache = GetPrivateProfileIntA("Features", "EnableNegativeCache", 1, iniPath.c_str()) != 0;
        EnableObjectPool = GetPrivateProfileIntA("Features", "EnableObjectPool", 1, iniPath.c_str()) != 0;
        EnableO1ShadowCache = GetPrivateProfileIntA("Features", "EnableO1ShadowCache", 1, iniPath.c_str()) != 0;

        NegativeCacheExpirationMs = GetPrivateProfileIntA("NegativeCache", "ExpirationMs", 60000, iniPath.c_str());
        DistanceTolerance = GetPrivateProfileIntA("NegativeCache", "DistanceTolerance", 15, iniPath.c_str());

        MaxPathNodes = GetPrivateProfileIntA("MemoryPools", "MaxPathNodes", 100000, iniPath.c_str());
        MaxGridIndex = GetPrivateProfileIntA("ShadowCache", "MaxGridIndex", 8000000, iniPath.c_str());

        // Safety check: O(1) Cache requires Object Pool's deallocation hook to invalidate its generation counter.
        if (!EnableObjectPool && EnableO1ShadowCache) {
            EnableO1ShadowCache = false;
        }
    }
  // clang-format on
};

bool Config::EnableListTraversalPrefetch = true;
bool Config::EnableNegativeCache = true;
bool Config::EnableObjectPool = true;
bool Config::EnableO1ShadowCache = true;
DWORD Config::NegativeCacheExpirationMs = 60000;
int Config::DistanceTolerance = 15;
size_t Config::MaxPathNodes = 100000;
int Config::MaxGridIndex = 8000000;

// ============================================================================
// ENGINE STRUCTURES AND OFFSETS
// ============================================================================

struct Node {
  Node *next;     // +0x00
  Node *prev;     // +0x04
  char pad[0x14]; // +0x08 to +0x1B
  float val1;     // +0x1C
  float val2;     // +0x20
};

struct TargetData {
  char pad[0x14]; // +0x00 to +0x13
  float val1;     // +0x14
  float val2;     // +0x18
};

struct Vector2i {
  int x;
  int y;
};

const DWORD PATHFINDING_FUNC_ADDR = 0x00565ae0;
const DWORD SINGLESTEPPATH_ADDR = 0x00566af0;
const int PATHGRID_DATA_OFFSET = 0x28;
const int START_POS_OFFSET = 0x3C;
const int TARGET_POS_OFFSET = 0x44;

// ============================================================================
// MEMORY MANAGEMENT AND CACHING
// ============================================================================

// Contiguous Memory Pool
class ContiguousMemoryPool {
private:
  size_t nodeSize;
  size_t maxNodes;
  char *memoryArena;
  void **freeListHead;

public:
  ContiguousMemoryPool(size_t size, size_t count) : nodeSize(size), maxNodes(count) {
    memoryArena =
        (char *)VirtualAlloc(NULL, nodeSize * maxNodes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    freeListHead = nullptr;

    // initialize free list in reverse order for sequential address delivery
    for (int i = maxNodes - 1; i >= 0; --i) {
      void **currentNode = (void **)(memoryArena + (i * nodeSize));
      *currentNode = freeListHead;
      freeListHead = currentNode;
    }
  }

  void *AllocateNode() {
    if (freeListHead) {
      void *allocatedNode = freeListHead;
      freeListHead = (void **)(*freeListHead);
      return allocatedNode;
    }
    return malloc(nodeSize); // failsafe
  }

  void FreeNode(void *ptr) {
    if (ptr >= memoryArena && ptr < memoryArena + (nodeSize * maxNodes)) {
      *(void **)ptr = freeListHead;
      freeListHead = (void **)ptr;
      return;
    }
    free(ptr); // failsafe
  }
};

ContiguousMemoryPool *g_GlobalPathPool = nullptr;

// Negative Cache
struct FailedPathCacheEntry {
  Vector2i startPos;
  Vector2i targetPos;
  DWORD timestamp;
};

class PathCacheManager {
private:
  std::vector<FailedPathCacheEntry> failedPaths;

  bool IsCloseEnough(const Vector2i &a, const Vector2i &b) {
    return std::abs(a.x - b.x) <= Config::DistanceTolerance &&
           std::abs(a.y - b.y) <= Config::DistanceTolerance;
  }

public:
  bool IsPathUnreachable(Vector2i *startPos, Vector2i *targetPos) {
    DWORD currentTime = GetTickCount();
    for (auto it = failedPaths.begin(); it != failedPaths.end();) {
      if (currentTime - it->timestamp > Config::NegativeCacheExpirationMs) {
        it = failedPaths.erase(it);
        continue;
      }
      if (IsCloseEnough(*startPos, it->startPos) && IsCloseEnough(*targetPos, it->targetPos)) {
        return true;
      }
      ++it;
    }
    return false;
  }

  void RegisterFailedPath(Vector2i *startPos, Vector2i *targetPos) {
    FailedPathCacheEntry entry = {*startPos, *targetPos, GetTickCount()};
    failedPaths.push_back(entry);
  }
};

PathCacheManager g_PathCache;

// O(1) Shadow Cache System
struct FastCacheEntry {
  void *treeSentinel;
  void *treeNode;
  unsigned long long searchGeneration;
};

FastCacheEntry *g_TreeCache = nullptr;
unsigned long long g_CurrentSearchGeneration = 1;

// ============================================================================
// ORIGINAL FUNCTION POINTERS
// ============================================================================

typedef int(__fastcall *SingleStepPath_t)(void *pThis, void *edxDummy);
SingleStepPath_t Original_SingleStepPath = nullptr;

typedef void(__thiscall *ClearAndFree_t)(void *pGridData, char resetFlag);
ClearAndFree_t ClearAndFree = (ClearAndFree_t)0x00566010;

typedef void(__fastcall *TreeFind_t)(void *pThis, void *edxDummy, void **outIter, int *searchKey);
TreeFind_t Original_TreeFind = (TreeFind_t)0x00504ec0;

// ============================================================================
// MEMORY HOOKING UTILITIES
// ============================================================================

void PatchCallInstruction(DWORD callAddress, void *hookedFunction) {
  DWORD oldProtect;
  VirtualProtect((void *)callAddress, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
  *(DWORD *)(callAddress + 1) = (DWORD)hookedFunction - callAddress - 5;
  VirtualProtect((void *)callAddress, 5, oldProtect, &oldProtect);
}

void PatchIndirectCallInstruction(DWORD callAddress, void *hookedFunction) {
  DWORD oldProtect;
  VirtualProtect((void *)callAddress, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
  *(BYTE *)callAddress = 0xE8; // convert FF 15 to E8 (Direct CALL)
  *(DWORD *)(callAddress + 1) = (DWORD)hookedFunction - callAddress - 5;
  *(BYTE *)(callAddress + 5) = 0x90; // NOP padding
  VirtualProtect((void *)callAddress, 6, oldProtect, &oldProtect);
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// Custom STL Allocator
void *__cdecl Hooked_STL_Allocate(size_t size) {
  if (size == 0x2C && g_GlobalPathPool)
    return g_GlobalPathPool->AllocateNode();
  return malloc(size);
}

// Custom STL Deallocator
void __cdecl Hooked_STL_Deallocate(void *ptr, size_t size) {
  if (!ptr)
    return;
  if (size == 0x2C && g_GlobalPathPool) {
    g_CurrentSearchGeneration++; // invalidate O(1) Cache safely
    g_GlobalPathPool->FreeNode(ptr);
    return;
  }
  free(ptr);
}

long long bypassed = 0;
long long notbypassed = 0;

// Binary Tree Shadow Cache
void __fastcall Hooked_TreeFind_Local(void *pThis, void *edxDummy, void **outIter, int *searchKey) {
  int key = *searchKey;
#ifdef DEBUGSTUFF
  if (bypassed + notbypassed == 100000000) {
    std::ofstream f("bypassed");
    f << "Bypassed: " << bypassed << std::endl;
    f << "notbypassed: " << notbypassed << std::endl;
    f.close();
  }
#endif

  if (key >= 0 && key < Config::MaxGridIndex) {
    if (g_TreeCache[key].searchGeneration == g_CurrentSearchGeneration &&
        g_TreeCache[key].treeSentinel == pThis) {
      *outIter = g_TreeCache[key].treeNode;
#ifdef DEBUGSTUFF
      bypassed++;
#endif
      return;
    }
  }
#ifdef DEBUGSTUFF
  notbypassed++;
#endif
  Original_TreeFind(pThis, edxDummy, outIter, searchKey);

  if (*outIter != pThis && key >= 0 && key < Config::MaxGridIndex) {
    g_TreeCache[key].treeNode = *outIter;
    g_TreeCache[key].treeSentinel = pThis;
    g_TreeCache[key].searchGeneration = g_CurrentSearchGeneration;
  }
}

// Negative Caching
int __fastcall Hooked_SingleStepPath(void *pThis, void *edxDummy) {
  void *pGridData = *(void **)((char *)pThis + PATHGRID_DATA_OFFSET);

  if (pGridData != nullptr) {
    Vector2i *startPos = (Vector2i *)((char *)pGridData + START_POS_OFFSET);
    Vector2i *targetPos = (Vector2i *)((char *)pGridData + TARGET_POS_OFFSET);

    if (g_PathCache.IsPathUnreachable(startPos, targetPos)) {
      ClearAndFree(pGridData, 0);
      *(BYTE *)((char *)pGridData + 0x6A) = 0;
      return 0x68;
    }

    int result = Original_SingleStepPath(pThis, edxDummy);

    if (result == 0x68) {
      g_PathCache.RegisterFailedPath(startPos, targetPos);
    }
    return result;
  }

  return Original_SingleStepPath(pThis, edxDummy);
}

// Hardware-accelerated list findInsert with prefetch
extern "C" void __stdcall OptimizedFindInsertNode(void **outNode,
                                                  Node *sentinelNode,
                                                  TargetData *targetData) {
  Node *currentNode = sentinelNode->next;

  if (currentNode == sentinelNode) {
    *outNode = sentinelNode;
    return;
  }

  float targetSum = targetData->val1 + targetData->val2;
  Node *tailNode = sentinelNode->prev;
  float tailSum = tailNode->val1 + tailNode->val2;

  if (targetSum >= tailSum) {
    *outNode = sentinelNode;
    return;
  }

  while (currentNode != sentinelNode) {
    _mm_prefetch((const char *)currentNode->next, _MM_HINT_T0);
    float nodeSum = currentNode->val1 + currentNode->val2;

    if (targetSum < nodeSum) {
      *outNode = currentNode;
      return;
    }
    currentNode = currentNode->next;
  }

  *outNode = sentinelNode;
}

// ============================================================================
// HOOK INSTALLERS
// ============================================================================

void InstallPathfindingOptimization() {
  DWORD oldProtect;
  VirtualProtect((void *)PATHFINDING_FUNC_ADDR, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
  *(BYTE *)PATHFINDING_FUNC_ADDR = 0xE9;
  *(DWORD *)(PATHFINDING_FUNC_ADDR + 1) =
      (DWORD)OptimizedFindInsertNode - PATHFINDING_FUNC_ADDR - 5;
  VirtualProtect((void *)PATHFINDING_FUNC_ADDR, 5, oldProtect, &oldProtect);
}

void InstallSingleStepPathHook() {
  const int PATCH_SIZE = 6;
  void *trampoline =
      VirtualAlloc(NULL, PATCH_SIZE + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

  memcpy(trampoline, (void *)SINGLESTEPPATH_ADDR, PATCH_SIZE);
  *(BYTE *)((DWORD)trampoline + PATCH_SIZE) = 0xE9;
  *(DWORD *)((DWORD)trampoline + PATCH_SIZE + 1) =
      (SINGLESTEPPATH_ADDR + PATCH_SIZE) - ((DWORD)trampoline + PATCH_SIZE + 5);

  Original_SingleStepPath = (SingleStepPath_t)trampoline;

  DWORD oldProtect;
  VirtualProtect((void *)SINGLESTEPPATH_ADDR, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);
  *(BYTE *)SINGLESTEPPATH_ADDR = 0xE9;
  *(DWORD *)(SINGLESTEPPATH_ADDR + 1) = (DWORD)Hooked_SingleStepPath - SINGLESTEPPATH_ADDR - 5;
  *(BYTE *)(SINGLESTEPPATH_ADDR + 5) = 0x90;
  VirtualProtect((void *)SINGLESTEPPATH_ADDR, PATCH_SIZE, oldProtect, &oldProtect);
}

void InstallObjectPoolAllocators() {
  g_GlobalPathPool = new ContiguousMemoryPool(0x2C, Config::MaxPathNodes);

  PatchIndirectCallInstruction(0x00565cd3, (void *)Hooked_STL_Allocate);
  PatchIndirectCallInstruction(0x0056604a, (void *)Hooked_STL_Deallocate);
  PatchIndirectCallInstruction(0x0056606d, (void *)Hooked_STL_Deallocate);
}

void InstallLocalizedTreeFindHook() {
  if (g_TreeCache == nullptr) {
    g_TreeCache = (FastCacheEntry *)VirtualAlloc(NULL,
                                                 sizeof(FastCacheEntry) * Config::MaxGridIndex,
                                                 MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE);
  }
  PatchCallInstruction(0x00567034, (void *)Hooked_TreeFind_Local);
}

// ============================================================================
// D3D9 PROXY & DLL MAIN
// ============================================================================

typedef void *(WINAPI *Direct3DCreate9_t)(unsigned int);
Direct3DCreate9_t Original_Direct3DCreate9 = nullptr;

extern "C" void *WINAPI Direct3DCreate9(unsigned int SDKVersion) {
  if (!Original_Direct3DCreate9) {
    char syspath[MAX_PATH];
    GetSystemDirectoryA(syspath, MAX_PATH);
    lstrcatA(syspath, "\\d3d9.dll");

    HMODULE hMod = LoadLibraryA(syspath);
    if (hMod) {
      Original_Direct3DCreate9 = (Direct3DCreate9_t)GetProcAddress(hMod, "Direct3DCreate9");
    }
  }

  if (Original_Direct3DCreate9) {
    return Original_Direct3DCreate9(SDKVersion);
  }
  return nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);

    Config::LoadOrGenerate();
    if (Config::EnableListTraversalPrefetch)
      InstallPathfindingOptimization();
    if (Config::EnableNegativeCache)
      InstallSingleStepPathHook();
    if (Config::EnableObjectPool)
      InstallObjectPoolAllocators();
    if (Config::EnableO1ShadowCache)
      InstallLocalizedTreeFindHook();
  } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
    // cleanup
    if (g_GlobalPathPool)
      delete g_GlobalPathPool;
  }
  return TRUE;
}
