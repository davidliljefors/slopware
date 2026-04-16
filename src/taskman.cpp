#include "taskman.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocators.h"
#include "app.h"
#include "app_util.h"
#include "host.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_util.h"
#include "os.h"
#include "os_window.h"
#include "theme.h"
#include "types.h"
#include "utf.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>


// NtQuerySystemInformation types


#define STATUS_INFO_LENGTH_MISMATCH ((i32)0xC0000004)

typedef struct _MY_UNICODE_STRING
{
	u16 Length;
	u16 MaximumLength;
	wchar_t* Buffer;
} MY_UNICODE_STRING;

typedef struct _MY_SYSTEM_PROCESS_INFORMATION
{
	u32 NextEntryOffset;
	u32 NumberOfThreads;
	u8 Reserved1[48];
	MY_UNICODE_STRING ImageName;
	i32 BasePriority;
	void* UniqueProcessId;
	void* InheritedFromUniqueProcessId;
	u32 HandleCount;
	u32 SessionId;
	void* UniquePageDirectoryBase;
	u64 PeakVirtualSize;
	u64 VirtualSize;
	u32 PageFaultCount;
	u64 PeakWorkingSetSize;
	u64 WorkingSetSize;
	u64 QuotaPeakPagedPoolUsage;
	u64 QuotaPagedPoolUsage;
	u64 QuotaPeakNonPagedPoolUsage;
	u64 QuotaNonPagedPoolUsage;
	u64 PagefileUsage;
	u64 PeakPagefileUsage;
	u64 PrivatePageCount;
	i64 ReadOperationCount;
	i64 WriteOperationCount;
	i64 OtherOperationCount;
	i64 ReadTransferCount;
	i64 WriteTransferCount;
	i64 OtherTransferCount;
} MY_SYSTEM_PROCESS_INFORMATION;

typedef i32(WINAPI* NtQuerySystemInformationFn)(
	i32 SystemInformationClass,
	void* SystemInformation,
	u32 SystemInformationLength,
	u32* ReturnLength);

static NtQuerySystemInformationFn g_nt_query_system_info = nullptr;
static constexpr i32 SystemProcessInformation = 5;


// Constants


static constexpr i32 TITLE_BAR_HEIGHT = 32;
static constexpr i32 TITLE_BAR_BUTTONS = 2;

static constexpr i32 MAX_PROCESSES = 4096;
static constexpr u32 NQSI_INITIAL_SIZE = 512 * 1024;
static constexpr u32 NQSI_MAX_SIZE = 64 * 1024 * 1024;

static constexpr i32 GRAPH_HISTORY_SIZE = 300;
static constexpr i32 MAX_CPUS = 256;

static constexpr f64 REFRESH_INTERVALS[] = {0.5, 1.0, 2.0, 5.0};
static constexpr i32 REFRESH_INTERVAL_COUNT = 4;
static const char* REFRESH_LABELS[] = {"0.5s", "1s", "2s", "5s"};


// Data structures


struct ProcessInfo
{
	u32 pid;
	u32 parent_pid;
	char name[260];
	char name_lower[260];
	u32 thread_count;
	u32 handle_count;
	u64 working_set;
	u64 private_bytes;
	u64 virtual_size;
	f32 cpu_percent;
	u64 kernel_time;
	u64 user_time;
	i32 priority;
	u32 session_id;
};

struct ProcessSnapshot
{
	ProcessInfo* processes;
	i32 count;
	i32 capacity;
	f64 wall_time;
	u64 system_kernel_time;
	u64 system_user_time;
	u64 system_idle_time;
};

struct SystemStats
{
	f32 cpu_percent;
	f32 per_cpu_percent[MAX_CPUS];
	i32 cpu_count;
	u64 total_physical;
	u64 used_physical;
	u64 available_physical;
};

struct CpuTimes
{
	u64 idle;
	u64 kernel;
	u64 user;
};

// Static system info collected once at init
struct SystemInfo
{
	char cpu_name[128];
	u32 base_speed_mhz;
	i32 physical_cores;
	i32 logical_cores;
	u64 total_physical_ram;
	u32 ram_speed_mhz;
	i32 ram_slots_total;
	i32 ram_slots_used;
};

struct GraphHistory
{
	f32 samples[GRAPH_HISTORY_SIZE];
	i32 write_index;
	i32 count;
};

enum Column : u32
{
	COL_PID = 0,
	COL_NAME,
	COL_CPU,
	COL_MEMORY,
	COL_THREADS,
	COL_HANDLES,
	COL_PRIORITY,
	COL_SESSION_ID,
	COL_PARENT_PID,
	COL_PRIVATE_BYTES,
	COL_VIRTUAL_SIZE,
	COL_COUNT,
};

struct ColumnDef
{
	const char* label;
	f32 default_width;
	ImGuiTableColumnFlags flags;
};

