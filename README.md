# guild2-performance-patch
Pathfinding optimizations patch for The Guild 2 Renaissance based on reverse engineering the engine

This patch fixes some architectural bottlenecks of the game's original A* pathfinding by replacing memory allocators, implementing hardware prefetching, and introducing some neat caching mechanisms.

# Benchmarks

## Methodology

The benchmarks were run on two games: one early-middle game save (10 turns) and one late-game save (29 turns). You'll see the most benefits in the long-running campaigns.

RNG Determinism was enforced on both. The early-middle game one has been running for 8000 ticks, the late-game one for 3000 (lower, because testing out all options would take an absurd amount of time).

## Results

### 29-turn save

In the long-running campaign with more buildings being placed, the pathfinding falls back to the incredibly costly single-stepping more, so you'll see the biggest improvements here.

<img width="1189" height="589" alt="1_avg_duration" src="https://github.com/user-attachments/assets/f1d0e574-42d9-4405-af1c-8b43b5069cc6" />

<details>
  <summary>Max pathfinding tick duration</summary>
  <img width="1190" height="589" alt="4_max_duration" src="https://github.com/user-attachments/assets/e403fb0c-64b1-4001-ac1c-3ebfd60e6ce0" />
  
</details>

<details>
  <summary>Min pathfinding tick duration</summary>
  <img width="1189" height="589" alt="3_min_duration" src="https://github.com/user-attachments/assets/9d1a7f3a-7392-4655-98e1-0ba0aca296ab" />

</details>

<details>
  <summary>Median pathfinding tick duration</summary>
  <img width="1189" height="589" alt="2_median_duration" src="https://github.com/user-attachments/assets/1c2e841f-c1db-4467-a683-9798893b19ff" />

</details>



### 10-turn save

In the early game, the improvements aren't that spectacular, but it's still a solid ~20% improvement in average tick speed and ~30% improvement in max tick duration.

<img width="1189" height="589" alt="10t_avg_duration" src="https://github.com/user-attachments/assets/c49d7236-23d6-45ae-a843-b17de003c20f" />

<details>
  <summary>Max pathfinding tick duration</summary>
  <img width="1189" height="589" alt="10t_max_duration" src="https://github.com/user-attachments/assets/9eab9ef0-f298-4792-8f29-6ab77672eebf" />

</details>

<details>
  <summary>Min pathfinding tick duration</summary>
<img width="1189" height="589" alt="10t_min_duration" src="https://github.com/user-attachments/assets/33a979e6-a162-4c74-8605-ecfed71f9a2e" />

</details>

<details>
  <summary>Median pathfinding tick duration</summary>
<img width="1189" height="589" alt="10t_median_duration" src="https://github.com/user-attachments/assets/b63094d0-ed0b-41a1-94ca-a2c227ae0820" />

</details>

# Patch installation

1. Download the compiled d3d9.dll
2. Place the d3d9.dll file directly into your The Guild 2 main installation directory (the same folder that contains the game's executable .exe file).
3. Launch the game. If everything is working correctly, *pawels_optimization_path.ini* file should appear in your game directory

Note on playing on Linux with Wine: if you play on stock Wine, you need to launch the game with those options: 
```sh
WINEDLLOVERRIDES="d3d9=n,b" wine GuildII.exe
```

# Building

Just run make with mingw installed. There aren't any other dependencies.

# Configuration

The patch can be configured by modifying *pawels_optimization_path.ini* config file. I've heavily tested the defaults, so those should work the best.

```ini
[Features]
; Replaces the slow list findInsert with a hardware-prefetch optimized version. (1 = ON, 0 = OFF)
; Impact: High-Very high (with EnableObjectPool). The game spends most CPU time here. The compilers optimizations this path is compiled with also really help here. 
; IMPORTANT: I HIGHLY recommend using it with EnableObjectPool to get the full effect. 
EnableListTraversalPrefetch=1

; Uses a pre-allocated contiguous memory block instead of the fragmented STLPort allocator. (1 = ON, 0 = OFF)
; Impact: High-Very high (with EnableListTraversalPrefetch). Should be more cache-friendly.
EnableObjectPool=1

; Drops A* SingleStep requests to unreachable areas and puts them on timeout. (1 = ON, 0 = OFF)
; Impact: Miniscule-Extreme(on long-running campaigns).
EnableNegativeCache=1

; Bypasses the engine's Red-Black tree lookups with an O(1) array cache. (1 = ON, 0 = OFF)
; Impact: High.
EnableO1ShadowCache=1

[NegativeCache]
; How long (in milliseconds) a blocked path is blacklisted.
; Higher = better performance when stuck, but moving objects take longer to realize a path opened up.
ExpirationMs=60000

; How many grid cells around the failed startpos/endpos pair should also be considered blocked.
; Higher = faster quarantine of blocked areas, but might falsely block valid nearby paths.
; IMPORTANT: If set too high it can cause some valid paths to become unpassable. From my testing 10 is a sweet spot - lower it if you encounter pathfinding issues.
DistanceTolerance=10

[MemoryPools]
; Maximum number of A* path nodes (0x2C size) that can exist at once.
; Default: 100000 (Takes ~4.4MB RAM). Should be enough.
MaxPathNodes=100000

[ShadowCache]
; Maximum grid index for the map. 8000000 is a good compomise to not run out of addressable space.
; Default: 8000000 (Takes ~96MB RAM). ~50%% of requests get cached
MaxGridIndex=8000000

[Benchmark]
; 1 = ON, 0 = OFF
EnableBenchmark=0
; Overrides the random number generator to always return the average (max/2) to stabilize the state between runs.
EnableRandDeterminism=0
; Number of calls to measure
BenchmarkIterations=5000
; Number of initial calls to discard (warmup)
WarmupTicks=100
; Automatically close the game after benchmark finishes (1 = ON, 0 = OFF)
ExitAfterBenchmark=0

```

# On multiplayer

This patch is primarly intended for single-player campaigns. I haven't tested it for multiplayer compatibility.
The *NegativeCache* option would 100% not work in multiplayer by the virtue of how the timestamps work.
The other options should theoretically work provided all players use the patch and have similiar PC setups (used compiler optimizations may result in slight differences in floating point operations results on different processors, which may desync the game)

# Notes on possible improvements

1. The A* Open List shouldn't be a simple doubly-linked list in the first place. Replacing it with a binary/fibonacci heap would bring massive performance gains in the hot-path, but would require changing the internal structure of the pathfinder.
2. Don't use the single-stepping this much in the first place. The game should utilize the navmesh more. That would require some major architectural changes in how the pathfinder works
3. The entire simulation runs on a single thread. Making the pathfinder multi-threaded would require tearing down everything from cl_PathFinder::Update downwards and rewriting it in a way that doesn't change the global state in the parts that would be offloaded to other threads.
4. Rewrite and recompile the entire pathfinder to utilize better compiler optimizations

