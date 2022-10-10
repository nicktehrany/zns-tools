#include "fpbench.h"

struct workload_manager wl_man;

/*
 *
 * Show the command help.
 *
 *
 * */
static void show_help() {
    MSG("Possible flags are:\n");
    MSG("-f [file]\tFilename for the benchmark [Required]\n");
    MSG("-l [Int, 0-3]\tLog Level to print (Default 0)\n");
    MSG("-s \t\tFile size (Default 4096B)\n");
    MSG("-b \t\tBlock size in which to submit I/Os (Default 4096B)\n");
    MSG("-w \t\tRead/Write Hint (Default 0)\n\t\tRWH_WRITE_LIFE_NOT_SET = 0\n\t\tRWH_WRITE_LIFE_NONE = 1\n\t\tRWH_WRITE_LIFE_SHORT = 2\n\t\tRWH_WRITE_LIFE_MEDIUM = 3\n\t\tRWH_WRITE_LIFE_LONG = 4\n\t\tRWH_WRITE_LIFE_EXTREME = 5\n");
    MSG("-h \t\tShow this help\n");
    MSG("-n \t\tNumber of jobs to concurrently execute the benchmark\n");

    exit(0);
}

static void check_options() {

    if (wl_man.nr_wls < 1) {
        ERR_MSG("Missing filename -f flag.\n");
    }
    if (wl_man.wl[0].fsize == 0) {
        ERR_MSG("Missing file size -s flag.\n");
    }
}


static void write_file(struct workload workload) {
    int out, r, w, ret;
    char buf[workload.bsize];
    
    r = read(wl_man.data_fd, &buf, workload.bsize);
    INFO(3, "Read %dB from /dev/urandom\n", r);

    if (access(workload.filename, F_OK) == 0) {
        ret = remove(workload.filename);
        if (ret == 0) {
            INFO(1, "Found existing file %s. Deleting it.\n", workload.filename);
        } else {
            ERR_MSG("Failed deleting existing file %s CODE %d\n", workload.filename, ret);
        }
    }

    out = open(workload.filename, O_WRONLY | O_CREAT, 0664);

    if (!out) {
        ERR_MSG("Failed opening file for writing: %s\n", workload.filename);
    }

    if (!fcntl(out, F_SET_FILE_RW_HINT, workload.rw_hint)) {
        ERR_MSG("Failed setting write hint: %d\n", workload.rw_hint);
    }

    INFO(1, "Set file %s with write hint: %d\n", workload.filename, workload.rw_hint);

    for (uint64_t i = 0; i < workload.fsize; i += workload.bsize) {
        lseek(out, i, SEEK_SET);
        w = write(out, &buf, workload.bsize);
        INFO(3, "Wrote %dB to %s at offset %lu\n", w, workload.filename, i);
    }

    fsync(out);
    close(out);
}

static void set_segment_counters(uint32_t segment_id, uint32_t num_segments) {
    wl_man.segment_ctr += num_segments;
    get_procfs_single_segment_bits(ctrl.bdev.dev_name, segment_id); 

    switch(segman.sm_info[0].type) {
        case CURSEG_COLD_DATA:
            wl_man.cold_ctr += num_segments;
            break;
        case CURSEG_WARM_DATA:
            wl_man.warm_ctr += num_segments;
            break;
        case CURSEG_HOT_DATA:
            wl_man.hot_ctr += num_segments;
            break;
        default:
            break;
    }
    free(segman.sm_info);
}

static void print_report(struct workload workload, struct extent_map *extents) {
    uint32_t segment_id;
    uint32_t current_zone = 0;
    uint32_t num_segments;
    uint64_t segment_start, segment_end;

    INFO(1, "File %s broken into %u extents\n", workload.filename, extents->ext_ctr);

    for (uint64_t i = 0; i < extents->ext_ctr; i++) {
        segment_id =
            (extents->extent[i].phy_blk & ctrl.f2fs_segment_mask) >>
            ctrl.segment_shift;

        if (current_zone != extents->extent[i].zone) {
            current_zone = extents->extent[i].zone;
            extents->zone_ctr++;
        }

        segment_start = (extents->extent[i].phy_blk & ctrl.f2fs_segment_mask);

        // if the beginning of the extent and the ending of the extent are in
        // the same segment
        if (segment_start == ((extents->extent[i].phy_blk +
                               extents->extent[i].len) &
                              ctrl.f2fs_segment_mask)) {
            if (segment_id != ctrl.cur_segment) {
                ctrl.cur_segment = segment_id;
            }

            set_segment_counters(segment_start >> ctrl.segment_shift, 1);
        } else {
            // Else the extent spans across multiple segments, so we need to
            // break it up

            // part 1: the beginning of extent to end of that single segment
            if (extents->extent[i].phy_blk != segment_start) {
                set_segment_counters(segment_start >> ctrl.segment_shift, 1);
                segment_id++;
            }

            // part 2: all in between segments after the 1st segment and the
            // last (in case the last is only partially used by the segment)
            segment_end = (extents->extent[i].phy_blk + extents->extent[i].len) &
                 ctrl.f2fs_segment_mask;
            num_segments = (segment_end - segment_start) >> ctrl.segment_shift;
            set_segment_counters(segment_start >> ctrl.segment_shift, num_segments);

            // part 3: any remaining parts of the last segment, which do not
            // fill the entire last segment only if the segment actually has a
            // remaining fragment
            segment_end = ((extents->extent[i].phy_blk + extents->extent[i].len) & ctrl.f2fs_segment_mask);
            if (segment_end != extents->extent[i].phy_blk + extents->extent[i].len) {
                set_segment_counters(segment_end >> ctrl.segment_shift, 1);
            }
        }
    }


    FORMATTER
    MSG("%-50s | Number of Extents | Number of Occupied Segments | Number of Occupied Zones | Cold Segments | Warm Segments | Hot Segments\n", "Filename");
    FORMATTER
    MSG("%-50s | %-17u | %-27u | %-24u | %-13u | %-13u | %-13u\n", workload.filename, extents->ext_ctr, wl_man.segment_ctr, extents->zone_ctr, wl_man.cold_ctr, wl_man.warm_ctr, wl_man.hot_ctr);
}

