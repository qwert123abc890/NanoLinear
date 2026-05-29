#pragma once
#include<vector>
#include <cstdint>
#include<cstddef>
#include<cassert>//断言
#include"BinHeader.h"

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
inline int safe_find_first_bin(uint64_t available_bins) noexcept {
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

class BinManager
{
private:
	void* os_page_address;
	size_t os_page_size;
	BinHeader virtual_heads[64];
	char* current_top;
	uint64_t bin_bitmap; //使用位图来记录每个bin是否有可用的内存块，1表示有可用内存块，0表示没有可用内存块


	//最小内存块大小为32字节,其中数据区字节大小为16字节，最大内存块的数据区字节大小为1024字节,总大小为1040字节，序号为63，超过1040字节的内存块则使用mmap内存映射
	//不过还需要进一步细分，32字节的内存块被划分为fast_bin,不需要合并
	//small_bin
	//large_bin
	int get_bin_index(size_t size)
	{
		if (size > 1040) return 63; //超过1040字节的内存块则使用mmap内存映射
		return static_cast<int>((size + 15) / 16) - 2;
	}


	void* request_from_os(size_t need_size) {
		// 无论用户要多少，一律以 4KB 物理页为基本单位向操作系统批发
		// 一个哨兵占 48 字节，首尾共 96 字节
		size_t page_size = (need_size + 96 + 4095) & ~4095;
		this->os_page_size = page_size;

#ifdef _WIN32 
		void* mem = VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		this->os_page_address = mem;
		if (mem) {
			[[likely]]; // C++23/MSVC 标准大括号内侧写法

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

	void insert_into_bin(BinHeader* ptr) 
	{
		size_t bin_size = ptr->size;
		int bin_index = get_bin_index(bin_size);
		BinHeader* head = &virtual_heads[bin_index];

		if (bin_index < 63)
		{
			bin_bitmap |= (1ULL << bin_index); //更新位图，标记该bin有可用内存块
			ptr->next = head->next;
			ptr->prev = head;
			head->next->prev = ptr;
			head->next = ptr;
		}
		else
		{
			BinHeader* curr = head;
			//bin64的内存块按从小到大排序
			while (curr->next != head && curr->next->size < ptr->size)
			{
				curr = curr->next;
			}
			curr->prev->next = ptr;
			ptr->prev = curr->prev;
			curr->prev = ptr;
			ptr->next = curr;
		}
		
	}

public:
	size_t alignas_up(size_t x, size_t alignasment)
	{
		return (x + alignasment - 1) & ~(alignasment - 1);
	}

	BinManager(const size_t& page_need_size = 4096) : bin_bitmap(0)
	{
		for (int i = 0; i <= 63; ++i)
		{
			virtual_heads[i].prev = &virtual_heads[i];
			virtual_heads[i].next = &virtual_heads[i];
			virtual_heads[i].size = 0;
			virtual_heads[i].is_used = true;
		}
		
		current_top =reinterpret_cast<char*>( request_from_os(page_need_size) );
		assert(((uintptr_t)current_top & 15) == 0 && "If the memory blocks allocated by the operating system are not aligned, it will cause the foundation to crash.");

		////将一个4KB的内存页作为内存池的起始地址，放入第64号bin中，作为所有bin的后备资源
		//BinHeader* head = &virtual_heads[64];
		//BinHeader* first_available_header = reinterpret_cast<BinHeader*>(request_from_os(page_need_size)); // 初始化时先向操作系统批发一个4KB的内存页，作为内存池的起始地址
		//assert(((uintptr_t)first_available_header & 15) == 0 && "If the memory blocks allocated by the operating system are not aligned, it will cause the foundation to crash.");

		//first_available_header->next = head->next;
		//first_available_header->prev = head;
		//head->next->prev = first_available_header;
		//head->next = first_available_header;

	}
	void* allocate(size_t user_need)
	{
		size_t need = sizeof(BinHeader) + alignas_up(user_need, alignof(std::max_align_t)) + sizeof(uint64_t) + sizeof(size_t);

		//优先指针碰撞法
		if (current_top + need <= (char*)os_page_address + os_page_size - 48)
		{
			BinHeader* header_ptr = reinterpret_cast<BinHeader*>(reinterpret_cast<char*>(current_top));
			header_ptr->size = need;
			header_ptr->is_used = true;
			//header_ptr->prev = nullptr;
			//header_ptr->next = nullptr;

			current_top += need;

			uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(header_ptr) + need - sizeof(uint64_t) - sizeof(size_t));
			*canary = 0xDEADBEEFCAFEBABEULL;

			size_t* size_ptr = reinterpret_cast<size_t*>(canary+1);
			*size_ptr = header_ptr->size;

			return header_ptr->user_ptr();
		}

		int user_need_bin = get_bin_index(need); //发现只要存在best_fit就存在分支，原本想减少分支来强制bin为64个
		//使用BitMap来快速判断对应bin是否有可用的内存块
		uint64_t filtered = bin_bitmap & ((~0ULL) << user_need_bin);
		uint64_t great_bin = safe_find_first_bin(filtered);
		if (great_bin == -1)
		{
			[[unlikely]];
			return nullptr; //没有合适的内存块了，返回nullptr
		}
		BinHeader* curr_head = &virtual_heads[great_bin];
		BinHeader* ptr ;
		if (great_bin != 63)
		{
			[[likely]];
			ptr = curr_head->next;
		}
		else
		{
			ptr = curr_head->find_best_fit(need);
		}
		ptr->remove_from_list();
		if (curr_head->next == curr_head) bin_bitmap &= ~(1ULL << great_bin); //更新位图，标记该bin没有可用内存块了
		BinHeader* remaining_ptr = ptr->split(need);
		if (remaining_ptr) 
		{
			[[likely]];
			insert_into_bin(remaining_ptr);
		}
			
		uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(ptr) + ptr->size - sizeof(uint64_t) - sizeof(size_t));
		*canary = 0xDEADBEEFCAFEBABEULL;

		size_t* size_ptr = reinterpret_cast<size_t*>(canary + 1);
		*size_ptr = ptr->size;

		return ptr->user_ptr();
	}
		//当need为64

		/*
		//发现没有合适的内存块，继续向下一个bin寻找，直到找到合适的内存块或者所有bin都找完了
		for (int i = bin_index; i < 64; ++i)
		{
			BinHeader* head = &virtual_heads[i];
			if (head->next == head)
			{
				continue;
			}
			BinHeader* ptr = head->next;
			ptr->remove_from_list();
			BinHeader* remaining_ptr = ptr->split(need);
			if (remaining_ptr) insert_into_bin(remaining_ptr);
			//user_need不一定对齐
			uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(ptr) + ptr->size - sizeof(uint64_t) - sizeof(size_t));
			*canary = 0xDEADBEEFCAFEBABEULL;

			size_t* size_ptr = reinterpret_cast<size_t*>(canary + 1);
			*size_ptr = ptr->size;

			return ptr->user_ptr();

		}

		BinHeader* best_fit = virtual_heads[64].find_best_fit(need);
		//考虑是否去除该if-else语句，实际应用场景一定满足了应用需求，无需过多的判断，反而增加了代码复杂度和维护难度
		if [[likely]] (best_fit)
		{
			best_fit->remove_from_list();
			BinHeader* remaining_block_ptr = best_fit->split(need);
			if (remaining_block_ptr)
			{
				insert_into_bin(remaining_block_ptr);
			}
			best_fit->is_used = true;

			uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(best_fit) + best_fit->size - sizeof(uint64_t) - sizeof(size_t));
			*canary = 0xDEADBEEFCAFEBABEULL;
			size_t* size_ptr = reinterpret_cast<size_t*>(canary + 1);
			*size_ptr = best_fit->size;

			return best_fit->user_ptr();
		}

		*/
		////所有bin都找完了，还是没有合适的内存块，向操作系统批发一个4KB的内存页

		//void* os_page = request_from_os(need);
		//if (!os_page) return nullptr;

		//BinHeader* header_ptr = reinterpret_cast<BinHeader*>(os_page);
		//header_ptr->prev = nullptr;
		//header_ptr->next = nullptr;
		//header_ptr->size = need;
		//header_ptr->is_used = true;

		//uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(header_ptr->user_ptr()) + user_need);
		//*canary = 0xDEADBEEFCAFEBABEULL;

		//size_t* header_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(canary) + sizeof(uint64_t));
		//*header_size_ptr = need;
		////此时的header_ptr的prev和next均未定义，并不一定为nullptr,易突破if(header_ptr->next)引发异常

		//BinHeader* remaining_ptr = reinterpret_cast<BinHeader*>((char*)(header_ptr)+need);
		//remaining_ptr->size = (need + 2 * sizeof(BinHeader) + 2 * sizeof(size_t) + 4095) & ~4095 - need - 2 * sizeof(BinHeader) - 2 * sizeof(size_t);
		//size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(header_ptr) + ((need + 2 * sizeof(BinHeader) + 2 * sizeof(size_t) + 4095) & ~4095) - sizeof(size_t) - sizeof(BinHeader) - sizeof(size_t));
		//*size_ptr = remaining_ptr->size;

		//if (remaining_ptr) insert_into_bin(remaining_ptr);
		//return header_ptr->user_ptr();

	//在Header处存储该指针所属于的页表，方便核实是否在合法位置
	void deallocate(void* user_ptr)
	{
		if (!user_ptr)
		{
			[[unlikely]];
			std::cerr << "Attempt to deallocate a null pointer\n";
			return;
		}
		//检查一下用户传入的指针是否合法，
		if (user_ptr < os_page_address || user_ptr >= (char*)os_page_address + os_page_size)
		{
			[[unlikely]];
			std::cerr << "Attempt to deallocate a pointer that is out of bounds\n";
			return;
		}

		BinHeader* header_ptr = (BinHeader*)((char*)user_ptr - sizeof(BinHeader));
		//是否非法越界写入
		uint64_t* canary_ptr = reinterpret_cast<uint64_t*>((char*)header_ptr + header_ptr->size - sizeof(uint64_t) - sizeof(size_t));
		if (*canary_ptr != 0xDEADBEEFCAFEBABEULL)
		{
			[[unlikely]];
			std::cerr << "Memory corruption detected: canary value has been altered\n";
			return;
		}

		// 是否被重复释放等
		if (!header_ptr->is_used)
		{
			[[unlikely]];
			std::cerr << "Attempt to deallocate a pointer that has already been deallocated\n";
			return;
		}

		bin_bitmap |= (1ULL << get_bin_index(header_ptr->size)); //更新位图，标记该bin有可用内存块了
		header_ptr->is_used = false;

		//合并相邻的空闲块
		header_ptr = header_ptr->merge(this->current_top);

		insert_into_bin(header_ptr);
		
	}

	~BinManager()
	{
		#ifdef _WIN32
					VirtualFree(os_page_address, 0, MEM_RELEASE);
		#else
					munmap(os_page_address,os_page_size); 
		#endif
	}

};
