#include "headers.h"
#include <getopt.h>
#include <math.h>

int print = 0;
int load = 1;
extern int cache_hit;
extern int merged;
extern int try_fsst;

static void print_help(char *n) {
  printf("Usage: %s [options] <nb_disks> <nb_workers> <nb_distributors>\n", n);
  puts("Options:");
  puts("  -P, --page-cache-size <bytes>   set page cache size");
  puts("  -b, --bench <bench_name>        select workload (e.g. ycsb_c_zipfian)");
  puts("  -a, --api <api_name>            select API (ycsb, dbbench, bgwork, locality, latprobe)");
  puts("  -k, --kv-size <bytes>           set KV_SIZE");
  puts("  -m, --max-file-size <bytes>     set MAX_FILE_SIZE");
  puts("  -i, --insert-mode <ascend|descend|random>");
  puts("  -o, --old-percent <float>       set OLD_PERCENT");
  puts("  -e, --epoch <number>            set EPOCH");
  puts("  -r, --with-reins                enable reinsertion logic");
  puts("  -R, --with-rebal                enable rebalancing logic");
  puts("      --rebalance-threshold <x>   rebalance when depth > ceil(log2(nodes+1)) * x (1.5; 1 = perfectly balanced)");
  puts("      --util-gate                 also require the utilization gate (off; kept for reference)");
  puts("      --latency-series <ms>       print per-interval latency lines (#L); off by default");
  puts("  -p, --with-prune                enable pruning logic");
  puts("      --prune-margin <slots>      slots kept free in a merged slab");
  puts("      --prune-min-age <slabs>     skip triples whose leaf is newer than this");
  puts("  -C, --pruning                   prune automatically when stale slots exceed the ratio");
  puts("      --prune-stale-ratio <0..1>  stale/reserved slot ratio that triggers a prune (0.3)");
  puts("      --prune-period-ms <ms>      how often the ratio is measured (500)");
  puts("  -n, --items <number>            set number of items in DB");
  puts("  -q, --requests <number>         set number of requests");
  puts("  -c, --chunk <number>            chunk size for shuffling");
  puts("  -h, --help                      show this help message");
}

