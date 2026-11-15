// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include "osmem.h"
#include "block_meta.h"

#define MMAP_THRESHOLD (128 * 1024)
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~((size_t)ALIGNMENT - 1))
#define META_SIZE ALIGN(sizeof(struct block_meta))
#define MIN_PAYLOAD ALIGNMENT

/* Doubly linked list of all blocks that live on the heap (brk) */
static struct block_meta *heap_head;
static struct block_meta *heap_tail;
static int heap_initialized;

static struct block_meta *get_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

static void *get_payload(struct block_meta *block)
{
	return (char *)block + META_SIZE;
}

/* Append a block at the end of the heap list */
static void list_append(struct block_meta *block)
{
	block->prev = heap_tail;
	block->next = NULL;
	if (heap_tail)
		heap_tail->next = block;
	else
		heap_head = block;
	heap_tail = block;
}

/* Allocate a block directly from the kernel with mmap() */
static void *alloc_mapped(size_t size)
{
	struct block_meta *block;
	void *addr;

	addr = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(addr == MAP_FAILED, "mmap");

	block = addr;
	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;

	return get_payload(block);
}

/* Merge a free block with the free blocks that follow it */
static void coalesce_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;
	if (next->next)
		next->next->prev = block;
	else
		heap_tail = block;
}

/* Merge every group of adjacent free blocks from the heap */
static void coalesce_all(void)
{
	struct block_meta *block = heap_head;

	while (block) {
		if (block->status == STATUS_FREE && block->next &&
		    block->next->status == STATUS_FREE)
			coalesce_next(block);
		else
			block = block->next;
	}
}

/* Split a block if the remainder can hold another block */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + MIN_PAYLOAD)
		return;

	rest = (struct block_meta *)((char *)get_payload(block) + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;
	if (block->next)
		block->next->prev = rest;
	else
		heap_tail = rest;
	block->next = rest;
	block->size = size;
}

/* Best fit: the smallest free block that is big enough */
static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;
	struct block_meta *block;

	for (block = heap_head; block; block = block->next) {
		if (block->status != STATUS_FREE || block->size < size)
			continue;
		if (!best || block->size < best->size)
			best = block;
	}

	return best;
}

/* Grow the program break by `increment` bytes and return the old break */
static void *grow_heap(size_t increment)
{
	void *addr = sbrk(increment);

	DIE(addr == (void *)-1, "sbrk");
	return addr;
}

/* Preallocate one big free block the first time the heap is used */
static void preallocate_heap(void)
{
	struct block_meta *block = grow_heap(MMAP_THRESHOLD);

	block->size = MMAP_THRESHOLD - META_SIZE;
	block->status = STATUS_FREE;
	list_append(block);
	heap_initialized = 1;
}

/* Allocate `size` (already aligned) bytes on the heap */
static void *alloc_heap(size_t size)
{
	struct block_meta *block;

	if (!heap_initialized)
		preallocate_heap();

	coalesce_all();
	block = find_best(size);

	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return get_payload(block);
	}

	/* No fitting block: expand the last one if it is free */
	if (heap_tail && heap_tail->status == STATUS_FREE) {
		grow_heap(size - heap_tail->size);
		heap_tail->size = size;
		heap_tail->status = STATUS_ALLOC;
		return get_payload(heap_tail);
	}

	block = grow_heap(META_SIZE + size);
	block->size = size;
	block->status = STATUS_ALLOC;
	list_append(block);

	return get_payload(block);
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);

	if (META_SIZE + size >= MMAP_THRESHOLD)
		return alloc_mapped(size);

	return alloc_heap(size);
}

void os_free(void *ptr)
{
	struct block_meta *block;

	if (!ptr)
		return;

	block = get_block(ptr);

	if (block->status == STATUS_MAPPED) {
		DIE(munmap(block, META_SIZE + block->size) == -1, "munmap");
		return;
	}

	block->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;

	if (__builtin_mul_overflow(nmemb, size, &total))
		return NULL;

	total = ALIGN(total);

	if (META_SIZE + total >= (size_t)getpagesize()) {
		/* mmap() already returns zeroed pages */
		return alloc_mapped(total);
	}

	ptr = alloc_heap(total);
	memset(ptr, 0, nmemb * size);

	return ptr;
}

/* Allocate a new block, copy the data and release the old one */
static void *move_block(struct block_meta *block, size_t size)
{
	void *new_ptr = os_malloc(size);
	size_t copy = block->size < size ? block->size : size;

	memcpy(new_ptr, get_payload(block), copy);
	os_free(get_payload(block));

	return new_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;

	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	block = get_block(ptr);

	if (block->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);

	if (block->status == STATUS_MAPPED)
		return move_block(block, size);

	/* Shrinking (or same size): truncate the block in place */
	if (size <= block->size) {
		split_block(block, size);
		return ptr;
	}

	/* Try to expand over the free blocks that follow, one at a time */
	while (block->size < size && block->next &&
	       block->next->status == STATUS_FREE)
		coalesce_next(block);

	if (block->size >= size) {
		split_block(block, size);
		return ptr;
	}

	/* Last block of the heap: grow the heap in place */
	if (block == heap_tail && META_SIZE + size < MMAP_THRESHOLD) {
		grow_heap(size - block->size);
		block->size = size;
		return ptr;
	}

	return move_block(block, size);
}
