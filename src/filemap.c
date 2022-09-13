#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <linux/blkzoned.h>
#include <linux/fiemap.h>
#include "filemap.h"
#include "control.h"

#define SECTOR_SHIFT 9

/* 
 * F2FS treats all devices as one address space, therefore if we use
 * a conventional device and a ZNS, extent maps contain the offset within
 * this total address space. For instance, with a 4GB conventional device
 * and a ZNS the offsets in the ZNS address space are +4GB. Therefore, we
 * need to find the size of the conventional space and subtract it from
 * the extent mapping offsets.
 *
 * TODO: This would only work with a single ZNS after a conventional device!
 *       For multiple it would require maintaining sizes of each ZNS, and 
 *       their order in mkfs.f2fs call (to know the F2FS address space in 
 *       correct order and calculate valid zone address ranges).
 * */
uint64_t offset = 0;

/* 
 * Calculate the zone number (starting with zone 1) of an LBA
 *
 * @lba: LBA to calculate zone number of
 * @zone_size: size of the zone
 *
 * returns: uint32_t of zone number (starting with 1)
 *
 * */
uint32_t get_zone_number(uint64_t lba, uint64_t zone_size) {
    uint64_t zone_mask = 0;
    uint64_t slba = 0;

    zone_mask = ~(zone_size - 1);
    slba = (lba & zone_mask);

    return slba == 0 ? 1 : (slba / zone_size + 1);
}


uint64_t print_zone_info(struct bdev* znsdev, uint32_t zone) {
    unsigned long long start_sector = 0;
    struct blk_zone_report *hdr = NULL;
    uint32_t zone_mask;
    uint64_t zone_wp = 0;

    start_sector = znsdev->zone_size * zone - znsdev->zone_size;

    int fd = open(znsdev->dev_path, O_RDONLY);
    if (fd < 0) {
        return -1 + 1;
    }

    hdr = calloc(1, sizeof(struct blk_zone_report) + sizeof(struct blk_zone));
    hdr->sector = start_sector;
    hdr->nr_zones = 1;

    if (ioctl(fd, BLKREPORTZONE, hdr) < 0) {
        fprintf(stderr, "\033[0;31mError\033[0m getting Zone Info\n");
        return -1;
    }

    zone_mask = ~(znsdev->zone_size - 1);
    zone_wp = hdr->zones[0].wp;
    printf("\n**** ZONE %d ****\n", zone);
    printf("LBAS: 0x%06llx  LBAE: 0x%06llx  CAP: 0x%06llx  WP: 0x%06llx  SIZE: "
            "0x%06llx  STATE: %#-4x  MASK: 0x%06"PRIx32"\n\n", hdr->zones[0].start,
            hdr->zones[0].start + hdr->zones[0].capacity, hdr->zones[0].capacity, 
            hdr->zones[0].wp, hdr->zones[0].len, hdr->zones[0].cond << 4, zone_mask);

    close(fd);

    free(hdr);
    hdr = NULL;

    return zone_wp;
}


/* 
 * Get the file extents with FIEMAP ioctl
 *
 * @fd: file descriptor of the file
 * @dev_name: name of the device (e.g., nvme0n2)
 * @stats: struct stat of fd
 *
 * returns: struct extent_map * to the extent maps. 
 *          NULL returned on Failure
 * 
 * */
struct extent_map * get_extents(struct control *ctrl) {
    struct fiemap *fiemap;
    struct extent_map *extent_map;
    uint8_t last_ext = 0;

    fiemap = (struct fiemap *) calloc(sizeof(struct fiemap), sizeof (char *));
    extent_map = (struct extent_map *) calloc(sizeof(struct extent_map) + sizeof(struct extent), sizeof(char *));

    fiemap->fm_flags = FIEMAP_FLAG_SYNC;
    fiemap->fm_start = 0;
    fiemap->fm_extent_count = 1; // get extents individually
    fiemap->fm_length = (ctrl->stats->st_blocks << SECTOR_SHIFT);

