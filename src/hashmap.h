#pragma once

#include <assert.h>
#include <intrin.h>
#include <string.h>

#include "os.h"
#include "types.h"

template <typename T>
struct HashEntry
{
	u64 key;
	u64 next;
	T value;
};

template <typename T>
struct HashMap
{
private:
	constexpr static f32 MAX_LOAD_FACTOR = 0.7f;
	constexpr static u64 END_OF_CHAIN = u64(-1);

	struct HashFind
	{
		u64 hashIndex;
		u64 dataPrev;
		u64 dataIndex;
	};

public:
	HashMap()
		: m_hash(nullptr)
		, m_data(nullptr)
		, m_size(0)
		, m_capacity(0)
	{
	}

	HashMap(const HashMap&) = delete;
	HashMap& operator=(const HashMap&) = delete;

	HashMap(HashMap&& other)
		: m_hash(other.m_hash)
		, m_data(other.m_data)
		, m_size(other.m_size)
		, m_capacity(other.m_capacity)
	{
		other.m_hash = nullptr;
		other.m_data = nullptr;
		other.m_size = 0;
		other.m_capacity = 0;
	}

	HashMap& operator=(HashMap&& other)
	{
		if (this != &other) {
			reset();

			m_hash = other.m_hash;
			m_data = other.m_data;
			m_size = other.m_size;
			m_capacity = other.m_capacity;

			other.m_hash = nullptr;
			other.m_data = nullptr;
			other.m_size = 0;
			other.m_capacity = 0;
		}
		return *this;
	}

	~HashMap() { reset(); }

	using Entry = HashEntry<T>;

	T* find(u64 key);

	const T* find(u64 key) const;

	bool contains(u64 key) const;

	void add(u64 key, T value);

	void insert_or_assign(u64 key, const T& value);

	bool erase(u64 key);

	T& operator[](u64 key);

	u64 size() const;

	u64 capacity() const;

	Entry* begin();

	Entry* end();

	const Entry* begin() const;

	const Entry* end() const;

	HashMap clone() const;

	void clear();

	void reset();

	// ensures capacity for 'count' elements without rehashing
	void reserve(u64 count);

	bool empty() const;

private:
	static u64 allocate_buffers(u64 element_count, u64** out_hash, Entry** out_data);
	static void free_buffers(u64* base);

	HashFind find_impl(u64 key);

	void erase_impl(HashFind find);

	u64 add_entry(u64 key);

	u64 find_or_make(u64 key);

	bool is_full();

	void grow();

	void rehash(u64 new_size);

	u64* m_hash;
	Entry* m_data;
	u64 m_size;
	u64 m_capacity;
};

template <typename T>
T* HashMap<T>::find(u64 key)
{
	HashFind find = find_impl(key);
	if (find.dataIndex == END_OF_CHAIN) {
		return nullptr;
	}

	return &m_data[find.dataIndex].value;
}

template <typename T>
const T* HashMap<T>::find(u64 key) const
{
	return const_cast<HashMap*>(this)->find(key);
}

template <typename T>
bool HashMap<T>::contains(u64 key) const
{
	HashFind find = const_cast<HashMap*>(this)->find_impl(key);
	return find.dataIndex != END_OF_CHAIN;
}

template <typename T>
void HashMap<T>::add(u64 key, T value)
{
	if (m_capacity == 0)
		grow();

	const HashFind fr = find_impl(key);

	if (fr.dataIndex != END_OF_CHAIN) {
		assert(false && "Cannot add same key twice");
		return;
	}

	u64 i = add_entry(key);
	if (fr.dataPrev == END_OF_CHAIN) {
		m_hash[fr.hashIndex] = i;
	} else {
		m_data[fr.dataPrev].next = i;
	}

	m_data[i].value = value;

	if (is_full()) {
		grow();
	}
}

template <typename T>
void HashMap<T>::insert_or_assign(u64 key, const T& value)
{
	if (m_capacity == 0)
		grow();

	const u64 i = find_or_make(key);

	m_data[i].value = value;

	if (is_full()) {
		grow();
	}
}

template <typename T>
bool HashMap<T>::erase(u64 key)
{
	const HashFind find = find_impl(key);
	if (find.dataIndex != END_OF_CHAIN) {
		erase_impl(find);
		return true;
	}
	return false;
}

template <typename T>
T& HashMap<T>::operator[](u64 key)
{
	if (m_capacity == 0)
		grow();

	u64 i = find_or_make(key);

	if (is_full()) {
		grow();
		i = find_or_make(key);
	}

	return m_data[i].value;
}

template <typename T>
u64 HashMap<T>::size() const
{
	return m_size;
}

template <typename T>
u64 HashMap<T>::capacity() const
{
	return m_capacity;
}

template <typename T>
typename HashMap<T>::Entry* HashMap<T>::begin()
{
	return m_data;
}

template <typename T>
typename HashMap<T>::Entry* HashMap<T>::end()
{
	return m_data + m_size;
}

template <typename T>
const typename HashMap<T>::Entry* HashMap<T>::begin() const
{
	return m_data;
}

template <typename T>
const typename HashMap<T>::Entry* HashMap<T>::end() const
{
	return m_data + m_size;
}