static const ColumnDef g_column_defs[COL_COUNT] = {
	{"PID", 70.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort},
	{"Name", 0.35f, ImGuiTableColumnFlags_WidthStretch},
	{"CPU %", 65.0f, ImGuiTableColumnFlags_WidthFixed},
	{"Memory", 90.0f, ImGuiTableColumnFlags_WidthFixed},
	{"Threads", 65.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Handles", 70.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Priority", 65.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Session", 60.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Parent PID", 80.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Private Bytes", 100.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
	{"Virtual Size", 100.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide},
};


// Global state


// NtQuerySystemInformation buffer (reused across collections)
static u8* g_nqsi_buffer = nullptr;
static u32 g_nqsi_buffer_size = NQSI_INITIAL_SIZE;

// Snapshots
static ProcessSnapshot g_staging_snapshot;
static ProcessSnapshot g_display_snapshot;
static ProcessSnapshot g_prev_snapshot;

// System stats (written by bg thread alongside staging snapshot)
static SystemStats g_staging_stats;
static SystemStats g_display_stats;

// Graphs
static GraphHistory g_cpu_history;
static GraphHistory g_mem_history;
static GraphHistory* g_per_cpu_histories = nullptr; // allocated in init
static i32 g_cpu_count = 0;

// Per-CPU time tracking (background thread)
static CpuTimes* g_prev_cpu_times = nullptr;
static CpuTimes* g_cur_cpu_times = nullptr;

// System info (collected once)
static SystemInfo g_sys_info;

// Synchronization
static Lock g_snapshot_lock;
static std::atomic<bool> g_staging_ready{false};
static std::atomic<bool> g_shutdown{false};
static Lock g_collector_lock;
static CondVar g_collector_cv;

// Background thread
static Thread* g_collector_thread = nullptr;

// UI state
static char g_search_buf[512] = {};
static char g_prev_search_buf[512] = {};
static TokenQuery g_search_query;
static i32* g_sorted_indices = nullptr;
static i32 g_filtered_count = 0;
static bool g_sort_dirty = true;
static bool g_has_first_snapshot = false;

// Selection
static ImGuiSelectionBasicStorage g_selection;

static ImGuiID selection_adapter(ImGuiSelectionBasicStorage*, int idx)
{
	if (idx >= 0 && idx < g_filtered_count)
		return (ImGuiID)g_display_snapshot.processes[g_sorted_indices[idx]].pid;
	return 0;
}

// Settings
static i32 g_refresh_interval_idx = 1; // default 1.0s
static bool g_show_per_cpu = false;
static bool g_settings_loaded = false;

// Sort state (used by qsort comparator)
static ImGuiTableSortSpecs* g_current_sort_specs = nullptr;
static ProcessInfo* g_sort_processes = nullptr;


// Helpers


static void str_to_lower(char* dst, const char* src, i32 max_len)
{
	i32 i = 0;
	for (; src[i] && i < max_len - 1; i++)
		dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? (src[i] + 32) : src[i];
	dst[i] = 0;
}

static void format_bytes(char* buf, i32 buf_size, u64 bytes)
{
	if (bytes >= 1024ull * 1024 * 1024)
		snprintf(buf, buf_size, "%.1f GB", (f64)bytes / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024ull * 1024)
		snprintf(buf, buf_size, "%.1f MB", (f64)bytes / (1024.0 * 1024.0));
	else
		snprintf(buf, buf_size, "%llu KB", bytes / 1024);
}

static void graph_history_push(GraphHistory* h, f32 value)
{
	h->samples[h->write_index] = value;
	h->write_index = (h->write_index + 1) % GRAPH_HISTORY_SIZE;
	if (h->count < GRAPH_HISTORY_SIZE)
		h->count++;
}

static f32 graph_history_get(const GraphHistory* h, i32 age)
{
	// age=0 is most recent, age=count-1 is oldest
	i32 idx = (h->write_index - 1 - age + GRAPH_HISTORY_SIZE * 2) % GRAPH_HISTORY_SIZE;
	return h->samples[idx];
}

static void snapshot_init(ProcessSnapshot* s, i32 capacity)
{
	s->processes = (ProcessInfo*)virtual_alloc(capacity * sizeof(ProcessInfo));
	s->count = 0;
	s->capacity = capacity;
	s->wall_time = 0;
	s->system_kernel_time = 0;
	s->system_user_time = 0;
	s->system_idle_time = 0;
}

static void snapshot_destroy(ProcessSnapshot* s)
{
	if (s->processes) {
		virtual_free(s->processes);
		s->processes = nullptr;
	}
	s->count = 0;
	s->capacity = 0;
}


// Data collection


static void collect_system_info(SystemInfo* info)
{
	memset(info, 0, sizeof(*info));

	// CPU name and speed from registry
	HKEY hkey;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
			0, KEY_READ, &hkey) == ERROR_SUCCESS) {
		wchar_t cpu_name_w[128] = {};
		DWORD size = sizeof(cpu_name_w);
		if (RegQueryValueExW(hkey, L"ProcessorNameString", nullptr, nullptr, (BYTE*)cpu_name_w, &size) == ERROR_SUCCESS)
			utf8_from_wide(info->cpu_name, sizeof(info->cpu_name), cpu_name_w);
		DWORD mhz = 0;
		size = sizeof(mhz);
		if (RegQueryValueExW(hkey, L"~MHz", nullptr, nullptr, (BYTE*)&mhz, &size) == ERROR_SUCCESS)
			info->base_speed_mhz = mhz;
		RegCloseKey(hkey);
	}

	// Core counts
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	info->logical_cores = si.dwNumberOfProcessors;

	// Physical cores via GetLogicalProcessorInformation
	DWORD buf_size = 0;
	GetLogicalProcessorInformation(nullptr, &buf_size);
	if (buf_size > 0) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf =
			(SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)virtual_alloc(buf_size);
		if (GetLogicalProcessorInformation(buf, &buf_size)) {
			i32 count = buf_size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
			i32 cores = 0;
			for (i32 i = 0; i < count; i++) {
				if (buf[i].Relationship == RelationProcessorCore)
					cores++;
			}
			info->physical_cores = cores;
		}
		virtual_free(buf);
	}

	// RAM total
	MEMORYSTATUSEX memstat = {};
	memstat.dwLength = sizeof(memstat);
	if (GlobalMemoryStatusEx(&memstat))
		info->total_physical_ram = memstat.ullTotalPhys;

	// RAM speed and slots from SMBIOS (Type 17 = Memory Device)
	DWORD smbios_size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
	if (smbios_size > 0) {
		u8* smbios = (u8*)virtual_alloc(smbios_size);
		if (GetSystemFirmwareTable('RSMB', 0, smbios, smbios_size) == smbios_size) {
			// SMBIOS raw data starts with 8-byte header
			u8* data = smbios + 8;
			u8* end = smbios + smbios_size;
			while (data < end) {
				u8 type = data[0];
				u8 length = data[1];
				if (length < 4 || data + length > end) break;

				if (type == 17 && length >= 28) {
					// Type 17: Memory Device
					u16 total_width = *(u16*)(data + 8);
					u16 speed = *(u16*)(data + 21); // Speed in MT/s
					u16 size16 = *(u16*)(data + 12);
					info->ram_slots_total++;
					if (size16 != 0 && size16 != 0xFFFF) {
						info->ram_slots_used++;
						if (speed > 0 && info->ram_speed_mhz == 0)
							info->ram_speed_mhz = speed;
					}
					(void)total_width;
				}

				// Skip to end of structure: past formatted area + unformatted strings
				u8* p = data + length;
				while (p + 1 < end && !(p[0] == 0 && p[1] == 0))
					p++;
				data = p + 2;
			}
		}
		virtual_free(smbios);
	}
}

