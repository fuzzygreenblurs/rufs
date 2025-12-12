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
	// search in-mem inode bmap for free inode
	int free_inode = -1;
	for(int i = 0; i < MAX_INUM; i++) {
		if(get_bitmap(ibm, i) == 0) {
			free_inode = i;
			break;
		}
	}
	if(free_inode == -1) return -1;

	// update inode bitmap and write to disk 
	set_bitmap(ibm, free_inode);
	bio_write(sb->i_bitmap_blk, ibm);
	return free_inode;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {
	// search in-mem data block bmap for free inode
	int free_dblock = -1;
	for(int i = 0; i < MAX_DNUM; i++) {
		if(get_bitmap(dbm, i) == 0) {
			free_dblock = i;
			break;
		}
	}
	if(free_dblock == -1) return -1;

	// update inode bitmap and write to disk 
	set_bitmap(dbm, free_dblock);
	bio_write(sb->d_bitmap_blk, dbm);
	return sb->d_start_blk + free_dblock;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
	// find the parent inode table block and its offset within that block
	uint32_t blk_idx = sb->i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
	uint32_t offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
	
	// disk can only be read from in block-sized chunks
	// the specific inode within that block must then be copied into the target struct
	char buffer[BLOCK_SIZE];
	bio_read(blk_idx, buffer);
	memcpy(inode, buffer + offset, sizeof(struct inode));

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	// find the parent inode table block and its offset within that block
	uint32_t blk_idx = sb->i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
	uint32_t offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
	
	// disk can only be written to in block-sized chunks
	// the target inode is updated in memory and then the whole block is updated on disk  
	// opposite buffer copy direction compared to readi
	char buffer[BLOCK_SIZE];
	bio_read(blk_idx, buffer);
	memcpy(buffer + offset, inode, sizeof(struct inode));

	bio_write(blk_idx, buffer);

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	// note: dir_find only looks up immediate subdirectories of a parent directory
	// recursive look ups are not handled here

	struct inode dir_inode;
	char buffer[BLOCK_SIZE]; 
	uint32_t num_dirents = BLOCK_SIZE / sizeof(struct dirent);

	// given a target dir (by inode number), read it from disk into memory
	readi(ino, &dir_inode);

	// loop through each of its direct pointers
	// each direct pointer points to a correponding data block
	for(int i = 0; i < 16; i++) {
		if(dir_inode.direct_ptr[i] == 0) break;

		// copy the data block into an in-mem buffer 
		bio_read(dir_inode.direct_ptr[i], buffer);

		// parse through the buffer in dirent sized units
		// this allows to index into the buffer using pointer arithmetic
		struct dirent* dirents = (struct dirent*)buffer;
	
		// perform the lookup against each of valid directory entry for that block 
		for(uint32_t j = 0; j < num_dirents; j++) {
			if(dirents[j].valid == 0) continue;

			// if there is a match, copy into the desired dirent in-mem buffer
			if(dirents[j].len == name_len &&
			   strncmp(dirents[j].name, fname, name_len) == 0) {
				memcpy(dirent, &dirents[j], sizeof(struct dirent));
				return 0;
			}
		}
	}

	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	char buffer[BLOCK_SIZE]; 
	uint32_t num_dirents = BLOCK_SIZE / sizeof(struct dirent);

	// loop through each of its data blocks to find an empty dirent slot
	for(int i = 0; i < 16; i++) {
		if(dir_inode.direct_ptr[i] == 0) break;	

		// copy the data block into an in-mem buffer 
		bio_read(dir_inode.direct_ptr[i], buffer);
		
		// search for a free slot in the block
		struct dirent* dirents = (struct dirent*)buffer;
		for(uint32_t j = 0; j < num_dirents; j++) {
			if(dirents[j].valid == 0) {
				// update existing invalid direntry in in-mem block buffer
				dirents[j].ino = f_ino;
				dirents[j].valid = 1;
				strncpy(dirents[j].name, fname, name_len);
				dirents[j].name[name_len] = '\0';
				dirents[j].len = name_len;

				// note: buffer is cast as pointer when passed to fn 
				bio_write(dir_inode.direct_ptr[i], buffer);
				
				// update inode disk record with new size 
				dir_inode.size += sizeof(struct dirent);
				writei(dir_inode.ino, &dir_inode);
				
				return 0;
			}
		}
	}

	// at this point, no free block found: allocate new block
	int dblk = get_avail_blkno();
	int slot_available = 0;
	for(int k = 0; k < 16; k++) {
		if(dir_inode.direct_ptr[k] == 0) {
			dir_inode.direct_ptr[k] = dblk;	
			slot_available = 1;
			break;
		}
	}
	
	// small file assumption: if the inode direct pointer list is full, error out
	if(!slot_available) return -1;

	// create new direntry in the data block
	memset(buffer, 0, BLOCK_SIZE);
	struct dirent* dirents = (struct dirent*)buffer;

	dirents[0].ino = f_ino;
	dirents[0].valid = 1;
	strncpy(dirents[0].name, fname, name_len);
	dirents[0].name[name_len] = '\0';
	dirents[0].len = name_len;

	bio_write(dblk, buffer);
	
	// update parent inode with new size
	dir_inode.size += sizeof(struct dirent);
	writei(dir_inode.ino, &dir_inode);

	return 0;
}


/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	// given a filepath as a string, return the corresponding inode (if valid filepath)	
	
	// if input path is root, return the root inode directly
	if(strcmp(path, "/") == 0) { 
		readi(0, inode);
		return 0;
	}

	// otherwise, begin from the specified inode
	struct inode current;
	readi(ino, &current);

	// use strtok (string-to_token) to parse the path using the "/" as a delimiter
	// ref: https://man7.org/linux/man-pages/man3/strtok.3p.html
	char mut_path[strlen(path) + 1];
	// otherwise, begin from the specified inode;
	strcpy(mut_path, path);
	char* token = strtok(mut_path, "/");

	// scan for terminal entry: use direct loops over recursion to improve space complexity 
	while(token) {
		struct dirent entry;
	
		// look up the token against all the current inode's directory entries
		if(dir_find(current.ino, token, strlen(token), &entry) < 0) return -1;
		
		// if a corresponding directory entry is found, begin looking in its subdirectories
		readi(entry.ino, &current);
		token = strtok(NULL, "/");
		
	}

	memcpy(inode, &current, sizeof(struct inode));
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
	root_inode.vstat.st_uid = getuid();
	root_inode.vstat.st_gid = getgid();
	root_inode.vstat.st_mode = S_IFDIR | 0755; // see pg.11 (faq) of project spec
	root_inode.vstat.st_nlink = 2;

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
	if(dev_open(diskfile_path) < 0) rufs_mkfs();;

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
	
	// find the corresponding inode 
	struct inode target;
	int ret = get_node_by_path(path, 0, &target);
	if(ret < 0) return -ENOENT;


	// populate stbuf with the requisite fields:
	// from pg.4, 10 of project spec: st_uid, st_gid, st_nlink, st_size, st_mtime, st_atime , and st_mode
	
	stbuf->st_size   = target.size;
	stbuf->st_uid    = target.vstat.st_uid;
	stbuf->st_gid    = target.vstat.st_gid;
	stbuf->st_nlink  = target.vstat.st_nlink;
	stbuf->st_mtime  = target.vstat.st_mtime;
	stbuf->st_atime  = target.vstat.st_atime;
	stbuf->st_mode   = target.vstat.st_mode;

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