    do {
        if (extent_map->ext_ctr > 0) {
            extent_map = realloc(extent_map, sizeof(struct extent_map) + sizeof(struct extent) * (extent_map->ext_ctr + 1));
        }

        if (ioctl(ctrl->fd, FS_IOC_FIEMAP, fiemap) < 0) {
            return NULL;
        }

        if (fiemap->fm_mapped_extents == 0) {
            fprintf(stderr, "\033[0;31mError\033[0m no extents are mapped\n");
            return NULL;
        }

        extent_map->extent[extent_map->ext_ctr].phy_blk = (fiemap->fm_extents[0].fe_physical - offset) >> SECTOR_SHIFT;
        extent_map->extent[extent_map->ext_ctr].logical_blk = fiemap->fm_extents[0].fe_logical >> SECTOR_SHIFT;
        extent_map->extent[extent_map->ext_ctr].len = fiemap->fm_extents[0].fe_length >> SECTOR_SHIFT;
        extent_map->extent[extent_map->ext_ctr].zone_size = ctrl->znsdev->zone_size;
        extent_map->extent[extent_map->ext_ctr].ext_nr = extent_map->ext_ctr;

        extent_map->cum_extent_size += extent_map->extent[extent_map->ext_ctr].len;

        if (fiemap->fm_extents[0].fe_flags & FIEMAP_EXTENT_LAST) {
            last_ext = 1;
        }

        extent_map->extent[extent_map->ext_ctr].zone = get_zone_number(((fiemap->fm_extents[0].fe_physical
                        - offset) >> SECTOR_SHIFT), extent_map->extent[extent_map->ext_ctr].zone_size);

        extent_map->ext_ctr++;
        fiemap->fm_start = ((fiemap->fm_extents[0].fe_logical) + (fiemap->fm_extents[0].fe_length));
    } while (last_ext == 0);

    free(fiemap);
    fiemap = NULL;

    return extent_map;
}


/* 
 * Check if an element is contained in the array.
 *
 * @list[]: uint32_t array of items to check.
 * @element: uint32_t element to find.
 * @size: size of the uint32_t array.
 *
 * returns: 1 if contained otherwise 0.
 *
 * */
int contains_element(uint32_t list[], uint32_t element, uint32_t size) {

    for (uint32_t i = 0; i < size; i++) {
        if (list[i] == element) {
            return 1;
        }
    } 

    return 0;
}


/* 
 * Sort the provided extent maps based on their PBAS.
 *
 * Note: extent_map->extent[x].ext_nr still shows the return
 * order of the extents by ioctl, hence depicting the logical
 * file data order.
 *
 * @extent_map: pointer to extent map struct
 *
 *
 * */
void sort_extents(struct extent_map *extent_map) {
    struct extent *temp;
    uint32_t cur_lowest = 0;
    uint32_t used_ind[extent_map->ext_ctr];

    temp = calloc(sizeof(struct extent) * extent_map->ext_ctr, sizeof(char *));

    for (uint32_t i = 0; i < extent_map->ext_ctr; i++) {
        for (uint32_t j = 0; j < extent_map->ext_ctr; j++) {
            if (extent_map->extent[j].phy_blk < extent_map->extent[cur_lowest].phy_blk && 
                    !contains_element(used_ind, j, i)) {
                cur_lowest = j;
            } else if (contains_element(used_ind, cur_lowest, i)) {
                cur_lowest = j;
            }
        }
        used_ind[i] = cur_lowest;
        temp[i] = extent_map->extent[cur_lowest];
    }

    memcpy(extent_map->extent, temp, sizeof(struct extent) * extent_map->ext_ctr);

    free(temp);
    temp = NULL;
}


static void show_info(int show_hole_info) {
    printf("LBAS:   Logical Block Address Start (for the Zone)\n");
    printf("LBAE:   Logical Block Address End (for the Zone, equal to LBAS + ZONE CAP)\n");
    printf("CAP:    Zone Capacity (in 512B sectors)\n");
    printf("WP:     Write Pointer of the Zone\n");
    printf("SIZE:   Size of the Zone (in 512B sectors)\n");
    printf("STATE:  State of a zone (e.g, FULL, EMPTY)\n");
    printf("MASK:   The Zone Mask that is used to calculate LBAS of LBA addresses in a zone\n");

    printf("EXTID:  Extent number in the order of the extents returned by ioctl(), depciting logical file data ordering\n");
    printf("PBAS:   Physical Block Address Start\n");
    printf("PBAE:   Physical Block Address End\n");

    printf("NOE:    Number of Extent\n");
    printf("TES:    Total Extent Size (in 512B sectors\n");
    printf("AES:    Average Extent Size (floored value due to hex print, in 512B sectors)\n"
            "\t[Meant for easier comparison with Extent Sizes\n");
    printf("EAES:   Exact Average Extent Size (double point precision value, in 512B sectors\n"
            "\t[Meant for exact calculations of average extent sizes\n");
    printf("NOZ:    Number of Zones (in which extents are\n");

    if(show_hole_info) {
        printf("NOH:    Number of Holes\n");
        printf("THS:    Total Hole Size (in 512B sectors\n");
        printf("AHS:    Average Hole Size (floored value due to hex print, in 512B sectors)\n");
        printf("EAHS:   Exact Average Hole Size (double point precision value, in 512B sectors\n");
    }
}