static bool collect_process_data(ProcessSnapshot* snap)
{
	// Query process information
	u32 returned_size = 0;
	i32 status = g_nt_query_system_info(SystemProcessInformation, g_nqsi_buffer, g_nqsi_buffer_size, &returned_size);
	while (status == STATUS_INFO_LENGTH_MISMATCH) {
		virtual_free(g_nqsi_buffer);
		g_nqsi_buffer_size = returned_size + 64 * 1024; // add margin
		if (g_nqsi_buffer_size > NQSI_MAX_SIZE)
			return false;
		g_nqsi_buffer = (u8*)virtual_alloc(g_nqsi_buffer_size);
		status = g_nt_query_system_info(SystemProcessInformation, g_nqsi_buffer, g_nqsi_buffer_size, &returned_size);
	}
	if (status < 0)
		return false;

	// Parse entries
	snap->count = 0;
	snap->wall_time = timer_now();

	u8* ptr = g_nqsi_buffer;
	for (;;) {
		MY_SYSTEM_PROCESS_INFORMATION* spi = (MY_SYSTEM_PROCESS_INFORMATION*)ptr;

		if (snap->count >= snap->capacity)
			break;

		ProcessInfo* p = &snap->processes[snap->count];
		p->pid = (u32)(uintptr_t)spi->UniqueProcessId;
		p->parent_pid = (u32)(uintptr_t)spi->InheritedFromUniqueProcessId;
		p->thread_count = spi->NumberOfThreads;
		p->handle_count = spi->HandleCount;
		p->working_set = spi->WorkingSetSize;
		p->private_bytes = spi->PrivatePageCount;
		p->virtual_size = spi->VirtualSize;
		p->priority = spi->BasePriority;
		p->session_id = spi->SessionId;

		// Reserved1[48] actual layout (x64):
		//   [0..7]   WorkingSetPrivateSize
		//   [8..11]  HardFaultCount
		//   [12..15] NumberOfThreadsHighWatermark
		//   [16..23] CycleTime
		//   [24..31] CreateTime
		//   [32..39] UserTime
		//   [40..47] KernelTime
		u64* times = (u64*)spi->Reserved1;
		p->user_time = times[4];   // offset 32
		p->kernel_time = times[5]; // offset 40

		p->cpu_percent = 0.0f;

		// Convert process name
		if (spi->ImageName.Buffer && spi->ImageName.Length > 0) {
			utf8_from_wide(p->name, sizeof(p->name), spi->ImageName.Buffer, spi->ImageName.Length / 2);
		} else {
			if (p->pid == 0)
				snprintf(p->name, sizeof(p->name), "[System Idle Process]");
			else
				snprintf(p->name, sizeof(p->name), "[pid %u]", p->pid);
		}
		str_to_lower(p->name_lower, p->name, sizeof(p->name_lower));

		snap->count++;

		if (spi->NextEntryOffset == 0)
			break;
		ptr += spi->NextEntryOffset;
	}

	// System times
	FILETIME idle_ft, kernel_ft, user_ft;
	if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
		snap->system_idle_time = ((u64)idle_ft.dwHighDateTime << 32) | idle_ft.dwLowDateTime;
		snap->system_kernel_time = ((u64)kernel_ft.dwHighDateTime << 32) | kernel_ft.dwLowDateTime;
		snap->system_user_time = ((u64)user_ft.dwHighDateTime << 32) | user_ft.dwLowDateTime;
	}

	return true;
}

static void compute_cpu_deltas(ProcessSnapshot* current, const ProcessSnapshot* prev)
{
	if (prev->count == 0 || prev->wall_time == 0)
		return;

	f64 wall_delta_seconds = current->wall_time - prev->wall_time;
	if (wall_delta_seconds <= 0)
		return;

	f64 wall_delta_ticks = wall_delta_seconds * 10000000.0; // seconds -> 100ns ticks
	i32 num_cpus = get_processor_count();

	// Build lookup from prev snapshot: pid -> index
	// Use a simple linear scan since process counts are small (<1000)
	for (i32 i = 0; i < current->count; i++) {
		ProcessInfo* cur = &current->processes[i];
		cur->cpu_percent = 0.0f;

		for (i32 j = 0; j < prev->count; j++) {
			const ProcessInfo* old = &prev->processes[j];
			if (old->pid == cur->pid && strcmp(old->name, cur->name) == 0) {
				u64 cur_total = cur->kernel_time + cur->user_time;
				u64 old_total = old->kernel_time + old->user_time;
				if (cur_total >= old_total) {
					f64 delta = (f64)(cur_total - old_total);
					cur->cpu_percent = (f32)(delta / (wall_delta_ticks * num_cpus) * 100.0);
					if (cur->cpu_percent > 100.0f)
						cur->cpu_percent = 100.0f;
				}
				break;
			}
		}
	}
}

static constexpr i32 SystemProcessorPerformanceInformation = 8;

struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
{
	i64 IdleTime;
	i64 KernelTime;
	i64 UserTime;
	i64 Reserved1[2];
	u32 Reserved2;
};

static SystemStats compute_system_stats(const ProcessSnapshot* current, const ProcessSnapshot* prev)
{
	SystemStats stats = {};

	// Overall CPU
	if (prev->wall_time > 0) {
		u64 dk = current->system_kernel_time - prev->system_kernel_time;
		u64 du = current->system_user_time - prev->system_user_time;
		u64 di = current->system_idle_time - prev->system_idle_time;
		u64 total = dk + du;
		if (total > 0)
			stats.cpu_percent = (f32)(1.0 - (f64)di / (f64)total) * 100.0f;
	}

	// Per-CPU
	stats.cpu_count = g_cpu_count;
	if (g_nt_query_system_info && g_cpu_count > 0) {
		u32 needed = g_cpu_count * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
		SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION infos[MAX_CPUS];
		u32 returned = 0;
		i32 status = g_nt_query_system_info(SystemProcessorPerformanceInformation,
			infos, needed, &returned);
		if (status >= 0) {
			i32 count = returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
			if (count > MAX_CPUS) count = MAX_CPUS;
			for (i32 i = 0; i < count; i++) {
				g_cur_cpu_times[i].idle = infos[i].IdleTime;
				g_cur_cpu_times[i].kernel = infos[i].KernelTime;
				g_cur_cpu_times[i].user = infos[i].UserTime;

				if (g_prev_cpu_times[i].kernel > 0 || g_prev_cpu_times[i].user > 0) {
					u64 dk = g_cur_cpu_times[i].kernel - g_prev_cpu_times[i].kernel;
					u64 du = g_cur_cpu_times[i].user - g_prev_cpu_times[i].user;
					u64 di = g_cur_cpu_times[i].idle - g_prev_cpu_times[i].idle;
					u64 total = dk + du;
					if (total > 0)
						stats.per_cpu_percent[i] = (f32)(1.0 - (f64)di / (f64)total) * 100.0f;
				}
			}
			memcpy(g_prev_cpu_times, g_cur_cpu_times, count * sizeof(CpuTimes));
			stats.cpu_count = count;
		}
	}

	// Memory
	MEMORYSTATUSEX memstat = {};
	memstat.dwLength = sizeof(memstat);
	if (GlobalMemoryStatusEx(&memstat)) {
		stats.total_physical = memstat.ullTotalPhys;
		stats.available_physical = memstat.ullAvailPhys;
		stats.used_physical = stats.total_physical - stats.available_physical;
	}

	return stats;
}


