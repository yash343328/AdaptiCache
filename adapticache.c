/*
 * ============================================================
 *  AdaptiCache: Intelligent Adaptive Cache Replacement System
 * ============================================================
 *  Author      : Yash Jain
 *  Version     : 1.0.0
 *  Language    : C (C11 Standard)
 *  Compile     : gcc -O2 -std=c11 -Wall adapticache.c -o adapticache
 *  Run         : ./adapticache
 *
 *  PROBLEM STATEMENT:
 *    Traditional cache replacement policies (LRU, FIFO, LFU, CLOCK)
 *    are STATIC — they do not adapt to runtime workload behavior.
 *    When access patterns shift (e.g., sequential → temporal locality),
 *    performance degrades significantly.
 *
 *  SOLUTION:
 *    AdaptiCache monitors real-time hit/miss ratios for each algorithm
 *    using a shadow-cache approach and DYNAMICALLY switches to the
 *    best-performing policy every EPOCH (configurable window).
 *
 *  NOVEL CONTRIBUTIONS (for Research Paper):
 *    1. Adaptive Policy Switching using Hit-Rate Feedback Loop
 *    2. Shadow Cache Architecture for zero-overhead policy evaluation
 *    3. Policy Confidence Scoring with Exponential Moving Average
 *    4. Workload Phase Detection (Sequential, Temporal, Random)
 *    5. Performance comparison with baseline static algorithms
 *
 *  INDUSTRY RELEVANCE:
 *    - CPU L1/L2/L3 cache management
 *    - Database buffer pool management (PostgreSQL, MySQL)
 *    - CDN edge cache optimization
 *    - OS Virtual Memory page replacement
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

/* ==================== CONFIGURATION ==================== */
#define CACHE_SIZE          8       /* Main cache capacity (pages/blocks) */
#define SHADOW_CACHE_SIZE   8       /* Shadow cache size per algorithm     */
#define EPOCH_SIZE          20      /* Accesses before policy re-evaluation */
#define NUM_ALGORITHMS      4       /* LRU, FIFO, LFU, CLOCK               */
#define MAX_TRACE_SIZE      200     /* Max memory access trace length       */
#define HISTORY_SIZE        10      /* EMA window for confidence scoring    */
#define EMA_ALPHA           0.3     /* Exponential Moving Average factor    */
#define WORKLOAD_PHASES     3       /* Number of workload pattern types     */

/* ==================== DATA STRUCTURES ==================== */

/* Cache Node - doubly linked list for LRU */
typedef struct CacheNode {
    int page_id;
    int frequency;          /* for LFU */
    int timestamp;          /* for FIFO/LRU */
    bool clock_bit;         /* for CLOCK algorithm */
    struct CacheNode *prev;
    struct CacheNode *next;
} CacheNode;

/* Cache Frame - main cache data structure */
typedef struct {
    CacheNode *frames[CACHE_SIZE];
    int size;
    int capacity;
    int clock_hand;         /* CLOCK algorithm pointer */
    int time_counter;       /* Global logical time     */
} CacheFrame;

/* Shadow Cache for policy evaluation (lightweight) */
typedef struct {
    int pages[SHADOW_CACHE_SIZE];
    int freq[SHADOW_CACHE_SIZE];
    int timestamp[SHADOW_CACHE_SIZE];
    int size;
    int hits;
    int accesses;
    int clock_hand;
    int time_counter;
} ShadowCache;

/* Algorithm Statistics */
typedef struct {
    const char *name;
    long long total_hits;
    long long total_misses;
    long long total_accesses;
    double hit_rate;
    double ema_hit_rate;        /* Smoothed hit rate using EMA */
    double confidence_score;    /* Confidence: how stable is hit rate */
    int times_selected;         /* How many epochs was this active */
    double epoch_history[HISTORY_SIZE]; /* Recent epoch hit rates */
    int history_idx;
} AlgoStats;

/* Workload Phase Detector */
typedef struct {
    int recent_accesses[EPOCH_SIZE];
    int count;
    char phase[32];         /* "SEQUENTIAL", "TEMPORAL", "RANDOM" */
    double locality_score;  /* 0.0 = random, 1.0 = perfect temporal */
    double seq_score;       /* sequential pattern score */
} WorkloadDetector;