int main(int argc, char **argv) {
  declare_timer;
    init_default_config(&cfg);

    static struct option long_opts[] = {
        {"page-cache-size", required_argument, 0, 'P'},
        {"bench",           required_argument, 0, 'b'},
        {"api",             required_argument, 0, 'a'},
        {"kv-size",         required_argument, 0, 'k'},
        {"max-file-size",   required_argument, 0, 'm'},
        {"insert-mode",     required_argument, 0, 'i'},
        {"old-percent",     required_argument, 0, 'o'},
        {"epoch",           required_argument, 0, 'e'},
        {"with-reins",      no_argument,       0, 'r'},
        {"with-rebal",      no_argument,       0, 'R'},
        {"rebalance-threshold", required_argument, 0, 1004},
        {"util-gate",       no_argument,       0, 1005},
        {"latency-series",  required_argument, 0, 1006},
        {"with-prune",      no_argument,       0, 'p'},
        {"prune-margin",    required_argument, 0, 1000},
        {"prune-min-age",   required_argument, 0, 1001},
        {"pruning",         no_argument,       0, 'C'},
        {"prune-stale-ratio", required_argument, 0, 1002},
        {"prune-period-ms", required_argument, 0, 1003},
        {"items",           required_argument, 0, 'n'},
        {"requests",        required_argument, 0, 'q'},
        {"chunk",           required_argument, 0, 'c'},
        {"help",            no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "P:b:a:k:m:i:o:e:rRpCn:q:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'P': cfg.page_cache_size = strtoul(optarg, NULL, 0); break;
        case 'b': cfg.bench           = parse_bench(optarg);     break;
        case 'a': cfg.api             = parse_api(optarg);      break;
        case 'k': cfg.kv_size         = atoi(optarg);            break;
        case 'm': cfg.max_file_size   = strtoul(optarg, NULL, 0); break;
        case 'i': cfg.insert_mode     = parse_insert_mode(optarg); break;
        case 'o': cfg.old_percent     = atof(optarg);            break;
        case 'e': cfg.epoch           = strtoul(optarg, NULL, 0); break;
        case 'r': cfg.with_reins      = 1;                       break;
        case 'R': cfg.with_rebal      = 1;                       break;
        case 1004: cfg.rebalance_threshold = strtod(optarg, NULL); break;
        case 1005: cfg.util_gate = 1; break;
        case 1006: cfg.latency_series_ms = strtoul(optarg, NULL, 0);
                   if (cfg.latency_series_ms && cfg.latency_series_ms < 100)
                     cfg.latency_series_ms = 100;
                   break;
        case 'p': cfg.with_prune      = 1;                       break;
        case 1000: cfg.prune_margin   = strtoul(optarg, NULL, 0); break;
        case 1001: cfg.prune_min_age  = strtoul(optarg, NULL, 0); break;
        case 'C': cfg.with_prune = 1; cfg.prune_auto = 1;        break;
        case 1002: cfg.prune_stale_ratio = strtod(optarg, NULL);   break;
        case 1003: cfg.prune_period_ms = strtoul(optarg, NULL, 0); break;
        case 'n': cfg.nb_items_in_db  = strtoull(optarg, NULL, 0); break;
        case 'q': cfg.nb_requests     = strtoull(optarg, NULL, 0); break;
        case 'c': cfg.chunk_for_shuffle = strtoull(optarg, NULL, 0); break;
        case 'h':
	    print_help(argv[0]);
            return 0;
        default:
            fprintf(stderr, "Unknown option\n");
            return 1;
        }
    }
        // 남은 포지셔널 세 개
    if (optind + 3 > argc) {
	print_help(argv[0]);
        return 1;
    }
    int nb_disks  = atoi(argv[optind++]);
    int nb_workers_per_disk = atoi(argv[optind++]);
    int nb_distributors_per_disk = atoi(argv[optind++]);

    if (!validate_runtime_config(&cfg))
        return EXIT_FAILURE;

    // --- 워크로드 초기화 예시 ---
    struct workload w;
    w.api            = cfg.api;
    w.nb_items_in_db = cfg.nb_items_in_db;
    w.nb_load_injectors = cfg.api == &LATPROBE ? 1 : 4;
    if (cfg.nb_requests)
        w.nb_requests = cfg.nb_requests;

    // 벤치 실행
    bench_t workload, workloads[] = { cfg.bench };

  /* Pretty printing useful info */
  printf("# Configuration:\n");
  printf("# \tPage cache size: %lu GB\n", cfg.page_cache_size / 1024 / 1024 / 1024);
  printf("# \tDisks: %d, I/O Workers: %d, Distributors: %d\n",
         nb_disks, nb_workers_per_disk, nb_distributors_per_disk);
  printf(
      "# \tIO configuration: %d queue depth (capped: %s, extra waiting: %s)\n",
      QUEUE_DEPTH, NEVER_EXCEED_QUEUE_DEPTH ? "yes" : "no",
      WAIT_A_BIT_FOR_MORE_IOS ? "yes" : "no");
  printf("# \tQueue configuration: %d maximum pending callbaks per worker\n",
         MAX_NB_PENDING_CALLBACKS_PER_WORKER);
  printf("# \tThread pinning: %s\n", PINNING ? "yes" : "no");
  printf("# \tBench: %s (%lu elements)\n", w.api->api_name(), w.nb_items_in_db);
  printf("# \tKV_SIZE: %d, MAX_FILE_SIZE: %lu\n", cfg.kv_size, cfg.max_file_size);
  printf("# \tInsert mode: %s\n", 
        cfg.insert_mode == ASCEND ? "ASCEND" : 
        cfg.insert_mode == DESCEND ? "DESCEND" : 
        cfg.insert_mode == RANDOM ? "RANDOM" : "UNKNOWN");
  printf("# \tChunk for shuffling: %lu\n", cfg.chunk_for_shuffle);
  printf("# \tReinsertion: %s\n", cfg.with_reins ? "enabled" : "disabled");
  printf("# \tRebalancing: %s\n", cfg.with_rebal ? "enabled" : "disabled");
  printf("# \tPruning: %s\n", cfg.with_prune ? "enabled" : "disabled");
  if (cfg.latency_series_ms)
    printf("# \tLatency series: every %lu ms (#L lines, histogram percentiles)\n",
           cfg.latency_series_ms);
  if (cfg.with_prune)
    printf("# \tPruning: margin %lu slots, minimum leaf age %lu slabs\n",
           cfg.prune_margin, cfg.prune_min_age);
  if (cfg.prune_auto)
    printf("# \tPruning trigger: stale ratio >= %.2f, measured every %lu ms\n",
           cfg.prune_stale_ratio, cfg.prune_period_ms);
  if (cfg.with_rebal) {
    printf("# \tRebalancing threshold: depth > ceil(log2(nodes+1)) * %.2f "
           "(checked every %lu ms; utilization gate %s)\n",
           cfg.rebalance_threshold, cfg.prune_period_ms,
           cfg.util_gate ? "required" : "off, sampled only");
  }

  /* Initialization of random library */
  start_timer {
    printf(
        "Initializing random number generator (Zipf) -- this might take a "
        "while for large databases...\n");
    init_zipf_generator(
        0, w.nb_items_in_db - 1); /* This takes about 3s... not sure why, but
                                     this is legacy code :/ */
  }
  stop_timer("Initializing random number generator (Zipf)");

#ifdef REALKEY_FILE_PATH
  start_timer {
    printf(
      "Loading real world keys from file -- this might take a while for large key sets...\n");
    load_real_keys(w.nb_items_in_db); // OSM 파일에서 키를 불러옵니다.
  }
  stop_timer("Loading keys from real world file");
#endif

  /* Recover database */
  start_timer {
    slab_workers_init(nb_disks, nb_workers_per_disk, nb_distributors_per_disk);
  }
  stop_timer("Init found %lu elements", get_database_size());


  repopulate_db(&w);
  load = 0;

  start_timer {
    flush_batched_load();
  }
  stop_timer("Remaining batch loading");

  print = 1;
  cache_hit = 0;
  merged = 0;

  /* Setup, not workload: counted in the load-phase block below. */
  if (cfg.with_rebal) {
    int rebalance_status;

    start_timer {
      rebalance_status = tnt_rebalancing();
    }
    stop_timer("Rebalancing operations");
    if (rebalance_status < 0)
      fprintf(stderr, "Rebalancing failed: %s\n", strerror(-rebalance_status));
    else if (rebalance_status == TNT_REBALANCE_NOOP)
      puts("Rebalancing was not needed");
  }

  print_restructuring_stats("load");
  prune_scan_report("load");
  reset_restructuring_stats();

  //if (w.api == &BGWORK) {
  //  start_timer {
  //    init_old_keys(w.nb_items_in_db);
  //  }
  //  stop_timer("Init array for reinsertion test");
  //}


  /* One thread runs both maintenance operations, so they exclude each other. */
  if (cfg.with_rebal || cfg.with_prune) {
    int worker_status = restructuring_worker_init();

    if (worker_status < 0)
      fprintf(stderr, "Cannot start restructuring worker: %s\n",
              strerror(-worker_status));
  }

  if (cfg.with_reins)
    fsst_worker_init();

  utilization_sampler_init();

  /* Launch benchs */
  foreach (workload, workloads) {
    if (!cfg.nb_requests) {
      if (workload == ycsb_e_uniform || workload == ycsb_e_zipfian) {
        w.nb_requests =
            2000000LU;  // requests for YCSB E are longer (scans) so we do less
      } else if (workload == dbbench_all_random || workload == dbbench_all_dist ||
                 workload == dbbench_prefix_random ||
                 workload == dbbench_prefix_dist) {
        w.nb_requests = 100000000LU;
      } else {
        w.nb_requests = 100000000LU;
      }
    }
    run_workload(&w, workload);
    printf("lookup hit: %d\n", cache_hit);
    printf("merged: %d\n", merged);
    printf("try_fsst: %d\n", try_fsst);
    cache_hit = 0;
    merged = 0;
  }

  //tnt_print();

#if DEBUG
  print_slow_payloads();
#endif
}