// Background collector thread


static void collector_thread_func(void*)
{
	while (!g_shutdown.load(std::memory_order_acquire)) {
		// Wait until UI has consumed the previous staging data
		if (g_staging_ready.load(std::memory_order_acquire)) {
			lock_write(&g_collector_lock);
			u32 interval_ms = (u32)(REFRESH_INTERVALS[g_refresh_interval_idx] * 1000.0);
			condvar_wait(&g_collector_cv, &g_collector_lock, interval_ms);
			unlock_write(&g_collector_lock);
			continue;
		}

		// Collect data into staging snapshot
		if (collect_process_data(&g_staging_snapshot)) {
			compute_cpu_deltas(&g_staging_snapshot, &g_prev_snapshot);
			g_staging_stats = compute_system_stats(&g_staging_snapshot, &g_prev_snapshot);

			// Swap prev: copy current times into prev for next delta
			// We reuse the prev buffer by copying just what we need
			if (g_prev_snapshot.capacity >= g_staging_snapshot.count) {
				memcpy(g_prev_snapshot.processes, g_staging_snapshot.processes,
					g_staging_snapshot.count * sizeof(ProcessInfo));
				g_prev_snapshot.count = g_staging_snapshot.count;
				g_prev_snapshot.wall_time = g_staging_snapshot.wall_time;
				g_prev_snapshot.system_kernel_time = g_staging_snapshot.system_kernel_time;
				g_prev_snapshot.system_user_time = g_staging_snapshot.system_user_time;
				g_prev_snapshot.system_idle_time = g_staging_snapshot.system_idle_time;
			}

			g_staging_ready.store(true, std::memory_order_release);
		}

		// Sleep for the configured interval, but wake on shutdown
		lock_write(&g_collector_lock);
		u32 interval_ms = (u32)(REFRESH_INTERVALS[g_refresh_interval_idx] * 1000.0);
		condvar_wait(&g_collector_cv, &g_collector_lock, interval_ms);
		unlock_write(&g_collector_lock);
	}
}


// Sorting


static int process_compare(const void* a, const void* b)
{
	i32 ia = *(const i32*)a;
	i32 ib = *(const i32*)b;
	const ProcessInfo& pa = g_sort_processes[ia];
	const ProcessInfo& pb = g_sort_processes[ib];

	for (i32 s = 0; s < g_current_sort_specs->SpecsCount; s++) {
		const ImGuiTableColumnSortSpecs& spec = g_current_sort_specs->Specs[s];
		int delta = 0;

		switch (spec.ColumnIndex) {
		case COL_PID:
			delta = (pa.pid > pb.pid) - (pa.pid < pb.pid);
			break;
		case COL_NAME:
			delta = strcmp(pa.name_lower, pb.name_lower);
			break;
		case COL_CPU:
			delta = (pa.cpu_percent > pb.cpu_percent) - (pa.cpu_percent < pb.cpu_percent);
			break;
		case COL_MEMORY:
			delta = (pa.working_set > pb.working_set) - (pa.working_set < pb.working_set);
			break;
		case COL_THREADS:
			delta = (pa.thread_count > pb.thread_count) - (pa.thread_count < pb.thread_count);
			break;
		case COL_HANDLES:
			delta = (pa.handle_count > pb.handle_count) - (pa.handle_count < pb.handle_count);
			break;
		case COL_PRIORITY:
			delta = (pa.priority > pb.priority) - (pa.priority < pb.priority);
			break;
		case COL_SESSION_ID:
			delta = (pa.session_id > pb.session_id) - (pa.session_id < pb.session_id);
			break;
		case COL_PARENT_PID:
			delta = (pa.parent_pid > pb.parent_pid) - (pa.parent_pid < pb.parent_pid);
			break;
		case COL_PRIVATE_BYTES:
			delta = (pa.private_bytes > pb.private_bytes) - (pa.private_bytes < pb.private_bytes);
			break;
		case COL_VIRTUAL_SIZE:
			delta = (pa.virtual_size > pb.virtual_size) - (pa.virtual_size < pb.virtual_size);
			break;
		}

		if (delta != 0)
			return (spec.SortDirection == ImGuiSortDirection_Ascending) ? delta : -delta;
	}

	// Stable fallback: sort by PID
	return (pa.pid > pb.pid) - (pa.pid < pb.pid);
}

static void rebuild_filtered_indices()
{
	g_filtered_count = 0;
	bool has_filter = g_search_query.count > 0;

	for (i32 i = 0; i < g_display_snapshot.count; i++) {
		if (has_filter && !token_query_match_all(&g_search_query, g_display_snapshot.processes[i].name_lower))
			continue;
		g_sorted_indices[g_filtered_count++] = i;
	}

	g_sort_dirty = true;
}

static void apply_sort()
{
	if (!g_sort_dirty || g_filtered_count == 0)
		return;
	if (g_current_sort_specs && g_current_sort_specs->SpecsCount > 0) {
		g_sort_processes = g_display_snapshot.processes;
		qsort(g_sorted_indices, g_filtered_count, sizeof(i32), process_compare);
	}
	g_sort_dirty = false;
}


// Settings persistence


static void* settings_read_open(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
{
	if (strcmp(name, "Settings") == 0)
		return (void*)1;
	return nullptr;
}

static void settings_read_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
	if (strncmp(line, "RefreshInterval=", 16) == 0) {
		i32 val = atoi(line + 16);
		if (val >= 0 && val < REFRESH_INTERVAL_COUNT)
			g_refresh_interval_idx = val;
	}
	if (strncmp(line, "ShowPerCpu=", 11) == 0)
		g_show_per_cpu = atoi(line + 11) != 0;
	g_settings_loaded = true;
}