/* Main AdaptiCache Controller */
typedef struct {
    CacheFrame main_cache;
    ShadowCache shadows[NUM_ALGORITHMS];
    AlgoStats stats[NUM_ALGORITHMS];
    WorkloadDetector detector;
    int active_policy;          /* 0=LRU, 1=FIFO, 2=LFU, 3=CLOCK */
    int epoch_counter;
    long long global_hits;
    long long global_misses;
    long long total_accesses;
    int policy_switches;
    double overall_hit_rate;
} AdaptiCache;

/* Enum for policies */
typedef enum { POLICY_LRU=0, POLICY_FIFO=1, POLICY_LFU=2, POLICY_CLOCK=3 } Policy;

const char *POLICY_NAMES[] = {"LRU", "FIFO", "LFU", "CLOCK"};
const char *POLICY_ICONS[] = {"[⏱ LRU ]", "[→ FIFO]", "[# LFU ]", "[⊙ CLK ]"};

/* ==================== UTILITY FUNCTIONS ==================== */

void print_separator(char ch, int len) {
    for (int i = 0; i < len; i++) printf("%c", ch);
    printf("\n");
}

void print_header(const char *title) {
    int len = 62;
    print_separator('=', len);
    int pad = (len - (int)strlen(title)) / 2;
    printf("%*s%s\n", pad, "", title);
    print_separator('=', len);
}

/* ==================== CACHE OPERATIONS ==================== */

/* Initialize main cache */
void cache_init(CacheFrame *cache) {
    memset(cache->frames, 0, sizeof(cache->frames));
    cache->size = 0;
    cache->capacity = CACHE_SIZE;
    cache->clock_hand = 0;
    cache->time_counter = 0;
}

/* Initialize shadow cache */
void shadow_init(ShadowCache *sc) {
    memset(sc->pages, -1, sizeof(sc->pages));
    memset(sc->freq, 0, sizeof(sc->freq));
    memset(sc->timestamp, 0, sizeof(sc->timestamp));
    sc->size = 0;
    sc->hits = 0;
    sc->accesses = 0;
    sc->clock_hand = 0;
    sc->time_counter = 0;
}

/* Check if page is in shadow cache */
int shadow_find(ShadowCache *sc, int page) {
    for (int i = 0; i < SHADOW_CACHE_SIZE; i++) {
        if (sc->pages[i] == page) return i;
    }
    return -1;
}

/* ---- SHADOW LRU ---- */
void shadow_access_lru(ShadowCache *sc, int page) {
    sc->accesses++;
    sc->time_counter++;
    int idx = shadow_find(sc, page);
    if (idx >= 0) {
        sc->hits++;
        sc->timestamp[idx] = sc->time_counter;
        return;
    }
    /* Miss: evict LRU entry */
    if (sc->size < SHADOW_CACHE_SIZE) {
        sc->pages[sc->size] = page;
        sc->timestamp[sc->size] = sc->time_counter;
        sc->size++;
    } else {
        int lru_idx = 0, min_time = INT_MAX;
        for (int i = 0; i < SHADOW_CACHE_SIZE; i++) {
            if (sc->timestamp[i] < min_time) {
                min_time = sc->timestamp[i];
                lru_idx = i;
            }
        }
        sc->pages[lru_idx] = page;
        sc->timestamp[lru_idx] = sc->time_counter;
    }
}

/* ---- SHADOW FIFO ---- */
void shadow_access_fifo(ShadowCache *sc, int page) {
    sc->accesses++;
    int idx = shadow_find(sc, page);
    if (idx >= 0) { sc->hits++; return; }
    if (sc->size < SHADOW_CACHE_SIZE) {
        sc->pages[sc->size] = page;
        sc->timestamp[sc->size] = sc->time_counter++;
        sc->size++;
    } else {
        int fifo_idx = 0, min_time = INT_MAX;
        for (int i = 0; i < SHADOW_CACHE_SIZE; i++) {
            if (sc->timestamp[i] < min_time) {
                min_time = sc->timestamp[i];
                fifo_idx = i;
            }
        }
        sc->pages[fifo_idx] = page;
        sc->timestamp[fifo_idx] = sc->time_counter++;
    }
}

