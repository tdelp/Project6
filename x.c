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
#include <stdbool.h>

/*
A structure describing the entire buffer cache.
You may add and modify this structure as needed.
*/

struct bcache {
    struct disk *disk;           // The disk object underlying the cache.
    struct block *cache;         // Pointer to the first block in the cache.
    int memory_blocks;           // The total number of memory blocks in the cache.
    int nreads;                  // A running count of read operations.
    int nwrites;                 // A running count of write operations.
    pthread_mutex_t cache_lock;  // Mutex for protecting access to the cache.
    pthread_mutex_t disk_lock;   // Mutex for serializing access to the disk.
};


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
    struct block* next;          // Next block in the linked list
};

/*
Create and initialize the buffer cache object.
You may modify this function as needed to set things up.
*/

struct bcache * bcache_create(struct disk *d, int memory_blocks) {
    struct bcache *bc = malloc(sizeof(*bc));
    if (!bc) {
        fprintf(stderr, "Failed to allocate memory for buffer cache.\n");
        return NULL;
    }

    bc->disk = d;
    bc->memory_blocks = memory_blocks;
    bc->nreads = 0;
    bc->nwrites = 0;
    bc->cache = NULL; // Initialize the cache pointer to NULL

    if (pthread_mutex_init(&bc->disk_lock, NULL) != 0) {
        fprintf(stderr, "Failed to initialize disk lock.\n");
        // Don't forget to destroy the already initialized mutexes before returning NULL
        pthread_mutex_destroy(&bc->cache_lock);
        // ... destroy any other initialized mutexes or condition variables ...
        free(bc);
        return NULL;
    }

    return bc;
}

struct block* find_block(struct bcache* bc, int blocknum) {
    struct block* current = bc->cache;
    while (current) {
        if (current->blocknum == blocknum && current->state != BLOCK_FREE) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void add_block_to_cache(struct bcache* bc, int blocknum, const char* data) {
    struct block* new_block = malloc(sizeof(struct block));
    if (!new_block) {
        fprintf(stderr, "Failed to allocate memory for a new block.\n");
        return;
    }
    new_block->blocknum = blocknum;
    memcpy(new_block->data, data, 4096);
    new_block->state = BLOCK_READY;
    pthread_mutex_init(&new_block->lock, NULL);
    pthread_cond_init(&new_block->cond, NULL);
    
    new_block->next = bc->cache;
    bc->cache = new_block;
}


/*
Read a block through the buffer cache.
This is a dummy implementation that calls disk_read directly.
It will work only for one thread, and it won't be particularly fast.
Instead, it should manipulate the data structure and wait for the scheduler.
*/

void bcache_read(struct bcache *bc, int blocknum, char *data) {
    struct block *blk = NULL;

    // Lock the cache to search for the block or add a new one.
    pthread_mutex_lock(&bc->cache_lock);
    blk = find_block(bc, blocknum);

    if (!blk) {
        // Block not found in cache, need to create and read it from disk
        blk = malloc(sizeof(struct block));
        if (!blk) {
            fprintf(stderr, "Failed to allocate block memory.\n");
            pthread_mutex_unlock(&bc->cache_lock);
            return;
        }
        memset(blk, 0, sizeof(struct block));
        blk->blocknum = blocknum;
        blk->state = BLOCK_READING;
        blk->next = bc->cache;
        bc->cache = blk;
        pthread_mutex_init(&blk->lock, NULL);
        pthread_cond_init(&blk->cond, NULL);
    }
    pthread_mutex_unlock(&bc->cache_lock);

    // Work with the block outside of cache lock to allow other threads to access the cache
    pthread_mutex_lock(&blk->lock);
    if (blk->state == BLOCK_READING) {
        // Ensure disk access is exclusive
        pthread_mutex_lock(&bc->disk_lock);
        disk_read(bc->disk, blocknum, blk->data);
        pthread_mutex_unlock(&bc->disk_lock);

        blk->state = BLOCK_READY;
        pthread_cond_broadcast(&blk->cond);
    }

    // If the block is not ready, wait until it is
    while (blk->state != BLOCK_READY) {
        pthread_cond_wait(&blk->cond, &blk->lock);
    }

    // Copy the data to the user's buffer
    memcpy(data, blk->data, 4096);
    pthread_mutex_unlock(&blk->lock);

    // Atomically increment the read count
    __sync_fetch_and_add(&bc->nreads, 1);
}



void bcache_write(struct bcache *bc, int blocknum, const char *data) {
    pthread_mutex_lock(&bc->cache_lock);
    struct block *blk = find_block(bc, blocknum);
    if (blk == NULL) {
        // Block not found in cache, create new block
        blk = malloc(sizeof(struct block));
        if (!blk) {
            fprintf(stderr, "Failed to allocate block memory.\n");
            pthread_mutex_unlock(&bc->cache_lock);
            return;
        }
        blk->blocknum = blocknum;
        blk->state = BLOCK_DIRTY;
        blk->next = bc->cache;
        bc->cache = blk;
        pthread_mutex_init(&blk->lock, NULL);
        pthread_cond_init(&blk->cond, NULL);
    }
    memcpy(blk->data, data, 4096); // Update block data
    blk->state = BLOCK_DIRTY;
    bc->nwrites++;
    pthread_mutex_unlock(&bc->cache_lock);
}


/*
Block until all dirty blocks in the buffer cache have been cleaned.
This needs to be implemented.
*/

void bcache_sync(struct bcache *bc) {
    pthread_mutex_lock(&bc->cache_lock);
    struct block *current = bc->cache;
    while (current) {
        pthread_mutex_lock(&current->lock);
        if (current->state == BLOCK_DIRTY) {
            current->state = BLOCK_WRITING;
            pthread_cond_wait(&current->cond, &current->lock); // Wait for the I/O scheduler to finish writing
        }
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }
    pthread_mutex_unlock(&bc->cache_lock);
}


/*
This is the function that will run the I/O scheduler.
This needs to be implemented.
*/

void *bcache_io_scheduler(void *vbc) {
    struct bcache *bc = (struct bcache *)vbc;
    struct block *blk;

    while (true) {  // This condition should be changed to a stop condition
        bool work_done = false;

        pthread_mutex_lock(&bc->cache_lock);
        for (blk = bc->cache; blk != NULL; blk = blk->next) {
            pthread_mutex_lock(&blk->lock);
            if (blk->state == BLOCK_READING || blk->state == BLOCK_WRITING) {
                pthread_mutex_unlock(&bc->cache_lock); // Unlock cache to avoid deadlock

                pthread_mutex_lock(&bc->disk_lock); // Ensure exclusive disk access
                if (blk->state == BLOCK_READING) {
                    disk_read(bc->disk, blk->blocknum, blk->data);
                    blk->state = BLOCK_READY;
                } else if (blk->state == BLOCK_WRITING) {
                    disk_write(bc->disk, blk->blocknum, blk->data);
                    blk->state = BLOCK_READY;
                }
                pthread_mutex_unlock(&bc->disk_lock);

                pthread_cond_broadcast(&blk->cond);
                pthread_mutex_unlock(&blk->lock);

                pthread_mutex_lock(&bc->cache_lock); // Re-lock cache
                work_done = true;
            } else {
                pthread_mutex_unlock(&blk->lock);
            }
        }
        pthread_mutex_unlock(&bc->cache_lock);

        // Sleep only if no work was done to avoid tight looping
        if (!work_done) {
            // Wait on a condition variable instead of using usleep
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