static void settings_write_all(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* buf)
{
	buf->appendf("[TaskManager][Settings]\n");
	buf->appendf("RefreshInterval=%d\n", g_refresh_interval_idx);
	buf->appendf("ShowPerCpu=%d\n", g_show_per_cpu ? 1 : 0);
	buf->appendf("\n");
}


// Graph rendering


static void draw_graph(const char* label, const GraphHistory* history, f32 current_value,
	const char* value_fmt, ImVec4 line_color, f32 height)
{
	ImGuiIO& io = ImGui::GetIO();
	f32 dpi_scale = io.FontGlobalScale > 0 ? io.FontGlobalScale : 1.0f;

	// Label + current value (skip if label is null)
	if (label && value_fmt) {
		char value_str[64];
		snprintf(value_str, sizeof(value_str), value_fmt, current_value);
		ImGui::Text("%s: %s", label, value_str);
	}

	ImVec2 graph_pos = ImGui::GetCursorScreenPos();
	ImVec2 graph_size = ImVec2(ImGui::GetContentRegionAvail().x, height);

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Background
	dl->AddRectFilled(graph_pos,
		ImVec2(graph_pos.x + graph_size.x, graph_pos.y + graph_size.y),
		ImGui::GetColorU32(ImGuiCol_FrameBg));

	// Grid lines at 25%, 50%, 75%
	ImU32 grid_color = ImGui::GetColorU32(ImVec4(theme_color_text_disabled().x, theme_color_text_disabled().y, theme_color_text_disabled().z, 0.2f));
	for (i32 pct = 25; pct <= 75; pct += 25) {
		f32 y = graph_pos.y + graph_size.y * (1.0f - (f32)pct / 100.0f);
		dl->AddLine(ImVec2(graph_pos.x, y), ImVec2(graph_pos.x + graph_size.x, y), grid_color);
	}

	// Border
	dl->AddRect(graph_pos,
		ImVec2(graph_pos.x + graph_size.x, graph_pos.y + graph_size.y),
		ImGui::GetColorU32(ImGuiCol_Border));

	// Draw polyline
	i32 n = history->count;
	if (n >= 2) {
		ImVec2 points[GRAPH_HISTORY_SIZE];
		for (i32 i = 0; i < n; i++) {
			f32 val = graph_history_get(history, n - 1 - i) / 100.0f;
			if (val < 0.0f) val = 0.0f;
			if (val > 1.0f) val = 1.0f;
			f32 t = (f32)i / (f32)(n - 1);
			points[i] = ImVec2(
				graph_pos.x + t * graph_size.x,
				graph_pos.y + graph_size.y * (1.0f - val));
		}
		f32 thickness = 1.5f * dpi_scale;
		dl->AddPolyline(points, n, ImGui::GetColorU32(line_color), 0, thickness);
	}

	ImGui::Dummy(graph_size);
}


// UI rendering


static void kill_selected_processes()
{
	void* it = nullptr;
	ImGuiID id;
	while (g_selection.GetNextSelectedItem(&it, &id)) {
		HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)id);
		if (h) {
			TerminateProcess(h, 1);
			CloseHandle(h);
		}
	}
	g_selection.Clear();
}