/* ---- SHADOW LFU ---- */
void shadow_access_lfu(ShadowCache *sc, int page) {
    sc->accesses++;
    int idx = shadow_find(sc, page);
    if (idx >= 0) { sc->hits++; sc->freq[idx]++; return; }
    if (sc->size < SHADOW_CACHE_SIZE) {
        sc->pages[sc->size] = page;
        sc->freq[sc->size] = 1;
        sc->size++;
    } else {
        int lfu_idx = 0, min_freq = INT_MAX;
        for (int i = 0; i < SHADOW_CACHE_SIZE; i++) {
            if (sc->freq[i] < min_freq) {
                min_freq = sc->freq[i];
                lfu_idx = i;
            }
        }
        sc->pages[lfu_idx] = page;
        sc->freq[lfu_idx] = 1;
    }
}

/* ---- SHADOW CLOCK ---- */
void shadow_access_clock(ShadowCache *sc, int page) {
    sc->accesses++;
    int idx = shadow_find(sc, page);
    if (idx >= 0) {
        sc->hits++;
        sc->freq[idx] = 1; /* reference bit */
        return;
    }
    if (sc->size < SHADOW_CACHE_SIZE) {
        sc->pages[sc->size] = page;
        sc->freq[sc->size] = 1;
        sc->size++;
        return;
    }
    /* CLOCK eviction */
    while (1) {
        if (sc->freq[sc->clock_hand] == 0) {
            sc->pages[sc->clock_hand] = page;
            sc->freq[sc->clock_hand] = 1;
            sc->clock_hand = (sc->clock_hand + 1) % SHADOW_CACHE_SIZE;
            break;
        }
        sc->freq[sc->clock_hand] = 0;
        sc->clock_hand = (sc->clock_hand + 1) % SHADOW_CACHE_SIZE;
    }
}

/* Route access to correct shadow cache */
void shadow_access(ShadowCache *sc, int policy, int page) {
    switch(policy) {
        case POLICY_LRU:   shadow_access_lru(sc, page);   break;
        case POLICY_FIFO:  shadow_access_fifo(sc, page);  break;
        case POLICY_LFU:   shadow_access_lfu(sc, page);   break;
        case POLICY_CLOCK: shadow_access_clock(sc, page); break;
    }
}

/* ==================== MAIN CACHE ACCESS ==================== */

/* Find page in main cache */
int main_cache_find(CacheFrame *cache, int page) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache->frames[i] && cache->frames[i]->page_id == page)
            return i;
    }
    return -1;
}

/* Evict based on active policy */
int evict_page(CacheFrame *cache, int policy) {
    int evict_idx = 0;
    if (policy == POLICY_LRU) {
        int min_time = INT_MAX;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (cache->frames[i] && cache->frames[i]->timestamp < min_time) {
                min_time = cache->frames[i]->timestamp;
                evict_idx = i;
            }
        }
    } else if (policy == POLICY_FIFO) {
        int min_time = INT_MAX;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (cache->frames[i] && cache->frames[i]->timestamp < min_time) {
                min_time = cache->frames[i]->timestamp;
                evict_idx = i;
            }
        }
    } else if (policy == POLICY_LFU) {
        int min_freq = INT_MAX;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (cache->frames[i] && cache->frames[i]->frequency < min_freq) {
                min_freq = cache->frames[i]->frequency;
                evict_idx = i;
            }
        }
    } else if (policy == POLICY_CLOCK) {
        while (1) {
            int h = cache->clock_hand;
            if (cache->frames[h] && !cache->frames[h]->clock_bit) {
                evict_idx = h;
                cache->clock_hand = (h + 1) % CACHE_SIZE;
                break;
            }
            if (cache->frames[h]) cache->frames[h]->clock_bit = false;
            cache->clock_hand = (h + 1) % CACHE_SIZE;
        }
    }
    return evict_idx;
}

