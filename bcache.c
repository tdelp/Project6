/*
This is the implementation of the buffer cache module.
You should make all of your changes here.
Feel free to add any structures, types or helper functions that you need.
*/

#include "bcache.h"
#include "disk.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

/*
A structure describing the entire buffer cache.
You may add and modify this structure as needed.
*/

#include "bcache.h"
#include "disk.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    BLOCK_FREE,      // Block is free and not currently being used.
    BLOCK_READING,   // Block is being read from disk.
    BLOCK_READY,     // Block contains valid data and is ready for access.
    BLOCK_DIRTY,     // Block has been modified and needs to be written back to disk.
    BLOCK_WRITING    // Block is being written to disk.
} block_state;

struct block {
    int blocknum;                // Disk block number
    block_state state;           // Current state of the block
    char data[4096];             // Data storage for the block
    pthread_mutex_t lock;        // Protects this block structure
    pthread_cond_t cond;         // Condition variable for state changes
    struct block *next;          // Next block in the linked list
};

struct bcache {
    struct disk *disk;           // The disk object underlying the cache.
    struct block *cache;         // Pointer to the first block in the cache.
    int memory_blocks;           // The total number of memory blocks in the cache.
    int nreads;                  // A running count of read operations.
    int nwrites;                 // A running count of write operations.
    pthread_mutex_t cache_lock;  // Mutex for protecting access to the cache.
    pthread_mutex_t disk_lock;   // Mutex for serializing access to the disk.
};

struct bcache *bcache_create(struct disk *d, int memory_blocks) {
    struct bcache *bc = malloc(sizeof(*bc));
    if (!bc) {
        fprintf(stderr, "Failed to allocate memory for buffer cache.\n");
        return NULL;
    }

    bc->disk = d;
    bc->memory_blocks = memory_blocks;
    bc->nreads = 0;
    bc->nwrites = 0;
    bc->cache = NULL;

    pthread_mutex_init(&bc->cache_lock, NULL);
    pthread_mutex_init(&bc->disk_lock, NULL);

    return bc;
}

struct block *find_or_create_block(struct bcache *bc, int blocknum) {
    struct block *blk = bc->cache;
    while (blk) {
        if (blk->blocknum == blocknum) return blk;
        blk = blk->next;
    }

    // Create new block if not found
    blk = malloc(sizeof(struct block));
    if (!blk) {
        fprintf(stderr, "Failed to allocate block.\n");
        return NULL;
    }
    blk->blocknum = blocknum;
    blk->state = BLOCK_FREE;
    pthread_mutex_init(&blk->lock, NULL);
    pthread_cond_init(&blk->cond, NULL);
    blk->next = bc->cache;
    bc->cache = blk;
    return blk;
}

void bcache_read(struct bcache *bc, int blocknum, char *data) {
    struct block *blk = NULL;

    pthread_mutex_lock(&bc->cache_lock);
    blk = find_or_create_block(bc, blocknum);
    pthread_mutex_unlock(&bc->cache_lock);

    pthread_mutex_lock(&blk->lock);
    if (blk->state == BLOCK_FREE || blk->state == BLOCK_DIRTY) {
        blk->state = BLOCK_READING;
        pthread_mutex_unlock(&blk->lock);

        pthread_mutex_lock(&bc->disk_lock);
        disk_read(bc->disk, blocknum, blk->data);
        pthread_mutex_unlock(&bc->disk_lock);

        pthread_mutex_lock(&blk->lock);
        blk->state = BLOCK_READY;
        pthread_cond_broadcast(&blk->cond);
    }

    while (blk->state != BLOCK_READY) {
        pthread_cond_wait(&blk->cond, &blk->lock);
    }

    memcpy(data, blk->data, 4096);
    pthread_mutex_unlock(&blk->lock);
    __sync_fetch_and_add(&bc->nreads, 1);
}

void bcache_write(struct bcache *bc, int blocknum, const char *data) {
    pthread_mutex_lock(&bc->cache_lock);
    struct block *blk = find_or_create_block(bc, blocknum);
    pthread_mutex_unlock(&bc->cache_lock);

    pthread_mutex_lock(&blk->lock);
    memcpy(blk->data, data, 4096);
    blk->state = BLOCK_DIRTY;
    pthread_mutex_unlock(&blk->lock);
    __sync_fetch_and_add(&bc->nwrites, 1);
}

void bcache_sync(struct bcache *bc) {
    struct block *blk;
    pthread_mutex_lock(&bc->cache_lock);
    blk = bc->cache;
    while (blk) {
        pthread_mutex_lock(&blk->lock);
        if (blk->state == BLOCK_DIRTY) {
            blk->state = BLOCK_WRITING;
            pthread_mutex_unlock(&blk->lock);
            pthread_mutex_lock(&bc->disk_lock);
            disk_write(bc->disk, blk->blocknum, blk->data);
            pthread_mutex_unlock(&bc->disk_lock);
            pthread_mutex_lock(&blk->lock);
            blk->state = BLOCK_READY;
        }
        pthread_cond_broadcast(&blk->cond);
        pthread_mutex_unlock(&blk->lock);
        blk = blk->next;
    }
    pthread_mutex_unlock(&bc->cache_lock);
}

void *bcache_io_scheduler(void *vbc) {
    struct bcache *bc = (struct bcache *)vbc;
    while (1) {
        struct block *blk = NULL;
        int found = 0;

        pthread_mutex_lock(&bc->cache_lock);
        for (blk = bc->cache; blk != NULL; blk = blk->next) {
            pthread_mutex_lock(&blk->lock);
            if (blk->state == BLOCK_DIRTY) {
                blk->state = BLOCK_WRITING;
                pthread_mutex_unlock(&blk->lock);

                pthread_mutex_lock(&bc->disk_lock);
                disk_write(bc->disk, blk->blocknum, blk->data);
                pthread_mutex_unlock(&bc->disk_lock);

                pthread_mutex_lock(&blk->lock);
                blk->state = BLOCK_READY;
                pthread_cond_broadcast(&blk->cond);
                found = 1;
            }
            pthread_mutex_unlock(&blk->lock);
            if (found) break;
        }
        pthread_mutex_unlock(&bc->cache_lock);

        if (!found) {
            sched_yield();  // Yield the processor to reduce busy waiting
        }
    }
    return NULL;
}

/*
These functions just return basic information about the buffer cache,
and you shouldn't need to change them.
*/

/* Return the number of memory blocks in the buffer cache. */

int bcache_memory_blocks( struct bcache *bc )
{
	return bc->memory_blocks;
}

/* Return the number of blocks in the underlying disk. */

int bcache_disk_blocks( struct bcache *bc )
{
	return disk_nblocks(bc->disk);
}

/* Return the number of reads performed on this buffer cache. */

int bcache_nreads( struct bcache *bc )
{
	return bc->nreads;
}

/* Return the number of writes performed on this buffer cache. */

int bcache_nwrites( struct bcache *bc )
{
	return bc->nwrites;
}