static void render_details_tab()
{
	f32 dpi_scale = window_get_dpi_scale(host_hwnd());

	// Filter bar inside details tab
	{
		ImGui::SetNextItemWidth(floorf(250.0f * dpi_scale));
		ImGui::InputTextWithHint("##search", "Filter processes...", g_search_buf, sizeof(g_search_buf));

		ImGui::SameLine();
		if (!g_has_first_snapshot) {
			ImGui::TextColored(theme_color_text_indexing(), "Collecting...");
		} else {
			ImGui::Text("%d processes", g_display_snapshot.count);
			if (g_selection.Size > 0) {
				ImGui::SameLine();
				ImGui::Text("(%d selected)", g_selection.Size);
			}
		}
	}

	// Process table
	i32 col_count = COL_COUNT;
	if (!ImGui::BeginTable("ProcessTable", col_count,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
				ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
				ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable,
			ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupScrollFreeze(0, 1);
	for (i32 i = 0; i < COL_COUNT; i++) {
		ImGui::TableSetupColumn(g_column_defs[i].label, g_column_defs[i].flags, g_column_defs[i].default_width);
	}
	ImGui::TableHeadersRow();

	// Handle sorting
	ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
	if (sort_specs) {
		if (sort_specs->SpecsDirty) {
			g_current_sort_specs = sort_specs;
			g_sort_dirty = true;
			sort_specs->SpecsDirty = false;
		}
		g_current_sort_specs = sort_specs;
	}
	apply_sort();

	// Multi-select
	ImGuiMultiSelectFlags ms_flags =
		ImGuiMultiSelectFlags_BoxSelect1d |
		ImGuiMultiSelectFlags_ClearOnEscape |
		ImGuiMultiSelectFlags_ClearOnClickVoid;
	ImGuiMultiSelectIO* ms_io = ImGui::BeginMultiSelect(ms_flags, g_selection.Size, g_filtered_count);
	g_selection.ApplyRequests(ms_io);

	// Render rows with clipper
	ImGuiListClipper clipper;
	clipper.Begin(g_filtered_count);

	// Ensure the range source item is always submitted (required for shift-click)
	if (ms_io->RangeSrcItem != -1)
		clipper.IncludeItemByIndex((int)ms_io->RangeSrcItem);

	while (clipper.Step()) {
		for (i32 row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
			const ProcessInfo& p = g_display_snapshot.processes[g_sorted_indices[row]];
			ImGui::TableNextRow();

			// First column: Selectable spanning all columns
			if (ImGui::TableSetColumnIndex(COL_PID)) {
				char label[32];
				snprintf(label, sizeof(label), "%u", p.pid);

				ImGuiID item_id = (ImGuiID)p.pid;
				bool is_selected = g_selection.Contains(item_id);
				ImGui::SetNextItemSelectionUserData(row);
				ImGui::Selectable(label, is_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);

				if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !is_selected) {
					g_selection.Clear();
					g_selection.SetItemSelected(item_id, true);
				}
				if (ImGui::BeginPopupContextItem()) {
					if (g_selection.Size > 0) {
						char kill_label[64];
						if (g_selection.Size == 1)
							snprintf(kill_label, sizeof(kill_label), "Kill process");
						else
							snprintf(kill_label, sizeof(kill_label), "Kill %d processes", g_selection.Size);
						if (ImGui::MenuItem(kill_label))
							kill_selected_processes();
					}
					ImGui::EndPopup();
				}
			}

			// Name
			if (ImGui::TableSetColumnIndex(COL_NAME)) {
				if (g_search_query.count > 0) {
					render_highlighted_text(p.name, p.name_lower,
						g_search_query.tokens, g_search_query.count,
						theme_color_text(), theme_color_highlight(), theme_highlight_bg_alpha());
				} else {
					ImGui::TextUnformatted(p.name);
				}
			}

			// CPU %
			if (ImGui::TableSetColumnIndex(COL_CPU)) {
				char buf[32];
				if (g_has_first_snapshot && g_prev_snapshot.count > 0)
					snprintf(buf, sizeof(buf), "%.1f", p.cpu_percent);
				else
					snprintf(buf, sizeof(buf), "--");
				ImGui::TextUnformatted(buf);
			}

			// Memory (Working Set)
			if (ImGui::TableSetColumnIndex(COL_MEMORY)) {
				char buf[32];
				format_bytes(buf, sizeof(buf), p.working_set);
				ImGui::TextUnformatted(buf);
			}

			// Threads
			if (ImGui::TableSetColumnIndex(COL_THREADS)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%u", p.thread_count);
				ImGui::TextUnformatted(buf);
			}

			// Handles
			if (ImGui::TableSetColumnIndex(COL_HANDLES)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%u", p.handle_count);
				ImGui::TextUnformatted(buf);
			}

			// Priority
			if (ImGui::TableSetColumnIndex(COL_PRIORITY)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%d", p.priority);
				ImGui::TextUnformatted(buf);
			}

			// Session ID
			if (ImGui::TableSetColumnIndex(COL_SESSION_ID)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%u", p.session_id);
				ImGui::TextUnformatted(buf);
			}

			// Parent PID
			if (ImGui::TableSetColumnIndex(COL_PARENT_PID)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%u", p.parent_pid);
				ImGui::TextUnformatted(buf);
			}

			// Private Bytes
			if (ImGui::TableSetColumnIndex(COL_PRIVATE_BYTES)) {
				char buf[32];
				format_bytes(buf, sizeof(buf), p.private_bytes);
				ImGui::TextUnformatted(buf);
			}

			// Virtual Size
			if (ImGui::TableSetColumnIndex(COL_VIRTUAL_SIZE)) {
				char buf[32];
				format_bytes(buf, sizeof(buf), p.virtual_size);
				ImGui::TextUnformatted(buf);
			}
		}
	}

	// Fill remaining space so box-select can start from empty area
	{
		f32 remaining = ImGui::GetContentRegionAvail().y;
		if (remaining > 0) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Dummy(ImVec2(0, remaining));
		}
	}

	// End multi-select
	ms_io = ImGui::EndMultiSelect();
	g_selection.ApplyRequests(ms_io);

	ImGui::EndTable();

	// Delete key kills selected processes
	if (g_selection.Size > 0 && ImGui::IsKeyPressed(ImGuiKey_Delete))
		kill_selected_processes();
}

static void render_performance_tab()
{
	ImVec4 cpu_color = theme_color_highlight(); // Gruvbox yellow
	ImVec4 mem_color = ImVec4(0.557f, 0.753f, 0.486f, 1.0f); // #8ec07c - Gruvbox aqua

	// CPU section
	{
		// System info line
		char cpu_label[256];
		if (g_sys_info.base_speed_mhz > 0) {
			snprintf(cpu_label, sizeof(cpu_label), "CPU  %.1f%%   %s   %.2f GHz   %d cores / %d logical",
				g_display_stats.cpu_percent, g_sys_info.cpu_name,
				g_sys_info.base_speed_mhz / 1000.0f,
				g_sys_info.physical_cores, g_sys_info.logical_cores);
		} else {
			snprintf(cpu_label, sizeof(cpu_label), "CPU  %.1f%%", g_display_stats.cpu_percent);
		}
		ImGui::TextColored(cpu_color, "%s", cpu_label);

		f32 avail = ImGui::GetContentRegionAvail().y;
		f32 section_height = avail * 0.48f;
		f32 line_h = ImGui::GetTextLineHeightWithSpacing();

		if (g_show_per_cpu && g_cpu_count > 0) {
			// Per-CPU grid: simple filled boxes like Windows Task Manager
			i32 cols = 16;
			if (g_cpu_count <= 4) cols = 4;
			else if (g_cpu_count <= 8) cols = 8;
			else if (g_cpu_count <= 32) cols = 8;
			i32 rows = (g_cpu_count + cols - 1) / cols;

			f32 gap = 2.0f;
			f32 total_w = ImGui::GetContentRegionAvail().x;
			f32 cell_w = (total_w - gap * (cols - 1)) / cols;
			f32 cell_h = (section_height - gap * (rows - 1)) / rows;
			if (cell_h < 8.0f) cell_h = 8.0f;

			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImU32 bg_col = ImGui::GetColorU32(ImGuiCol_FrameBg);
			ImU32 fill_col = ImGui::GetColorU32(cpu_color);

			for (i32 idx = 0; idx < g_cpu_count; idx++) {
				i32 r = idx / cols;
				i32 c = idx % cols;
				f32 x = origin.x + c * (cell_w + gap);
				f32 y = origin.y + r * (cell_h + gap);

				ImVec2 p0 = ImVec2(x, y);
				ImVec2 p1 = ImVec2(x + cell_w, y + cell_h);

				// Background
				dl->AddRectFilled(p0, p1, bg_col);

				// Fill from bottom proportional to usage
				f32 pct = g_display_stats.per_cpu_percent[idx] / 100.0f;
				if (pct > 1.0f) pct = 1.0f;
				if (pct > 0.0f) {
					f32 fill_y = y + cell_h * (1.0f - pct);
					dl->AddRectFilled(ImVec2(x, fill_y), p1, fill_col);
				}

				// Tooltip on hover
				if (ImGui::IsMouseHoveringRect(p0, p1)) {
					ImGui::BeginTooltip();
					ImGui::Text("CPU %d: %.1f%%", idx, g_display_stats.per_cpu_percent[idx]);
					ImGui::EndTooltip();
				}
			}

			ImGui::Dummy(ImVec2(total_w, rows * (cell_h + gap) - gap));
		} else {
			// Single overall CPU graph
			f32 graph_h = section_height - line_h;
			if (graph_h < 60.0f) graph_h = 60.0f;
			draw_graph(nullptr, &g_cpu_history, g_display_stats.cpu_percent, nullptr, cpu_color, graph_h);
		}
	}

	ImGui::Spacing();

	// Memory section
	{
		char used_str[32], total_str[32];
		format_bytes(used_str, sizeof(used_str), g_display_stats.used_physical);
		format_bytes(total_str, sizeof(total_str), g_display_stats.total_physical);

		f32 mem_percent = 0.0f;
		if (g_display_stats.total_physical > 0)
			mem_percent = (f32)g_display_stats.used_physical / (f32)g_display_stats.total_physical * 100.0f;

		// Memory info line
		char mem_label[256];
		i32 pos = snprintf(mem_label, sizeof(mem_label), "Memory  %.1f%%   %s / %s",
			mem_percent, used_str, total_str);
		if (g_sys_info.ram_speed_mhz > 0)
			pos += snprintf(mem_label + pos, sizeof(mem_label) - pos, "   %u MT/s", g_sys_info.ram_speed_mhz);
		if (g_sys_info.ram_slots_total > 0)
			snprintf(mem_label + pos, sizeof(mem_label) - pos, "   %d of %d slots used",
				g_sys_info.ram_slots_used, g_sys_info.ram_slots_total);
		ImGui::TextColored(mem_color, "%s", mem_label);

		f32 graph_h = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing();
		if (graph_h < 60.0f) graph_h = 60.0f;
		draw_graph(nullptr, &g_mem_history, mem_percent, nullptr, mem_color, graph_h);
	}
}