/* Access main cache — returns true on HIT */
bool main_cache_access(CacheFrame *cache, int page, int policy, int *evicted_page) {
    cache->time_counter++;
    *evicted_page = -1;

    int idx = main_cache_find(cache, page);
    if (idx >= 0) {
        /* HIT — update metadata */
        cache->frames[idx]->frequency++;
        if (policy == POLICY_LRU)
            cache->frames[idx]->timestamp = cache->time_counter;
        if (policy == POLICY_CLOCK)
            cache->frames[idx]->clock_bit = true;
        return true;
    }

    /* MISS — load page */
    CacheNode *new_node = (CacheNode*)malloc(sizeof(CacheNode));
    new_node->page_id   = page;
    new_node->frequency = 1;
    new_node->timestamp = cache->time_counter;
    new_node->clock_bit = true;
    new_node->prev = new_node->next = NULL;

    if (cache->size < CACHE_SIZE) {
        /* Empty slot */
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (!cache->frames[i]) {
                cache->frames[i] = new_node;
                cache->size++;
                break;
            }
        }
    } else {
        /* Evict */
        int evict_idx = evict_page(cache, policy);
        *evicted_page = cache->frames[evict_idx]->page_id;
        free(cache->frames[evict_idx]);
        cache->frames[evict_idx] = new_node;
    }
    return false;
}

/* ==================== WORKLOAD DETECTOR ==================== */

void detector_init(WorkloadDetector *wd) {
    memset(wd->recent_accesses, -1, sizeof(wd->recent_accesses));
    wd->count = 0;
    strcpy(wd->phase, "UNKNOWN");
    wd->locality_score = 0.0;
    wd->seq_score = 0.0;
}

void detector_update(WorkloadDetector *wd, int page) {
    int pos = wd->count % EPOCH_SIZE;
    wd->recent_accesses[pos] = page;
    wd->count++;

    if (wd->count < EPOCH_SIZE) return;

    /* Analyze pattern */
    int seq_count = 0, repeat_count = 0;
    for (int i = 1; i < EPOCH_SIZE; i++) {
        int cur  = wd->recent_accesses[i];
        int prev = wd->recent_accesses[i-1];
        if (cur == prev + 1) seq_count++;
        if (cur == prev) repeat_count++;
    }

    wd->seq_score      = (double)seq_count    / (EPOCH_SIZE - 1);
    wd->locality_score = (double)repeat_count / (EPOCH_SIZE - 1);

    if (wd->seq_score > 0.5)
        strcpy(wd->phase, "SEQUENTIAL");
    else if (wd->locality_score > 0.3)
        strcpy(wd->phase, "TEMPORAL");
    else
        strcpy(wd->phase, "RANDOM");
}

/* ==================== ADAPTIVE POLICY SELECTOR ==================== */

/* Update EMA hit rate for each algorithm's shadow cache */
void update_algo_stats(AdaptiCache *ac) {
    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        ShadowCache *sc = &ac->shadows[i];
        AlgoStats   *st = &ac->stats[i];

        double epoch_hit_rate = (sc->accesses > 0)
            ? (double)sc->hits / sc->accesses : 0.0;

        /* Update EMA */
        st->ema_hit_rate = EMA_ALPHA * epoch_hit_rate
                         + (1.0 - EMA_ALPHA) * st->ema_hit_rate;

        /* Store history */
        st->epoch_history[st->history_idx % HISTORY_SIZE] = epoch_hit_rate;
        st->history_idx++;

        /* Compute confidence: low variance = high confidence */
        double mean = 0.0;
        int h_count = (st->history_idx < HISTORY_SIZE) ? st->history_idx : HISTORY_SIZE;
        for (int j = 0; j < h_count; j++) mean += st->epoch_history[j];
        mean /= (h_count > 0 ? h_count : 1);

        double variance = 0.0;
        for (int j = 0; j < h_count; j++) {
            double diff = st->epoch_history[j] - mean;
            variance += diff * diff;
        }
        variance /= (h_count > 0 ? h_count : 1);
        st->confidence_score = 1.0 / (1.0 + variance * 10.0);

        /* Update totals */
        st->total_hits      += sc->hits;
        st->total_misses    += (sc->accesses - sc->hits);
        st->total_accesses  += sc->accesses;
        if (st->total_accesses > 0)
            st->hit_rate = (double)st->total_hits / st->total_accesses;

        /* Reset shadow for next epoch */
        sc->hits = 0; sc->accesses = 0;
    }
}

