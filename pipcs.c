// pipcs.c - pipcs
// ipcs-like breakdown of POSIX and System V shared memory
//
// This program is licensed under the GNU Library General Public License, v2
//
// Zach Walton (zacwalt@gmail.com)

// exposes nftw()
#define _XOPEN_SOURCE 500
// exposes mincore()
#define _BSD_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <mntent.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <ftw.h>


//lifted from free.c - bitshift based on unit
#define S(X) ( ((unsigned long long)(X) << 10) >> shift)

const char help_message[] =
"usage: pipcs [-b|-k|-m|-g] [-s]\n"
"  default: show all\n"
"  -b,-k,-m,-g show output in bytes, KB, MB, or GB\n"
"  -s show shared memory breakdown\n"
;

//from ipcs.c
#ifndef SHM_STAT
#define SHM_STAT        13
#define SHM_INFO        14
struct shm_info {
	int used_ids;
	ulong shm_tot;		/* total allocated shm */
	ulong shm_rss;		/* total resident shm */
	ulong shm_swp;		/* total swapped shm */
	ulong swap_attempts;
	ulong swap_successes;
};
#endif

struct posix_shmem_stats {
        unsigned long posix_swp_pages;
        unsigned long posix_swp_total;
        unsigned long posix_res_pages;
        unsigned long posix_res_total;
        unsigned long posix_shm_total;
};

int print_shared(int shift, long PAGE_SIZE, char base);
int posix_shmem_callback(const char * fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf);

long get_posix_shmem();
long get_sysv_shmem(long PAGE_SIZE);

struct posix_shmem_stats posix_shmem_stat = {0,0,0,0,0};
struct shmid_ds shm_seg;
struct shm_info shm_info;

int main(int argc, char *argv[]) {
	long PAGE_SIZE = sysconf(_SC_PAGESIZE);
	int show_shared = 0;
	int opt = 0;
	int no_opt = 0;
	int shift = 10;
	char base = 'K';
	while( (opt = getopt(argc, argv, "bkmgs") ) != -1 )
	switch(opt) {
	    case 's': show_shared = 1; break;
            case 'b': shift = 0;  base=0x0;  break;
            case 'k': shift = 10; base='K'; break;
            case 'm': shift = 20; base='M'; break;
            case 'g': shift = 30; base='G'; break;
	    default:
	        fwrite(help_message,1,strlen(help_message),stderr);
                return 1;
	}
	if (show_shared) {
	    print_shared(shift, PAGE_SIZE, base);
	    no_opt = 1;
	}
	if (!no_opt) {
	    print_shared(shift, PAGE_SIZE, base);
	}
	return 0;
}

int print_shared(int shift, long PAGE_SIZE, char base) {

	/*
         * SYSV shared memory
         */

        long locked_pages = get_sysv_shmem(PAGE_SIZE);
	printf("\n------ SYS V Shared Memory ------\n"
	       "sysv_swp_pages: %ld\n"
	       "sysv_swp_total: %Ld%c\n"
	       "sysv_res_pages: %ld\n"
	       "sysv_res_total: %Ld%c\n"
	       "sysv_lck_pages: %ld\n"
	       "sysv_lck_total: %Ld%c\n"
	       "sysv_shm_total: %Ld%c\n",
	       shm_info.shm_swp,
	       S(shm_info.shm_swp*PAGE_SIZE/1024),
	       base,
	       shm_info.shm_rss,
	       S(shm_info.shm_rss*PAGE_SIZE/1024),
	       base,
	       locked_pages,
	       S(locked_pages*PAGE_SIZE/1024),
	       base,
	       // i have no idea why there's a discrepancy between
	       // res+swap vs total, but ipcs -u shows the same
	       // difference.  nice
	       S(shm_info.shm_tot*PAGE_SIZE/1024),
	       base);

	/*
	 * POSIX shared memory.  Should work in any post-2.4 kernel
	 */

	// populate posix_shmem_stats with posix shmem data
	printf("\n------ POSIX Shared Memory ------\n");
	get_posix_shmem();
	printf("posix_swp_pages: %ld\n"
	       "posix_swp_total: %Ld%c\n"
	       "posix_res_pages: %ld\n"
	       "posix_res_total: %Ld%c\n"
	       "posix_shm_total: %Ld%c\n",
	       posix_shmem_stat.posix_swp_pages,
	       S(posix_shmem_stat.posix_swp_total/1024),
	       base,
	       posix_shmem_stat.posix_res_pages,
	       S(posix_shmem_stat.posix_res_total/1024),
	       base,
	       S(posix_shmem_stat.posix_shm_total*PAGE_SIZE/1024),
	       base);
	printf("\n------ Total Shared Memory ------\n"
	       "total_swp_pages: %ld\n"
	       "total_res_pages: %ld\n"
	       "shm_total:       %Ld%c\n",
	       (shm_info.shm_swp + posix_shmem_stat.posix_swp_pages),
	       (shm_info.shm_rss + posix_shmem_stat.posix_res_pages),
	       S(shm_info.shm_tot*PAGE_SIZE/1024 + posix_shmem_stat.posix_shm_total*PAGE_SIZE/1024),
	       base);
	return 0;
}

