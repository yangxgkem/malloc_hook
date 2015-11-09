#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>
#include <stdarg.h>


#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(uint32_t) //后缀大小,用来存放 handleid

static size_t _used_memory = 0; //共分配了多少字节内存
static size_t _memory_block = 0; //分配了多少块内存

typedef struct _mem_data {
	uint32_t handle; //模块id handleid
	ssize_t allocated; //已分配了多少字节内存
	ssize_t blocknum; //分配了多少块内存
} mem_data;

static mem_data mem_stats[SLOT_SIZE];

//获取某服务已分配了多少字节内存(地址), 如果此服务未在内存管理中,则加入管理
static ssize_t*
get_allocated_field(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	mem_data *data = &mem_stats[h];
	uint32_t old_handle = data->handle;
	ssize_t old_alloc = data->allocated;
	ssize_t old_blocknum = data->blocknum;
	if(old_handle == 0 || old_alloc <= 0) {
		// data->allocated may less than zero, because it may not count at start.
		if(!__sync_bool_compare_and_swap(&data->handle, old_handle, handle)) {
			return 0;
		}
		if (old_alloc < 0) {
			__sync_bool_compare_and_swap(&data->allocated, old_alloc, 0);
		}
		if (old_blocknum < 0) {
			__sync_bool_compare_and_swap(&data->blocknum, old_blocknum, 0);
		}
	}
	if(data->handle != handle) {
		return 0;
	}
	return &data->allocated;
}

//获取某服务已分配多少块内存(地址)
static ssize_t*
get_blocknum(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	mem_data *data = &mem_stats[h];
	if(data->handle != handle) {
		return 0;
	}
	return &data->blocknum;
}

//申请内存统计
inline static void
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	__sync_add_and_fetch(&_used_memory, __n);
	__sync_add_and_fetch(&_memory_block, 1);

	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		__sync_add_and_fetch(allocated, __n);
	}

	ssize_t* blocknum = get_blocknum(handle);
	if(blocknum) {
		__sync_add_and_fetch(blocknum, 1);
	}
}

//释放内存统计
inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	__sync_sub_and_fetch(&_used_memory, __n);
	__sync_sub_and_fetch(&_memory_block, 1);

	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		__sync_sub_and_fetch(allocated, __n);
	}

	ssize_t* blocknum = get_blocknum(handle);
	if(blocknum) {
		__sync_sub_and_fetch(blocknum, 1);
	}
}

//把 handleid 压入 ptr 内存尾部, 并进行申请内存统计
inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = server_current_handle();
	size_t size = malloc_usable_size(ptr);
	uint32_t *p = (uint32_t *)(ptr + size - PREFIX_SIZE);
	memcpy(p, &handle, sizeof(handle));

	update_xmalloc_stat_alloc(handle, size);
	return ptr;
}

//根据 ptr 提取 handleid , 并进行释放内存统计
inline static void*
clean_prefix(char* ptr) {
	size_t size = malloc_usable_size(ptr);
	uint32_t *p = (uint32_t *)(ptr + size - PREFIX_SIZE);
	uint32_t handle;
	memcpy(&handle, p, sizeof(handle));
	update_xmalloc_stat_free(handle, size);
	return ptr;
}

//内存分配失败提示
static void
malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n", size);
	fflush(stderr);
	abort();
}

void *
server_malloc(size_t size) {
	void* ptr = malloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void
server_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr);
	free(rawptr);
}

void *
server_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return server_malloc(size);

	void* rawptr = clean_prefix(ptr);
	void *newptr = realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr);
}

void *
server_calloc(size_t nmemb, size_t size) {
	void* ptr = calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

//获取当前已分配多少字节内存
size_t
malloc_used_memory(void) {
	return _used_memory;
}

//获取当前已分配内存块
size_t
malloc_memory_block(void) {
	return _memory_block;
}


//打印运行日志
void
__RUNTIME(const char *msg, ...) {
	char tmp[256];
	char *data = NULL;

	va_list ap;
	va_start(ap,msg);
	int len = vsnprintf(tmp, 256, msg, ap);//读取可变参数"...",按照格式msg输出到tmp下,最大输出长度为256
	va_end(ap);
	
	if (len < 256) {
		size_t sz = strlen(tmp);
		data = server_malloc(sz+1);
		memcpy(data, tmp, sz+1);
	} else {
		int max_size = 256;
		for (;;) {
			max_size *= 2;
			data = server_malloc(max_size);
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);
			if (len < max_size) {
				break;
			}
			server_free(data);
		}
	}

	printf("%s\n", data);
	server_free(data);
}

//打印内存分配情况
void
dump_c_mem() {
	int i;
	size_t total_all,total_block = 0;
	__RUNTIME("dump all service mem:");
	for(i=0; i<SLOT_SIZE; i++) {
		mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total_all += data->allocated;
			total_block += data->blocknum;
			__RUNTIME("0x%x -> %zdkb, %zd", data->handle, data->allocated >> 10, data->blocknum);
		}
	}
	__RUNTIME("+total: %zdkb, %zd",total_all >> 10, total_block);
	__RUNTIME("+total2: %zdkb, %zd",malloc_used_memory() >> 10, malloc_memory_block());
}