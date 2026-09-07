#include "headers.h"
#include <errno.h>
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
  puts("  -r[<x>], --with-reins[=<x>]     reinsert hot records after >= ceil(x * log2(nodes+1)) history hops");
  puts("                                x >= 0, default 1.0; attach the value: -r0.5 or --with-reins=0.5");
  puts("  -R, --with-rebal                enable rebalancing logic");
  puts("      --rebalance-threshold <x>   rebalance when depth > ceil(log2(nodes+1)) * x (1.5; 1 = perfectly balanced)");
  puts("      --util-gate                 also require the utilization gate (off; kept for reference)");
  puts("      --latency-series <ms>       print per-interval latency lines (#L); off by default");
  puts("      --churn-mix <U/I/D>         ycsb_churn: %% updates / inserts / deletes, rest reads (50/25/25)");
  puts("      --dump-slabs <s>            heavy monitoring: one #S line per slab every <s> s and at the end (0 = off)");
  puts("      --reins-sample <N>          with -r: attempt a copy on one in N qualifying reads (16; 0 means 1)");
  puts("  -p, --with-prune <0..1>         enable repeated pruning at this global stale/reserved ratio");
  puts("  -M, --maintenance-period-ms <ms> interval for background maintenance (500)");
  puts("      --migrate-th <0..1>         migration: move a node into its history parent when both fit in t * capacity (0 = off)");
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
        {"with-reins",      optional_argument, 0, 'r'},
        {"with-rebal",      no_argument,       0, 'R'},
        {"rebalance-threshold", required_argument, 0, 1004},
        {"util-gate",       no_argument,       0, 1005},
        {"latency-series",  required_argument, 0, 1006},
        {"churn-mix",       required_argument, 0, 1011},
        {"dump-slabs",      required_argument, 0, 1012},
        {"reins-sample",    required_argument, 0, 1009},
        {"with-prune",      required_argument, 0, 'p'},
        {"maintenance-period-ms", required_argument, 0, 'M'},
        {"migrate-th",      required_argument, 0, 1014},
        {"items",           required_argument, 0, 'n'},
        {"requests",        required_argument, 0, 'q'},
        {"chunk",           required_argument, 0, 'c'},
        {"help",            no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "P:b:a:k:m:i:o:e:r::Rp:M:n:q:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'P': cfg.page_cache_size = strtoul(optarg, NULL, 0); break;
        case 'b': cfg.bench           = parse_bench(optarg);     break;
        case 'a': cfg.api             = parse_api(optarg);      break;
        case 'k': cfg.kv_size         = atoi(optarg);            break;
        case 'm': cfg.max_file_size   = strtoul(optarg, NULL, 0); break;
        case 'i': cfg.insert_mode     = parse_insert_mode(optarg); break;
        case 'o': cfg.old_percent     = atof(optarg);            break;
        case 'e': cfg.epoch           = strtoul(optarg, NULL, 0); break;
        case 'r': {
          double multiplier = 1.0;

          if (optarg != NULL) {
            char *end;

            errno = 0;
            multiplier = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || errno == ERANGE ||
                !isfinite(multiplier) || multiplier < 0.0) {
              fprintf(stderr, "-r/--with-reins requires a finite, nonnegative multiplier\n");
              return 1;
            }
          }
          cfg.with_reins = 1;
          cfg.reins_multiplier = multiplier;
          break;
        }
        case 'R': cfg.with_rebal      = 1;                       break;
        case 1004: cfg.rebalance_threshold = strtod(optarg, NULL); break;
        case 1005: cfg.util_gate = 1; break;
        case 1009: cfg.reins_sample = strtoul(optarg, NULL, 0);
                   if (!cfg.reins_sample) cfg.reins_sample = 1;
                   break;
        case 1012: cfg.dump_slabs_s = strtoul(optarg, NULL, 0); break;
        case 1011: if (sscanf(optarg, "%d/%d/%d", &cfg.churn_upd, &cfg.churn_ins,
                              &cfg.churn_del) != 3 ||
                       cfg.churn_upd + cfg.churn_ins + cfg.churn_del > 100) {
                     fprintf(stderr, "--churn-mix wants U/I/D percentages summing to <= 100\n");
                     return 1;
                   }
                   break;
        case 1006: cfg.latency_series_ms = strtoul(optarg, NULL, 0);
                   if (cfg.latency_series_ms && cfg.latency_series_ms < 100)
                     cfg.latency_series_ms = 100;
                   break;
        case 'p': {
          char *end;
          double ratio = strtod(optarg, &end);

          if (end == optarg || *end != '\0' || !isfinite(ratio) ||
              ratio < 0.0 || ratio > 1.0) {
            fprintf(stderr, "-p/--with-prune requires a ratio between 0 and 1\n");
            return 1;
          }
          cfg.with_prune = 1;
          cfg.prune_stale_ratio = ratio;
          break;
        }
        case 'M': cfg.maintenance_period_ms = strtoul(optarg, NULL, 0); break;
        case 1014: cfg.migrate_th      = strtod(optarg, NULL);   break;
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
    if (optind + 3 != argc) {
	fprintf(stderr, "Expected exactly three positional arguments: disks, workers, distributors.\n"
                        "Attach an optional -r value as -r0.5 or --with-reins=0.5.\n");
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
  printf("# \tReinsertion: %s%s\n", cfg.with_reins ? "enabled" : "disabled",
         cfg.with_reins ? " (on-read mode)" : "");
  if (cfg.with_reins)
    printf("# \tReinsert on read: >= ceil(%.6g * log2(nodes+1)) history hops, page must be "
           "hot already, one copy attempt per %lu qualifying reads\n",
           cfg.reins_multiplier, cfg.reins_sample);
  printf("# \tRebalancing: %s\n", cfg.with_rebal ? "enabled" : "disabled");
  printf("# \tPruning: %s\n", cfg.with_prune ? "enabled" : "disabled");
  if (cfg.latency_series_ms)
    printf("# \tLatency series: every %lu ms (#L lines, histogram percentiles)\n",
           cfg.latency_series_ms);
  if (cfg.with_prune)
    printf("# \tPruning trigger: repeat while global stale ratio >= %.2f, checked every %lu ms\n",
           cfg.prune_stale_ratio, cfg.maintenance_period_ms);
  if (cfg.migrate_th > 0)
    printf("# \tMigration: combined valid slots <= %.2f * slab capacity, "
           "one attempt per maintenance wake (%lu ms)\n",
           cfg.migrate_th, cfg.maintenance_period_ms);
  if (cfg.with_rebal) {
    printf("# \tRebalancing threshold: depth > ceil(log2(nodes+1)) * %.2f "
           "(checked every %lu ms; utilization gate %s)\n",
           cfg.rebalance_threshold, cfg.maintenance_period_ms,
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
  prune_stale_distribution_report("load");
  reset_restructuring_stats();

  //if (w.api == &BGWORK) {
  //  start_timer {
  //    init_old_keys(w.nb_items_in_db);
  //  }
  //  stop_timer("Init array for reinsertion test");
  //}


  /* One thread runs the maintenance operations, so they exclude each other. */
  if (cfg.with_rebal || cfg.with_prune || cfg.migrate_th > 0) {
    int worker_status = restructuring_worker_init();

    if (worker_status < 0)
      fprintf(stderr, "Cannot start restructuring worker: %s\n",
              strerror(-worker_status));
  }

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
