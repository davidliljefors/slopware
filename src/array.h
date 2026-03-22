#pragma once

#include <string.h>

#include "allocators.h"
#include "types.h"

template <typename T>
struct Array
{
	T* data;
	u64 count;
	u64 capacity;
	Allocator* allocator;

	Array()
	: data(nullptr)
	, count(0ull)
	, capacity(0ull)
	, allocator(application_heap())
	{
		
	}

	void init(Allocator* a)
	{
		data = nullptr;
		count = 0;
		capacity = 0;
		allocator = a;
	}

	void destroy()
	{
		if (data) {
			allocator->free(data);
			data = nullptr;
		}
		count = 0;
		capacity = 0;
	}

	void clear()
	{
		count = 0;
	}

	void reserve(u64 new_capacity)
	{
		if (new_capacity <= capacity)
			return;
		T* new_data = (T*)allocator->alloc(new_capacity * sizeof(T), alignof(T));
		if (data) {
			memcpy(new_data, data, count * sizeof(T));
			allocator->free(data);
		}
		data = new_data;
		capacity = new_capacity;
	}

	void grow()
	{
		u64 new_cap = capacity < 8 ? 8 : capacity * 2;
		reserve(new_cap);
	}

	void push(const T& item)
	{
		if (count == capacity)
			grow();
		memcpy(&data[count], &item, sizeof(T));
		count++;
	}

	void insert(u64 index, const T& item)
	{
		if (count == capacity)
			grow();
		if (index < count)
			memmove(&data[index + 1], &data[index], (count - index) * sizeof(T));
		memcpy(&data[index], &item, sizeof(T));
		count++;
	}

	void remove_ordered(u64 index)
	{
		if (index < count - 1)
			memmove(&data[index], &data[index + 1], (count - index - 1) * sizeof(T));
		count--;
	}

	void remove_swap(u64 index)
	{
		data[index] = data[count - 1];
		count--;
	}

	void resize(u64 new_count)
	{
		if (new_count > capacity)
			reserve(new_count);
		count = new_count;
	}

	void resize_zeroed(u64 new_count)
	{
		u64 old_count = count;
		resize(new_count);
		if (new_count > old_count)
			memset(&data[old_count], 0, (new_count - old_count) * sizeof(T));
	}

	T& operator[](u64 index) { return data[index]; }
	const T& operator[](u64 index) const { return data[index]; }

	T* begin() { return data; }
	T* end() { return data + count; }
	const T* begin() const { return data; }
	const T* end() const { return data + count; }

	bool empty() const { return count == 0; }
};