long get_sysv_shmem(long PAGE_SIZE) {
	// populate shm_info with system v shmem data
        int maxid = shmctl(0, SHM_INFO, (struct shmid_ds *) (void *) &shm_info);
        int id, shmid;
        long locked_pages = 0;
        if (maxid < 0) {
            printf("kernel not configured for %s\n", "shared memory");
            return -1;
        }
	// iterate through all sys vshared memory to look for locked segments
        for (id=0; id<=maxid; id++) {
            if ((shmid = shmctl(id, SHM_STAT, &shm_seg)) < 0) {
                continue;
            }
            if (shm_seg.shm_perm.mode & SHM_LOCKED)
                locked_pages+=(shm_seg.shm_segsz/PAGE_SIZE);
        }
	return locked_pages;
}

/* 
 *
 * This is the callback function called by the ftw() directory tree walk in
 * get_posix_shmem().  Calls mincore() on each file to determine residency
 * of pages and increments totals in posix_shmem_stat
 *
 */
int posix_shmem_callback(const char * fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {

	// return if item is a directory
	if (typeflag != FTW_F)
	    return 0;
        int fd;
        long j;
	long res_pages=0;
        char * addr;
	const char *shm_path = (strstr(fpath,"/dev/shm"))?fpath+8:fpath;
	struct stat sb2;
	int PAGE_SIZE=sysconf(_SC_PAGESIZE);
	//for /dev/shm, use shm_open.  for other tmpfs filesystems, use open
        fd = (strstr(fpath,"/dev/shm"))?shm_open(shm_path, O_RDONLY, 0):open(shm_path,O_RDONLY,0);
	if (fd<0) {
	    fprintf(stderr, "Error: Unable to open file %s: %s\n", fpath, strerror(errno));
	    return 0;
	}
        if (fstat(fd, &sb2)<0) {
	    fprintf(stderr, "Error: Unable to stat file %s: %s\n", fpath, strerror(errno));
	    close(fd);
	    return 0;
	}
        addr = mmap(NULL, sb2.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr<0) {
	    fprintf(stderr, "Error: Unable to map %s into memory: %s\n", fpath, strerror(errno));
	    close(fd);
	    return 0;
	}
        unsigned char *vec = malloc((sb2.st_size + PAGE_SIZE -1)/PAGE_SIZE);
        if (mincore(addr, sb2.st_size, vec) < 0) {
	    fprintf(stderr, "Error: Unable to check page residency for %s: %s\n", fpath, strerror(errno));
	    close(fd);
	    munmap(addr, sb2.st_size);
	    return 0;
	}
        for (j=0; j<((sb2.st_size/PAGE_SIZE)+1); j++) {
            if (vec[j] & 0x1)
                res_pages++;
        }
        long swap_pages = sb2.st_size/PAGE_SIZE - res_pages;
	posix_shmem_stat.posix_res_pages+=res_pages;
	posix_shmem_stat.posix_swp_pages+=(swap_pages<0)?swap_pages*-1:swap_pages;
	posix_shmem_stat.posix_res_total+=(res_pages*PAGE_SIZE);
	posix_shmem_stat.posix_swp_total+=(((swap_pages<0)?swap_pages*-1:swap_pages)*PAGE_SIZE);
	posix_shmem_stat.posix_shm_total+=(((swap_pages<0)?swap_pages*-1:swap_pages)+res_pages);
        free(vec);
	munmap(addr, sb2.st_size);
	close(fd);
        return 0;
}

/*
 *
 * This iterates through tmpfs filesystems.  at
 * the end, posix_shmem_stat should be populated
 *
 */
long get_posix_shmem() {
	FILE *mounts = setmntent("/etc/mtab", "r");
	struct mntent *ent;
	while ((ent = getmntent(mounts)) != NULL) {
	    if (strcmp(ent->mnt_type, "tmpfs") == 0)
	        nftw(ent->mnt_dir, posix_shmem_callback, 100, FTW_MOUNT);
	}
	free(ent);
	endmntent(mounts);
	return 0;
}
