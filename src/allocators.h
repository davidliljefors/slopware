#pragma once

#include "types.h"

class Allocator
{
public:
	virtual void* alloc(u64 size, u64 alignment = 16) = 0;
	virtual void free(void* ptr) = 0;
	virtual ~Allocator() { }
};

class TempAllocator : public Allocator
{
public:
	void free(void*) override { }
	virtual void free_all() = 0;
	virtual void reset() = 0;
};

class BumpAllocator : public TempAllocator
{
public:
	struct Block
	{
		u8* data;
		u64 capacity;
		u64 used;
	};

	BumpAllocator(u64 chunk_size = 8 * 1024 * 1024);
	~BumpAllocator() override;

	BumpAllocator(const BumpAllocator&) = delete;
	BumpAllocator& operator=(const BumpAllocator&) = delete;

	void* alloc(u64 size, u64 alignment = 16) override;
	void free_all() override;
	void reset() override;
	u64 get_bytes_allocated() const;

private:
	Block* blocks;
	i32 block_count;
	i32 block_capacity;
	u64 chunk_size;

	friend void grow_block_array(BumpAllocator* a);
};

class HeapAllocator : public Allocator
{
public:
	void* alloc(u64 size, u64 alignment = 16) override;
	void free(void* ptr) override;
};

HeapAllocator* application_heap();