template <typename T>
HashMap<T> HashMap<T>::clone() const
{
	HashMap clone;

	if (m_size != 0) {
		clone.m_size = m_size;
		clone.m_capacity = m_capacity;
		const u64 total_size = allocate_buffers(m_capacity, &clone.m_hash, &clone.m_data);
		memcpy(clone.m_hash, m_hash, total_size);
	}

	return clone;
}

template <typename T>
void HashMap<T>::clear()
{
	m_size = 0;
	memset(m_hash, 0xff, m_capacity * sizeof(u64));
}

template <typename T>
void HashMap<T>::reset()
{
	if (m_capacity != 0) {
		free_buffers(m_hash);
	}

	m_data = nullptr;
	m_hash = nullptr;

	m_capacity = 0;
	m_size = 0;
}

template <typename T>
void HashMap<T>::reserve(u64 count)
{
	constexpr static f64 one_over_max_load_factor = 1.0 / (f64)MAX_LOAD_FACTOR;
	u64 needed = (u64)((f64)count * one_over_max_load_factor + 1.0);

	if (needed > m_capacity) {
		if (__popcnt64(needed) != 1) {
			needed = 1ull << (64 - __lzcnt64(needed));
		}
		rehash(needed);
	}
}

template <typename T>
bool HashMap<T>::empty() const
{
	return m_size == 0;
}

template <typename T>
u64 HashMap<T>::allocate_buffers(u64 element_count, u64** out_hash, Entry** out_data)
{
	constexpr u64 entry_alignment = alignof(Entry);

	const u64 hash_size = sizeof(u64) * element_count;
	const u64 data_size = sizeof(Entry) * element_count;

	const u64 hash_size_aligned = (hash_size + entry_alignment - 1) & ~(entry_alignment - 1);
	const u64 total_size = hash_size_aligned + data_size;

	void* buffer = virtual_alloc(total_size);

	*out_hash = (u64*)buffer;
	*out_data = (Entry*)((u8*)buffer + hash_size_aligned);

	return total_size;
}

template <typename T>
void HashMap<T>::free_buffers(u64* base)
{
	virtual_free(base);
}

template <typename T>
typename HashMap<T>::HashFind HashMap<T>::find_impl(u64 key)
{
	HashFind find;
	find.hashIndex = END_OF_CHAIN;
	find.dataPrev = END_OF_CHAIN;
	find.dataIndex = END_OF_CHAIN;

	if (m_capacity == 0)
		return find;

	find.hashIndex = key & (m_capacity - 1);
	find.dataIndex = m_hash[find.hashIndex];
	while (find.dataIndex != END_OF_CHAIN) {
		if (m_data[find.dataIndex].key == key) {
			return find;
		}
		find.dataPrev = find.dataIndex;
		find.dataIndex = m_data[find.dataIndex].next;
	}

	return find;
}

template <typename T>
void HashMap<T>::erase_impl(HashFind find)
{
	if (find.dataPrev == END_OF_CHAIN)
		m_hash[find.hashIndex] = m_data[find.dataIndex].next;
	else
		m_data[find.dataPrev].next = m_data[find.dataIndex].next;

	if (find.dataIndex == m_size - 1) {
		m_size--;
		return;
	}

	m_data[find.dataIndex] = m_data[m_size - 1];
	HashFind last = find_impl(m_data[find.dataIndex].key);

	if (last.dataPrev != END_OF_CHAIN) {
		m_data[last.dataPrev].next = find.dataIndex;
	} else {
		m_hash[last.hashIndex] = find.dataIndex;
	}

	m_size--;
}

template <typename T>
u64 HashMap<T>::add_entry(u64 key)
{
	u64 ei = m_size;
	++m_size;

	HashMap<T>::Entry* e = &m_data[ei];
	e->key = key;
	e->next = END_OF_CHAIN;

	return ei;
}

template <typename T>
u64 HashMap<T>::find_or_make(u64 key)
{
	const HashFind fr = find_impl(key);
	if (fr.dataIndex != END_OF_CHAIN)
		return fr.dataIndex;

	u64 i = add_entry(key);
	if (fr.dataPrev == END_OF_CHAIN) {
		m_hash[fr.hashIndex] = i;
	} else {
		m_data[fr.dataPrev].next = i;
	}

	return i;
}

template <typename T>
bool HashMap<T>::is_full()
{
	return (f64)m_size >= (f64)m_capacity * (f64)MAX_LOAD_FACTOR;
}

template <typename T>
void HashMap<T>::grow()
{
	if (m_capacity == 0) {
		rehash(4);
	} else {
		const u64 new_size = m_capacity * 2;
		rehash(new_size);
	}
}

template <typename T>
void HashMap<T>::rehash(u64 new_size)
{
	HashMap<T> nh;

	nh.m_capacity = new_size;
	allocate_buffers(new_size, &nh.m_hash, &nh.m_data);
	memset(nh.m_hash, 0xff, new_size * sizeof(u64));

	for (u64 i = 0; i < m_size; ++i) {
		auto& e = m_data[i];
		const u64 slot = nh.find_or_make(e.key);
		nh.m_data[slot].value = e.value;
	}

	u64* old_hash = m_hash;
	u64 old_cap = m_capacity;

	m_capacity = nh.m_capacity;
	m_size = nh.m_size;
	m_hash = nh.m_hash;
	m_data = nh.m_data;

	nh.m_capacity = old_cap;
	nh.m_hash = old_hash;
	// nh destructor calls reset() which frees old buffers
}