// App callbacks


static void taskman_init()
{
	// Load NtQuerySystemInformation
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	g_nt_query_system_info = (NtQuerySystemInformationFn)GetProcAddress(ntdll, "NtQuerySystemInformation");

	// Collect static system info
	collect_system_info(&g_sys_info);

	// Per-CPU data
	g_cpu_count = get_processor_count();
	if (g_cpu_count > MAX_CPUS) g_cpu_count = MAX_CPUS;
	g_per_cpu_histories = (GraphHistory*)virtual_alloc(g_cpu_count * sizeof(GraphHistory));
	memset(g_per_cpu_histories, 0, g_cpu_count * sizeof(GraphHistory));
	g_prev_cpu_times = (CpuTimes*)virtual_alloc(g_cpu_count * sizeof(CpuTimes));
	g_cur_cpu_times = (CpuTimes*)virtual_alloc(g_cpu_count * sizeof(CpuTimes));
	memset(g_prev_cpu_times, 0, g_cpu_count * sizeof(CpuTimes));
	memset(g_cur_cpu_times, 0, g_cpu_count * sizeof(CpuTimes));

	// Allocate NQSI buffer
	g_nqsi_buffer = (u8*)virtual_alloc(g_nqsi_buffer_size);

	// Allocate snapshots
	snapshot_init(&g_staging_snapshot, MAX_PROCESSES);
	snapshot_init(&g_display_snapshot, MAX_PROCESSES);
	snapshot_init(&g_prev_snapshot, MAX_PROCESSES);

	// Sorted indices
	g_sorted_indices = (i32*)virtual_alloc(MAX_PROCESSES * sizeof(i32));

	// Init sync primitives
	lock_init(&g_snapshot_lock);
	lock_init(&g_collector_lock);
	condvar_init(&g_collector_cv);

	// Graph history
	memset(&g_cpu_history, 0, sizeof(g_cpu_history));
	memset(&g_mem_history, 0, sizeof(g_mem_history));

	// Selection adapter
	g_selection.AdapterIndexToStorageId = selection_adapter;

	// Settings handler
	ImGuiSettingsHandler handler;
	handler.TypeName = "TaskManager";
	handler.TypeHash = ImHashStr("TaskManager");
	handler.ReadOpenFn = settings_read_open;
	handler.ReadLineFn = settings_read_line;
	handler.WriteAllFn = settings_write_all;
	ImGui::AddSettingsHandler(&handler);

	// Start collector thread
	g_collector_thread = thread_create(collector_thread_func, nullptr);
}