/* Select best policy based on EMA hit rate + confidence */
int select_best_policy(AdaptiCache *ac) {
    double best_score = -1.0;
    int best_policy   = ac->active_policy;

    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        /* Combined score: hit_rate weighted by confidence */
        double score = ac->stats[i].ema_hit_rate * 0.7
                     + ac->stats[i].confidence_score * 0.3;
        if (score > best_score) {
            best_score  = score;
            best_policy = i;
        }
    }
    return best_policy;
}

/* ==================== MAIN ADAPTICACHE ACCESS ==================== */

bool adapticache_access(AdaptiCache *ac, int page) {
    ac->total_accesses++;
    ac->epoch_counter++;

    /* Update workload detector */
    detector_update(&ac->detector, page);

    /* Access all shadow caches */
    for (int i = 0; i < NUM_ALGORITHMS; i++)
        shadow_access(&ac->shadows[i], i, page);

    /* Access main cache with active policy */
    int evicted = -1;
    bool hit = main_cache_access(&ac->main_cache, page, ac->active_policy, &evicted);

    if (hit) {
        ac->global_hits++;
        ac->stats[ac->active_policy].total_hits++;
    } else {
        ac->global_misses++;
        ac->stats[ac->active_policy].total_misses++;
    }
    ac->stats[ac->active_policy].total_accesses++;

    /* Epoch boundary: re-evaluate policy */
    if (ac->epoch_counter >= EPOCH_SIZE) {
        update_algo_stats(ac);
        int new_policy = select_best_policy(ac);
        if (new_policy != ac->active_policy) {
            ac->policy_switches++;
            ac->active_policy = new_policy;
        }
        ac->stats[new_policy].times_selected++;
        ac->epoch_counter = 0;
    }

    ac->overall_hit_rate = (double)ac->global_hits / ac->total_accesses;
    return hit;
}

/* ==================== REPORTING & VISUALIZATION ==================== */

void print_cache_state(AdaptiCache *ac) {
    printf("  Cache[");
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (ac->main_cache.frames[i])
            printf("%3d", ac->main_cache.frames[i]->page_id);
        else
            printf("  _");
    }
    printf(" ] Policy: %s", POLICY_NAMES[ac->active_policy]);
}

void print_final_report(AdaptiCache *ac) {
    print_header(" AdaptiCache — Final Performance Report");

    printf("\n  %-20s %10s %10s %10s %10s %8s\n",
           "Algorithm", "Hits", "Misses", "Accesses", "Hit Rate%", "Selected");
    print_separator('-', 62);

    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        AlgoStats *st = &ac->stats[i];
        bool active = (i == ac->active_policy);
        printf("  %s%-8s%-12s %10lld %10lld %10lld %9.2f%% %8d\n",
               active ? ">> " : "   ",
               POLICY_NAMES[i],
               active ? "(ACTIVE)" : "",
               st->total_hits,
               st->total_misses,
               st->total_accesses,
               st->hit_rate * 100.0,
               st->times_selected);
    }

    print_separator('=', 62);
    printf("\n  ADAPTICACHE OVERALL:\n");
    printf("  Total Accesses     : %lld\n", ac->total_accesses);
    printf("  Global Hits        : %lld\n", ac->global_hits);
    printf("  Global Misses      : %lld\n", ac->global_misses);
    printf("  Overall Hit Rate   : %.2f%%\n", ac->overall_hit_rate * 100.0);
    printf("  Policy Switches    : %d\n", ac->policy_switches);
    printf("  Final Policy       : %s\n", POLICY_NAMES[ac->active_policy]);
    printf("\n  WORKLOAD ANALYSIS:\n");
    printf("  Detected Phase     : %s\n", ac->detector.phase);
    printf("  Locality Score     : %.3f\n", ac->detector.locality_score);
    printf("  Sequential Score   : %.3f\n", ac->detector.seq_score);

    print_separator('=', 62);

    /* Compare against pure static policies */
    printf("\n  STATIC vs ADAPTIVE COMPARISON:\n");
    print_separator('-', 62);
    printf("  %-20s %12s %12s\n", "Policy", "Hit Rate%", "vs Adaptive");
    print_separator('-', 62);
    double best_static = 0.0;
    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        if (ac->stats[i].hit_rate > best_static)
            best_static = ac->stats[i].hit_rate;
        double diff = (ac->overall_hit_rate - ac->stats[i].hit_rate) * 100.0;
        printf("  %-20s %11.2f%% %+11.2f%%\n",
               POLICY_NAMES[i],
               ac->stats[i].hit_rate * 100.0,
               diff);
    }
    printf("  %-20s %11.2f%% %+11s\n",
           "AdaptiCache (Ours)", ac->overall_hit_rate * 100.0, "(baseline)");
    print_separator('=', 62);
}

