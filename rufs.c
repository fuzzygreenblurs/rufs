/*
 *  Copyright (C) 2025 CS416 Rutgers CS
 *	Rutgers Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];
struct superblock* sb;
bitmap_t ibm;
bitmap_t dbm;

// Declare your in-memory data structures here
// BLOCK_SIZE
/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	
	// Step 2: Traverse inode bitmap to find an available slot
	// Step 3: Update inode bitmap and write to disk 

	return 0;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	
	// Step 2: Traverse data block bitmap to find an available slot

	// Step 3: Update data block bitmap and write to disk 

	return 0;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	// Step 1: Get the inode's on-disk block number

	// Step 2: Get offset of the inode in the inode on-disk block

	// Step 3: Read the block from disk and then copy into inode structure

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	// Step 1: Get the block number where this inode resides on disk
	
	// Step 2: Get the offset in the block where this inode resides on disk

	// Step 3: Write inode to disk 

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)

	// Step 2: Get data block of current directory from inode

	// Step 3: Read directory's data block and check each directory entry.
	// If the name matches, then copy directory entry to dirent structure

	return 0;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	
	// Step 2: Check if fname (directory name) is already used in other entries

	// Step 3: Add directory entry in dir_inode's data block and write to disk

	// Allocate a new data block for this directory if it does not exist

	// Update directory inode

	// Write directory entry

	return 0;
}


/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	return 0;
}

/* 
 * Make file system
 */
int rufs_mkfs() {

	// call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);
	
	// write superblock information
	uint32_t ibm_start  = 1;
	uint32_t dbm_start  = ibm_start  + ((MAX_INUM + (8 * BLOCK_SIZE) - 1) / (8 * BLOCK_SIZE)); 
	uint32_t itbl_start = dbm_start  + ((MAX_DNUM + (8 * BLOCK_SIZE) - 1) / (8 * BLOCK_SIZE));
	uint32_t dblk_start = itbl_start + ((MAX_INUM * (sizeof(struct inode)) + BLOCK_SIZE - 1) / BLOCK_SIZE);

	sb = {
		.magic_num    = MAGIC_NUM,
		.max_inum     = MAX_INUM,
		.max_dnum     = MAX_DNUM,
		.i_bitmap_blk = ibm_start,
		.d_bitmap_blk = dbm_start,	
		.i_start_blk  = itbl_start,
		.d_start_blk  = dblk_start		
	};
	bio_write(0, &sb);

	// initialize inode bitmap
	ibm = malloc((MAX_INUM + 7) / 8);
	memset(ibm, 0, ((MAX_INUM + 7) / 8));
	bio_write(ibm_start, ibm);

	// initialize data block bitmap
	dbm = malloc((MAX_DNUM + 7) / 8);
	memset(dbm, 0, (MAX_DNUM + 7) / 8);
	bio_write(dbm_start, dbm);
	
	// update bitmap information for root directory
	set_bitmap(ibm, 0);
	bio_write(ibm_start, ibm);

	set_bitmap(dbm, 0);
	bio_write(dbm_start, dbm);

	// update inode for root directory
	struct inode root_inode = {
		.ino   = 0,		// inode number
		.valid = 1,		
		.size  = 2 * sizeof(struct dirent),
		.type  = S_IFDIR,	// is directory type
		.link = 2,		//link count: . and .. 
		.direct_ptr[0] = dblk_start
	};
	
	time(&root_inode.vstat.st_mtime);	// last modified timestamp
	time(&root_inode.vstat.st_atime);	// last accessed timestamp
	bio_write(itbl_start, &root_inode);	

	// creating the two default entries for the root directory
	struct dirent root_entries[2];
	memset(root_entries, 0, sizeof(root_entries));

	root_entries[0].ino = 0;
	root_entries[0].valid = 1;
	strcpy(root_entries[0].name, ".");
	root_entries[0].len = 1;

	root_entries[1].ino = 0;
	root_entries[1].valid = 1;
	strcpy(root_entries[1].name, "..");
	root_entries[1].len = 2;

	bio_write(dblk_start, root_entries);

	return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
	if(dev_open(diskfile_path) < 0) rufs_mkfs();

	sb  = malloc(sizeof(struct superblock));
	bio_read(0, sb);
	if(sb->magic_num != MAGIC_NUM) {
		rufs_mkfs();
		dev_open(diskfile_path);
		bio_read(0, sb);
	}

	ibm = malloc((MAX_INUM + 7) / 8);
	dbm = malloc((MAX_DNUM + 7) / 8);
	bio_read(sb->i_bitmap_blk, ibm);
	bio_read(sb->d_bitmap_blk, dbm);

	return NULL;
}

static void rufs_destroy(void *userdata) {
	free(sb);
	free(ibm);
	free(dbm);

	dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

	stbuf->st_mode   = S_IFDIR | 0755;
	stbuf->st_nlink  = 2;
	time(&stbuf->st_mtime);

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk
	
	return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}


/* 
 * Functions you DO NOT need to implement for this project
 * (stubs provided for completeness)
 */

static int rufs_rmdir(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_unlink(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.mkdir		= rufs_mkdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,

	//Operations that you don't have to implement.
	.rmdir		= rufs_rmdir,
	.releasedir	= rufs_releasedir,
	.unlink		= rufs_unlink,
	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