/* 
 * Print the report summary of extent_map. 
 *
 * @dev_name: char * to the device name (e.g., nvme0n2)
 * @extent_map: struct extent_map * to the extent maps 
 * 
 * */
void print_extent_report(struct control *ctrl, struct extent_map *extent_map) {
    uint32_t current_zone = 0;
    uint32_t hole_ctr = 0;
    uint64_t hole_cum_size = 0;
    uint64_t hole_size = 0; 

    if (ctrl->info) {
        printf("\n====================================================================\n");
        printf("\t\t\tACRONYM INFO\n");
        printf("====================================================================\n");
        printf("\nInfo: Extents are sorted by PBAS but have an associated "
                "Extent Number to indicate the logical order of file data.\n\n");
        show_info(ctrl->show_holes);
    }

    printf("\n====================================================================\n");
    printf("\t\t\tEXTENT MAPPINGS\n");
    printf("====================================================================\n");

    for (uint32_t i = 0; i < extent_map->ext_ctr; i++) {
        if (current_zone != extent_map->extent[i].zone) {
            current_zone = extent_map->extent[i].zone;
            extent_map->zone_ctr++;
            extent_map->extent[i].zone_wp = print_zone_info(ctrl->znsdev, current_zone); 
        }

        // Track holes in between extents in the same zone
        if (ctrl->show_holes && i > 0 && (extent_map->extent[i - 1].phy_blk + extent_map->extent[i - 1].len 
                    != extent_map->extent[i].phy_blk)) {

            if (extent_map->extent[i - 1].zone == extent_map->extent[i].zone) {
                // Hole in the same zone between segments

                hole_size = extent_map->extent[i].phy_blk - (extent_map->extent[i - 1].phy_blk + 
                        extent_map->extent[i - 1].len);
                hole_cum_size += hole_size;
                hole_ctr++;

                printf("--- HOLE:    PBAS: %#-10"PRIx64"  PBAE: %#-10"PRIx64"  SIZE: %#-10"PRIx64"\n",
                        extent_map->extent[i - 1].phy_blk + extent_map->extent[i - 1].len, 
                        extent_map->extent[i].phy_blk, hole_size);
            } 
        }

        printf("EXTID: %-4d  PBAS: %#-10"PRIx64"  PBAE: %#-10"PRIx64"  SIZE: %#-10"PRIx64"\n",
                extent_map->extent[i].ext_nr + 1, extent_map->extent[i].phy_blk, (extent_map->extent[i].phy_blk + 
                    extent_map->extent[i].len), extent_map->extent[i].len);

    }

    printf("\n====================================================================\n");
    printf("\t\t\tSTATS SUMMARY\n");
    printf("====================================================================\n");
    printf("\nNOE: %-4u  TES: %#-10"PRIx64"  AES: %#-10"PRIx64"  EAES: %-10f"
            "  NOZ: %-4u\n", extent_map->ext_ctr, extent_map->cum_extent_size, 
            extent_map->cum_extent_size / (extent_map->ext_ctr + 1),
            (double)extent_map->cum_extent_size / (double)(extent_map->ext_ctr + 1), extent_map->zone_ctr);

    if (ctrl->show_holes) {
        printf("NOH: %-4u  THS: %#-10"PRIx64"  AHS: %#-10"PRIx64"  EAHS: %-10f\n", 
                hole_ctr, hole_cum_size, hole_cum_size / hole_ctr, (double)hole_cum_size / (double)hole_ctr);
    }
}


int main(int argc, char *argv[]) {
    struct control *ctrl;
    struct extent_map *extent_map;

    ctrl = (struct control *) init_ctrl(argc, argv);

    if (!ctrl) {
        return 1;
    }

    extent_map = (struct extent_map *) get_extents(ctrl);

    if (!extent_map) {
        fprintf(stderr, "\033[0;31mError\033[0m retrieving extents for %s\n", ctrl->filename);
        return 1;
    }

    sort_extents(extent_map);
    print_extent_report(ctrl, extent_map);

    close(ctrl->fd);

    free(ctrl->bdev->dev_name);
    free(ctrl->bdev);
    if (ctrl->multi_dev) {
        free(ctrl->znsdev->dev_name);
        free(ctrl->znsdev);
    }
    free(ctrl->stats);
    free(ctrl);
    free(extent_map);

    return 0;
}