void print_algorithm_ema(AdaptiCache *ac) {
    printf("\n  EMA Hit Rates (Smoothed):\n");
    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        printf("  %-6s | ", POLICY_NAMES[i]);
        int bars = (int)(ac->stats[i].ema_hit_rate * 30);
        for (int b = 0; b < 30; b++)
            printf("%s", b < bars ? "█" : "░");
        printf(" %.1f%%\n", ac->stats[i].ema_hit_rate * 100.0);
    }
}

/* ==================== WORKLOAD GENERATORS ==================== */

/* Generate mixed workload: temporal + sequential + random phases */
void generate_mixed_trace(int *trace, int len) {
    srand(42);  /* Fixed seed for reproducibility */
    int i = 0;

    /* Phase 1: Temporal locality (hot set of 4 pages) */
    int hot_set[] = {1, 2, 3, 4};
    for (; i < len / 3; i++)
        trace[i] = hot_set[rand() % 4];

    /* Phase 2: Sequential scan */
    for (int j = 0; i < (len * 2) / 3; i++, j++)
        trace[i] = (j % 16) + 1;

    /* Phase 3: Random access */
    for (; i < len; i++)
        trace[i] = (rand() % 20) + 1;
}

/* Generate Zipf-distributed workload (realistic web/DB workload) */
void generate_zipf_trace(int *trace, int len) {
    srand(123);
    double zipf_weights[20];
    double total = 0.0;
    for (int i = 0; i < 20; i++) {
        zipf_weights[i] = 1.0 / (i + 1);
        total += zipf_weights[i];
    }
    for (int i = 0; i < len; i++) {
        double r = ((double)rand() / RAND_MAX) * total;
        double cumsum = 0.0;
        for (int j = 0; j < 20; j++) {
            cumsum += zipf_weights[j];
            if (r <= cumsum) { trace[i] = j + 1; break; }
        }
    }
}

/* ==================== SIMULATION RUNNER ==================== */

void run_simulation(AdaptiCache *ac, int *trace, int len,
                    const char *workload_name, bool verbose) {
    print_header(" AdaptiCache — Simulation");
    printf("  Workload   : %s\n", workload_name);
    printf("  Trace Len  : %d accesses\n", len);
    printf("  Cache Size : %d frames\n", CACHE_SIZE);
    printf("  Epoch Size : %d accesses\n\n", EPOCH_SIZE);
    print_separator('-', 62);

    if (verbose) {
        printf("  %-5s %-5s %-6s %-30s %-10s\n",
               "Acc#", "Page", "Result", "Cache State", "Policy");
        print_separator('-', 62);
    }

    for (int i = 0; i < len; i++) {
        bool hit = adapticache_access(ac, trace[i]);

        if (verbose) {
            printf("  %-5d %-5d %-6s", i+1, trace[i], hit ? "HIT" : "MISS");
            print_cache_state(ac);
            printf("\n");
        } else if (i % 20 == 0) {
            /* Progress indicator */
            printf("  [%3d] Page=%-3d %s | Hit%%=%.1f%% | Policy=%s\n",
                   i+1, trace[i], hit ? "HIT " : "MISS",
                   ac->overall_hit_rate * 100.0,
                   POLICY_NAMES[ac->active_policy]);
        }
    }

    print_separator('-', 62);
    print_algorithm_ema(ac);
    print_final_report(ac);
}

