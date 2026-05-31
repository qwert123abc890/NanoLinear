#pragma once
#include"BinHeader.h"
#include<vector>
#include <cstdint>
#include<cstddef>
#include<cassert>//断言

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/**
 * @brief 工业级跨平台 O(1) 位图检索安全函数
 * @param available_bins 经过掩码过滤后的 64 位无符号位图整数
 * @return 如果找到有货的 Bin，返回其索引(0~63)；若全空(为0)，则安全返回 -1
 */
inline int safe_find_first_bin(uint64_t available_bins) noexcept 
{
#ifdef _WIN32
	unsigned long index;
	// Windows 自带防弹机制：如果为 0，函数直接返回 0 (false)，不进入 if
	if (_BitScanForward64(&index, available_bins))
	{
		return static_cast<int>(index);
	}
	return -1; // 完美拦截全 0 情况
#else
	// Linux 环境：必须进行前置的安全零检查（Zero Check）
	// 别担心，现代 CPU 带有零标志位（Zero Flag），这个判断在汇编层面极快
	if ((available_bins == 0)
	{
		[[unlikely]];
		return -1;
	}
	// 确保绝对不为 0 后，再安全调用硬件指令
	return __builtin_ctzll(available_bins);
#endif
}


class TwoLevelBitmapAllocator
{
private:
	uint64_t first_bin_bitmap; //使用位图来记录每个bin是否有可用的内存块，1表示有可用内存块，0表示没有可用内存块
	uint64_t second_bin_bitmap[64];
	char* bin_head[64];
	//常规bin都有一大块连续的内存块，大小固定，无需Header
	void* os_page_address;
	size_t os_page_size;
	char* current_top;
	size_t alignasment;
	void* request_from_os(size_t need_size) 
	{
		size_t page_size = (need_size + 96 + 4095) & ~4095;
		this->os_page_size = page_size;

#ifdef _WIN32 
		void* mem = VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		this->os_page_address = mem;
		if (mem) 
		{
			[[likely]]; 

			BinHeader* header_ptr = reinterpret_cast<BinHeader*>(mem);
			header_ptr->size = 48;
			header_ptr->is_used = true;
			header_ptr->prev = nullptr;
			header_ptr->next = nullptr;

			size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + 40);
			*size_ptr = 48;

			char* back_sentinel_start = reinterpret_cast<char*>(mem) + page_size - 48;
			BinHeader* back_ptr = reinterpret_cast<BinHeader*>(back_sentinel_start);
			back_ptr->size = 48;
			back_ptr->is_used = true;
			back_ptr->prev = nullptr;
			back_ptr->next = nullptr;

			size_t* back_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + page_size - sizeof(size_t));
			*back_size_ptr = 48;

			mem = reinterpret_cast<void*>(reinterpret_cast<char*>(mem) + 48);
		}
		return mem;
#else 
		// Linux/MacOS 的原生批发接口
		void* mem = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		this->os_page_address = mem;
		if (mem == MAP_FAILED)
		{
			[[unlikely]];
			return nullptr;
		}

		BinHeader* header_ptr = reinterpret_cast<BinHeader*>(mem);
		header_ptr->size = 48;
		header_ptr->is_used = true;
		header_ptr->prev = nullptr;
		header_ptr->next = nullptr;

		size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + 40);
		*size_ptr = 48;

		char* back_sentinel_start = reinterpret_cast<char*>(mem) + page_size - 48;
		BinHeader* back_ptr = reinterpret_cast<BinHeader*>(back_sentinel_start);
		back_ptr->size = 48;
		back_ptr->is_used = true;
		back_ptr->prev = nullptr;
		back_ptr->next = nullptr;

		size_t* back_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + page_size - sizeof(size_t));
		*back_size_ptr = 48;

		mem = reinterpret_cast<void*>(reinterpret_cast<char*>(mem) + 48);
		return mem;
#endif 
	}
	int get_bin_index(size_t size)
	{
		if (size > 1040) return 63; //超过1040字节的内存块则使用mmap内存映射
		return static_cast<int>((size + 15) / 16) - 2;
	}
