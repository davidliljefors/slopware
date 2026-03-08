#include "allocators.h"

#include <stdlib.h>
#include <string.h>

#include "os.h"

BumpAllocator::BumpAllocator(u64 chunk)
	: blocks(nullptr)
	, block_count(0)
	, block_capacity(0)
	, chunk_size(chunk)
{
}

BumpAllocator::~BumpAllocator()
{
	free_all();
}

static void grow_block_array(BumpAllocator* a)
{
	i32 new_cap = a->block_capacity ? a->block_capacity * 2 : 16;
	u64 new_bytes = new_cap * sizeof(BumpAllocator::Block);
	auto* new_blocks = (BumpAllocator::Block*)virtual_alloc(new_bytes);
	if (a->blocks) {
		memcpy(new_blocks, a->blocks, a->block_count * sizeof(BumpAllocator::Block));
		virtual_free(a->blocks);
	}
	a->blocks = new_blocks;
	a->block_capacity = new_cap;
}

void* BumpAllocator::alloc(u64 size, u64 alignment)
{
	if (size == 0)
		return nullptr;

	u64 mask = alignment - 1;
	size = (size + mask) & ~mask;

	if (block_count > 0) {
		Block& b = blocks[block_count - 1];
		if (b.used + size <= b.capacity) {
			void* ptr = b.data + b.used;
			b.used += size;
			return ptr;
		}
	}

	u64 cap = (size > chunk_size) ? size : chunk_size;
	u8* mem = (u8*)virtual_alloc(cap);
	if (!mem)
		return nullptr;

	if (block_count == block_capacity)
		grow_block_array(this);

	blocks[block_count++] = { mem, cap, size };
	return mem;
}

void BumpAllocator::free_all()
{
	for (i32 i = 0; i < block_count; i++)
		virtual_free(blocks[i].data);
	if (blocks)
		virtual_free(blocks);
	blocks = nullptr;
	block_count = 0;
	block_capacity = 0;
}

void BumpAllocator::reset()
{
	for (i32 i = 0; i < block_count; i++)
		blocks[i].used = 0;
}

u64 BumpAllocator::get_bytes_allocated() const
{
	return block_count * chunk_size;
}

// HeapAllocator

void* HeapAllocator::alloc(u64 size, u64 alignment)
{
	if (size == 0)
		return nullptr;
	return _aligned_malloc((size_t)size, (size_t)alignment);
}

void HeapAllocator::free(void* ptr)
{
	_aligned_free(ptr);
}

static HeapAllocator g_application_heap;

HeapAllocator* application_heap()
{
	return &g_application_heap;
}