/* ==================== INITIALIZATION ==================== */

AdaptiCache* adapticache_create() {
    AdaptiCache *ac = (AdaptiCache*)calloc(1, sizeof(AdaptiCache));

    cache_init(&ac->main_cache);
    detector_init(&ac->detector);

    for (int i = 0; i < NUM_ALGORITHMS; i++) {
        shadow_init(&ac->shadows[i]);
        ac->stats[i].name          = POLICY_NAMES[i];
        ac->stats[i].ema_hit_rate  = 0.0;
        ac->stats[i].confidence_score = 0.5;
        ac->stats[i].history_idx   = 0;
    }

    ac->active_policy   = POLICY_LRU;  /* Start with LRU */
    ac->epoch_counter   = 0;
    ac->global_hits     = 0;
    ac->global_misses   = 0;
    ac->total_accesses  = 0;
    ac->policy_switches = 0;
    ac->overall_hit_rate= 0.0;

    return ac;
}

void adapticache_destroy(AdaptiCache *ac) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (ac->main_cache.frames[i]) free(ac->main_cache.frames[i]);
    free(ac);
}

/* ==================== DEMO: INTERACTIVE MODE ==================== */

void interactive_demo() {
    print_header("  AdaptiCache — Interactive Demo");
    printf("\n  Enter page numbers to access (0 to quit):\n");
    printf("  Cache Size = %d | Active Policy shown after each access\n\n",
           CACHE_SIZE);

    AdaptiCache *ac = adapticache_create();
    int page;

    while (1) {
        printf("  Access page > ");
        if (scanf("%d", &page) != 1 || page == 0) break;
        bool hit = adapticache_access(ac, page);
        printf("  %s | ", hit ? ">>> HIT  <<<" : "--- MISS ---");
        print_cache_state(ac);
        printf("\n  Hit Rate: %.1f%% | Switches: %d\n\n",
               ac->overall_hit_rate * 100.0, ac->policy_switches);
    }

    print_final_report(ac);
    adapticache_destroy(ac);
}

/* ==================== MAIN ENTRY POINT ==================== */

int main(int argc, char *argv[]) {
    printf("\n");
    print_header("  AdaptiCache v1.0 — Adaptive Cache Replacement System");
    printf("\n  Problem  : Static cache policies fail under dynamic workloads\n");
    printf("  Solution : Real-time policy switching via shadow cache feedback\n");
    printf("  Research : Novel EMA-based confidence scoring for policy selection\n\n");

    /* Check for interactive mode */
    if (argc > 1 && strcmp(argv[1], "--interactive") == 0) {
        interactive_demo();
        return 0;
    }

    /* ---- Simulation 1: Mixed Workload ---- */
    int trace1[MAX_TRACE_SIZE];
    generate_mixed_trace(trace1, MAX_TRACE_SIZE);

    AdaptiCache *ac1 = adapticache_create();
    run_simulation(ac1, trace1, MAX_TRACE_SIZE, "Mixed (Temporal+Sequential+Random)", false);
    adapticache_destroy(ac1);

    printf("\n\n");

    /* ---- Simulation 2: Zipf Distribution (Realistic Web/DB) ---- */
    int trace2[MAX_TRACE_SIZE];
    generate_zipf_trace(trace2, MAX_TRACE_SIZE);

    AdaptiCache *ac2 = adapticache_create();
    run_simulation(ac2, trace2, MAX_TRACE_SIZE, "Zipf Distribution (Web/DB Workload)", false);
    adapticache_destroy(ac2);

    printf("\n  Usage: %s --interactive   (for manual page access)\n\n", argv[0]);
    return 0;
}