static void run_workloads() {
    struct extent_map *extents;

    wl_man.data_fd = open("/dev/urandom", O_RDONLY);
    if (!wl_man.data_fd) {
        ERR_MSG("Failed opening /dev/urandom for data generation\n");
    }

    for (uint16_t i = 0; i < wl_man.nr_wls; i++) {
        write_file(wl_man.wl[i]);
        ctrl.filename = wl_man.wl[i].filename;
        ctrl.fd = open(ctrl.filename, O_RDONLY);
        ctrl.stats = calloc(sizeof(struct stat), sizeof(char *));

        if (fstat(ctrl.fd, ctrl.stats) < 0) {
            ERR_MSG("Failed stat on file %s\n", ctrl.filename);
        }

        extents = (struct extent_map *) get_extents();
        close(ctrl.fd);

        sort_extents(extents);

        print_report(wl_man.wl[i], extents);

        free(extents);
    }

    close(wl_man.data_fd);
}

static uint64_t get_integer_value(char *optarg) {
    uint32_t multiplier = 1;

    if (optarg[strlen(optarg) - 1] == 'B') {
        multiplier = 1;
    } else if (optarg[strlen(optarg) - 1] == 'K') {
        multiplier = 1024;
    } else if (optarg[strlen(optarg) - 1] == 'M') {
        multiplier = 1024 * 1024;
    } else if (optarg[strlen(optarg) - 1] == 'G') {
        multiplier = 1024 * 1024 * 1024;
    } else {
        return atoi(optarg);
    }

    return atoi(optarg) * multiplier;
}

int main(int argc, char *argv[]) {
    int c;
    char *root_dir;

    wl_man.wl = calloc(sizeof(struct workload), 1);
    wl_man.wl[0].bsize = BLOCK_SZ;
    wl_man.wl[0].fsize = BLOCK_SZ;

    while ((c = getopt(argc, argv, "b:f:l:hn:s:w:")) != -1) {
        switch (c) {
        case 'h':
            show_help();
            break;
        case 'f':
            wl_man.wl[0].filename = optarg;
            wl_man.nr_wls = 1;
            break;
        case 's':
            wl_man.wl[0].fsize = get_integer_value(optarg);
            break;
        case 'b':
            wl_man.wl[0].bsize = get_integer_value(optarg);
            break;
        case 'n':
            wl_man.nr_jobs = atoi(optarg);
            break;
        case 'w':
            wl_man.wl[0].rw_hint = atoi(optarg);
            break;
        case 'l':
            ctrl.log_level = atoi(optarg);
            break;
        default:
            show_help();
            abort();
        }
    }

    if (wl_man.nr_jobs == 0) {
        check_options();
        char *root = strrchr(wl_man.wl[0].filename, '/');
        root_dir = malloc(root - wl_man.wl[0].filename);
        strncpy(root_dir, wl_man.wl[0].filename, root - wl_man.wl[0].filename);
    }

    struct stat stats;

    if (stat(root_dir, &stats) < 0) {
        ERR_MSG("Failed stat on dir %s\n", root_dir);
    }

    if (!S_ISDIR(stats.st_mode)) {
        ERR_MSG("%s is not a directory\n", root_dir);
    }

    init_dev(&stats);

    f2fs_read_super_block(ctrl.bdev.dev_path);
    set_super_block_info(f2fs_sb);

    ctrl.multi_dev = 1;
    ctrl.offset = get_dev_size(ctrl.bdev.dev_path);
    ctrl.znsdev.zone_size = get_zone_size();
    ctrl.znsdev.nr_zones = get_nr_zones();

    run_workloads();

    cleanup_ctrl();

    free(wl_man.wl);

    return 0;
}