public:
	TwoLevelBitmapAllocator(const size_t& page_need_size = 4096) : alignasment(alignof(std::max_align_t)), first_bin_bitmap(0)
	{
		memset(second_bin_bitmap, 0, sizeof(second_bin_bitmap));
		//for (int i = 0; i <= 63; ++i)
		//{
		//	second_bin_bitmap[i] = 0;
		//	bin_head[i] = nullptr;
		//}
		
		current_top =reinterpret_cast<char*>( request_from_os(page_need_size) );
		assert(((uintptr_t)current_top & 15) == 0 && "If the memory blocks allocated by the operating system are not aligned, it will cause the foundation to crash.");

	}
	void* allocate(size_t user_need)
	{
		size_t need = (user_need + 2*sizeof(char) + sizeof(bool) + sizeof(uint64_t) + alignasment - 1) & ~(alignasment - 1);
		int user_need_bin = get_bin_index(need); //发现只要存在best_fit就存在分支，原本想减少分支来强制bin为64个
		//使用BitMap来快速判断对应bin是否有可用的内存块
		uint64_t filtered = first_bin_bitmap & ((~0ULL) << user_need_bin);
		if (!filtered)
		{
			[[unlikely]];
			//发现没有合适的内存块，指针碰撞法
			size_t total_need = need * 64;
			//指针碰撞法,考虑是把整个bin桶都顺便填满
			if (current_top + total_need <= (char*)os_page_address + os_page_size - 48)
			{
				[[likely]];
				char* available_block = current_top;
				current_top += total_need;

				//更新位图
				first_bin_bitmap |= (1ULL << user_need_bin);
				second_bin_bitmap[user_need_bin] = (~0ULL) ^ 1ULL;
				bin_head[user_need_bin] = available_block;
				
				char* first_bin_index = available_block;
				*first_bin_index = user_need_bin;
				char* second_bin_index = first_bin_index + sizeof(char);
				*second_bin_index = 0;
				bool* is_used_ptr = (bool*)(second_bin_index + sizeof(char));
				*is_used_ptr = true;
				uint64_t* canary = (uint64_t*)(available_block + need - sizeof(uint64_t));
				*canary = 0xDEADBEEFCAFEBABEULL;

				return available_block + 2 * sizeof(char) + sizeof(bool);
			
			}
			return nullptr;
		}
		char first_bin;

#ifdef _WIN32
		unsigned long index;
		_BitScanForward64(&index, filtered);
		first_bin = static_cast<char>(index);
#else
		first_bin = __builtin_ctzll(filtered);
		
#endif

		//只要filtered不为0，就一定能找到first_bin

#ifdef _WIN32
		unsigned long index2;
		//size_t available_index = _BitScanForward64(&index2, second_bin_bitmap[first_bin]);
		_BitScanForward64(&index2, second_bin_bitmap[first_bin]);
		size_t available_index = static_cast<char>(index2);

#else

		size_t available_index = __builtin_ctzll(second_bin_bitmap[first_bin]);
#endif
		//需要知道当前bin桶的内存块大小，才能计算出block_size和available_block
		//更新位图,标记该bin的available_index位置的内存块被占用了
		unsigned long sbb = second_bin_bitmap[first_bin];
		second_bin_bitmap[first_bin] &= ~(1ULL << available_index);
		if (second_bin_bitmap[first_bin] == 0)
		{
			[[unlikely]];
			first_bin_bitmap &= ~(1ULL << first_bin); //更新位图，标记该bin没有可用内存块了
		}
		size_t block_size = (first_bin + 2) * 16;
		char* available_block = bin_head[first_bin] + available_index * block_size;

		char* first_bin_index = available_block;
		*first_bin_index = first_bin;

		char* second_bin_index = first_bin_index + sizeof(char);
		*second_bin_index = available_index;

		bool* is_used_ptr = (bool*)(second_bin_index + sizeof(char));
		*is_used_ptr = true;

		uint64_t* canary = (uint64_t*)(available_block + block_size - sizeof(uint64_t));
		*canary = 0xDEADBEEFCAFEBABEULL;

		return available_block+2*sizeof(char)+sizeof(bool);
		
	}

	void deallocate(void* user_ptr)
	{
		bool* is_used_ptr = (bool*)user_ptr - 1;
		if (*is_used_ptr)
		{
			[[unlikely]];

		}
		char* first_bin_index = (char*)(is_used_ptr - 2*sizeof(char));
		uint64_t* canary = (uint64_t*)((char*)user_ptr + ((size_t)(*first_bin_index) + 2) * 16 - sizeof(uint64_t));
		if (*canary != 0xDEADBEEFCAFEBABEULL)
		{
			[[unlikely]];
			//警报

		}
		char* second_bin_index = first_bin_index + sizeof(char);
		second_bin_bitmap[(size_t)(*first_bin_index)] |= (1ULL << (*second_bin_index)); //更新位图，标记该bin的available_index位置的内存块被释放了
		*is_used_ptr = false;
		first_bin_bitmap |= (1ULL << (*first_bin_index)); //更新位图，标记该bin有可用内存块了

	}
	~TwoLevelBitmapAllocator()
	{
#ifdef _WIN32
		VirtualFree(os_page_address, 0, MEM_RELEASE);
#else
		munmap(os_page_address, os_page_size);
#endif
	}
};