static void taskman_tick(TempAllocator*)
{
	// Check for new snapshot from background thread
	// Freeze the display snapshot while the user is box-selecting so rows don't shift
	ImGuiBoxSelectState& bs = GImGui->BoxSelectState;
	bool box_selecting = bs.IsActive || bs.IsStarting;
	if (!box_selecting && g_staging_ready.load(std::memory_order_acquire)) {
		// Swap display and staging process buffers
		lock_write(&g_snapshot_lock);

		ProcessInfo* tmp = g_display_snapshot.processes;
		g_display_snapshot.processes = g_staging_snapshot.processes;
		g_display_snapshot.count = g_staging_snapshot.count;
		g_display_snapshot.wall_time = g_staging_snapshot.wall_time;
		g_display_snapshot.system_kernel_time = g_staging_snapshot.system_kernel_time;
		g_display_snapshot.system_user_time = g_staging_snapshot.system_user_time;
		g_display_snapshot.system_idle_time = g_staging_snapshot.system_idle_time;
		g_staging_snapshot.processes = tmp;
		g_staging_snapshot.count = 0;

		g_display_stats = g_staging_stats;

		unlock_write(&g_snapshot_lock);

		g_staging_ready.store(false, std::memory_order_release);

		// Update graph histories
		graph_history_push(&g_cpu_history, g_display_stats.cpu_percent);
		for (i32 i = 0; i < g_display_stats.cpu_count && i < g_cpu_count; i++)
			graph_history_push(&g_per_cpu_histories[i], g_display_stats.per_cpu_percent[i]);
		f32 mem_pct = 0.0f;
		if (g_display_stats.total_physical > 0)
			mem_pct = (f32)g_display_stats.used_physical / (f32)g_display_stats.total_physical * 100.0f;
		graph_history_push(&g_mem_history, mem_pct);

		g_has_first_snapshot = true;

		// Prune selection: remove PIDs that no longer exist
		if (g_selection.Size > 0) {
			ImVector<ImGuiID> stale;
			void* it = nullptr;
			ImGuiID id;
			while (g_selection.GetNextSelectedItem(&it, &id)) {
				bool found = false;
				for (i32 i = 0; i < g_display_snapshot.count; i++) {
					if (g_display_snapshot.processes[i].pid == (u32)id) {
						found = true;
						break;
					}
				}
				if (!found)
					stale.push_back(id);
			}
			for (int i = 0; i < stale.Size; i++)
				g_selection.SetItemSelected(stale[i], false);
		}

		// Rebuild filtered indices for the new data
		rebuild_filtered_indices();
	}

	// Check if search changed
	if (strcmp(g_search_buf, g_prev_search_buf) != 0) {
		token_query_parse(&g_search_query, g_search_buf);
		memcpy(g_prev_search_buf, g_search_buf, sizeof(g_search_buf));
		rebuild_filtered_indices();
	}

	// Main window
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("taskman", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

	draw_title_bar_buttons(TITLE_BAR_HEIGHT);
	draw_title_bar_title(TITLE_BAR_HEIGHT, "Task Manager", nullptr);

	// Move cursor below the title bar chrome with padding
	f32 dpi_scale_tb = window_get_dpi_scale(host_hwnd());
	f32 pad_y = ImGui::GetStyle().WindowPadding.y;
	ImGui::SetCursorPosY(floorf((f32)TITLE_BAR_HEIGHT * dpi_scale_tb) + pad_y);

	// Remember where the tab bar row starts so we can place the gear button
	f32 tab_row_y = ImGui::GetCursorPosY();

	// Settings modal
	{
		f32 dpi_scale = window_get_dpi_scale(host_hwnd());
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(floorf(360.0f * dpi_scale), 0.0f));
		if (ImGui::BeginPopupModal("Settings", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {

			ImGui::Text("Refresh interval");
			ImGui::SetNextItemWidth(floorf(120.0f * dpi_scale));
			if (ImGui::Combo("##refresh", &g_refresh_interval_idx, REFRESH_LABELS, REFRESH_INTERVAL_COUNT)) {
				ImGui::MarkIniSettingsDirty();
				lock_write(&g_collector_lock);
				condvar_wake_all(&g_collector_cv);
				unlock_write(&g_collector_lock);
			}

			ImGui::Separator();

			ImGui::Text("Performance");
			if (ImGui::Checkbox("Show per-CPU graphs", &g_show_per_cpu))
				ImGui::MarkIniSettingsDirty();

			ImGui::Separator();

			if (ImGui::Button("Close", ImVec2(-1, 0)))
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}
	}

	// Tab bar
	if (ImGui::BeginTabBar("##tabs")) {
		if (ImGui::BeginTabItem("Details")) {
			if (g_has_first_snapshot) {
				render_details_tab();
			} else {
				ImGui::TextColored(theme_color_text_indexing(), "Collecting process data...");
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Performance")) {
			if (g_has_first_snapshot) {
				render_performance_tab();
			} else {
				ImGui::TextColored(theme_color_text_indexing(), "Waiting for first sample...");
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	// Gear button on the tab bar row — drawn manually to avoid expanding content rect
	{
		f32 gear_size = ImGui::GetFrameHeight();
		ImVec2 win_pos = ImGui::GetWindowPos();
		ImVec2 pos = ImVec2(
			win_pos.x + ImGui::GetContentRegionMax().x - gear_size,
			win_pos.y + tab_row_y);
		ImVec2 p1 = ImVec2(pos.x + gear_size, pos.y + gear_size);

		ImGuiID gear_id = ImGui::GetID("##gear");
		ImGui::SetCursorScreenPos(pos);
		bool hovered = ImGui::IsMouseHoveringRect(pos, p1);
		bool clicked = hovered && ImGui::IsMouseClicked(0);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImU32 bg = hovered ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : ImGui::GetColorU32(ImGuiCol_Button);
		dl->AddRectFilled(pos, p1, bg, ImGui::GetStyle().FrameRounding);

		ImVec2 text_size = ImGui::CalcTextSize(ICON_GEAR);
		dl->AddText(ImVec2(pos.x + (gear_size - text_size.x) * 0.5f,
			pos.y + (gear_size - text_size.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Text), ICON_GEAR);

		if (clicked)
			ImGui::OpenPopup("Settings");
		(void)gear_id;
	}

	ImGui::End();

	// Escape: clear selection
	if (g_selection.Size > 0 && ImGui::IsKeyPressed(ImGuiKey_Escape))
		g_selection.Clear();
}

static void taskman_begin_shutdown()
{
	g_shutdown.store(true, std::memory_order_release);

	// Wake the sleeping collector thread
	lock_write(&g_collector_lock);
	condvar_wake_all(&g_collector_cv);
	unlock_write(&g_collector_lock);
}

static void taskman_wait_for_shutdown()
{
	if (g_collector_thread) {
		thread_join(g_collector_thread);
		g_collector_thread = nullptr;
	}

	snapshot_destroy(&g_staging_snapshot);
	snapshot_destroy(&g_display_snapshot);
	snapshot_destroy(&g_prev_snapshot);

	if (g_sorted_indices) {
		virtual_free(g_sorted_indices);
		g_sorted_indices = nullptr;
	}

	if (g_nqsi_buffer) {
		virtual_free(g_nqsi_buffer);
		g_nqsi_buffer = nullptr;
	}

	if (g_per_cpu_histories) {
		virtual_free(g_per_cpu_histories);
		g_per_cpu_histories = nullptr;
	}
	if (g_prev_cpu_times) {
		virtual_free(g_prev_cpu_times);
		g_prev_cpu_times = nullptr;
	}
	if (g_cur_cpu_times) {
		virtual_free(g_cur_cpu_times);
		g_cur_cpu_times = nullptr;
	}
}

App* taskman_get_app()
{
	static App app = {};
	app.name = "Task Manager";
	app.app_id = "taskman";
	app.init = taskman_init;
	app.tick = taskman_tick;
	app.on_activated = nullptr;
	app.on_resize = nullptr;
	app.begin_shutdown = taskman_begin_shutdown;
	app.wait_for_shutdown = taskman_wait_for_shutdown;
	app.hotkeys = nullptr;
	app.hotkey_count = 0;
	app.initial_width = 1200;
	app.initial_height = 800;
	app.title_bar_height = TITLE_BAR_HEIGHT;
	app.title_bar_buttons_width = TITLE_BAR_HEIGHT * TITLE_BAR_BUTTONS;
	app.use_system_tray = false;
	return &app;
}
