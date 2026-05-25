#pragma once
#include<vector>
#include<cstddef>
#include"Bin_Header.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
class BinManager
{
private:
	void* os_page_address;
	size_t os_page_size;
	BinHeader virtual_heads[65];
	//最小内存块大小为32字节,其中数据区字节大小为16字节，最大内存块的数据区字节大小为1024字节,总大小为1040字节，序号为63，超过1040字节的内存块则使用mmap内存映射
	//不过还需要进一步细分，32字节的内存块被划分为fast_bin,不需要合并
	//small_bin
	//large_bin
	int get_bin_index(size_t size)
	{
		if (size > 1040) return 64; //超过1040字节的内存块则使用mmap内存映射
		return static_cast<int>((size + 15) / 16) - 2;
	}


	void* request_from_os(size_t need_size)
	{
		// 无论用户要多少，底层一律以 4KB 物理页为基本单位向操作系统批发
		//一个哨兵需要 sizeof(BinHeader) + sizeof(size_t) ,需要16字节
		size_t page_size = (need_size + 2 * sizeof(BinHeader) + 2 * sizeof(size_t) + 4095) & ~4095;
		this->os_page_size = page_size;

#ifdef _WIN32
		void* mem = VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		this->os_page_address = mem;

		if (mem)
		{
			BinHeader* header_ptr = reinterpret_cast<BinHeader*>(mem);
			header_ptr->size = sizeof(BinHeader) + sizeof(size_t);
			size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + sizeof(BinHeader));
			*size_ptr = sizeof(BinHeader) + sizeof(size_t);
			header_ptr->is_used = true;

			BinHeader* back_ptr = reinterpret_cast<BinHeader*>(reinterpret_cast<char*>(mem) + page_size - sizeof(BinHeader) - sizeof(size_t));
			size_t* back_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + page_size - sizeof(size_t));
			*back_size_ptr = sizeof(BinHeader) + sizeof(size_t);
			back_ptr->size = sizeof(BinHeader) + sizeof(size_t);
			back_ptr->is_used = true;

			mem = reinterpret_cast<void*>(reinterpret_cast<char*>(mem) + sizeof(BinHeader) + sizeof(size_t));  // 返回给用户的地址，跳过头部
		}
		return mem;
#else
		// Linux/MacOS 的原生批发接
		void* mem = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		this->os_page_address = mem;

		if (mem == MAP_FAILED) return nullptr;


		BinHeader* header_ptr = reinterpret_cast<BinHeader*>(mem);
		size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + sizeof(BinHeader));
		*size_ptr = sizeof(BinHeader) + sizeof(size_t);
		header_ptr->size = sizeof(BinHeader) + sizeof(size_t);
		header_ptr->is_used = sizeof(BinHeader) + sizeof(size_t);

		BinHeader* back_ptr = reinterpret_cast<BinHeader*>(reinterpret_cast<char*>(mem) + page_size - sizeof(size_t));
		back_ptr->size = header_ptr->size;
		size_t* back_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(mem) + page_size - sizeof(size_t));
		*back_size_ptr = sizeof(BinHeader) + sizeof(size_t);
		back_ptr->size = sizeof(BinHeader) + sizeof(size_t);
		back_ptr->is_used = true;

		mem = reinterpret_cast<void*>(reinterpret_cast<char*>(mem) + sizeof(BinHeader) + sizeof(size_t)); // 返回给用户的地址，跳过头部

		return mem;
#endif
	}
	void insert_into_bin(BinHeader* ptr) 
	{
		size_t bin_size = ptr->size;
		int bin_index = get_bin_index(bin_size);
		BinHeader* head = &virtual_heads[bin_index];
		ptr->next = head->next;
		ptr->prev = head;
		head->next->prev = ptr;
		head->next = ptr;
	}
public:
	size_t alignas_up(size_t x, size_t alignasment)
	{
		return (x + alignasment - 1) & ~(alignasment - 1);
	}

	BinManager(const size_t& page_need_size)
	{
		for (int i = 0; i <= 64; ++i)
		{
			virtual_heads[i].prev = &virtual_heads[i];
			virtual_heads[i].next = &virtual_heads[i];
			virtual_heads[i].size = 0;
			virtual_heads[i].is_used = true;
		}
		//将一个4KB的内存页作为内存池的起始地址，放入第64号bin中，作为所有bin的后备资源
		BinHeader* head = &virtual_heads[64];
		BinHeader* first_available_header = reinterpret_cast<BinHeader*>(request_from_os(page_need_size)); // 初始化时先向操作系统批发一个4KB的内存页，作为内存池的起始地址
		first_available_header->next = head->next;
		first_available_header->prev = head;
		head->next->prev = first_available_header;
		head->next = first_available_header;

	}
	void* allocate(size_t user_need)
	{
		size_t need = sizeof(BinHeader) + alignas_up(user_need, alignof(std::max_align_t)) + sizeof(uint64_t) + sizeof(size_t);
		int bin_index = get_bin_index(need);

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

			uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(ptr->user_ptr()) + user_need);
			*canary = 0xDEADBEEFCAFEBABEULL;

			size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(ptr->user_ptr()) + user_need + sizeof(uint64_t));
			*size_ptr = ptr->size;

			return ptr->user_ptr();

		}

		BinHeader* best_fit = virtual_heads[64].find_best_fit(need);
		if (best_fit)
		{
			best_fit->remove_from_list();
			BinHeader* remaining_block_ptr = best_fit->split(need);
			if (remaining_block_ptr)
			{
				insert_into_bin(remaining_block_ptr);
			}
			best_fit->is_used = true;

			uint64_t* canary = reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(best_fit->user_ptr()) + user_need);
			*canary = 0xDEADBEEFCAFEBABEULL;
			size_t* size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(best_fit->user_ptr()) + user_need + sizeof(uint64_t));
			*size_ptr = best_fit->size;

			return best_fit->user_ptr();
		}
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

		return nullptr;

	}
	//在Header处存储该指针所属于的页表，方便核实是否在合法位置
	void deallocate(void* user_ptr)
	{
		if (!user_ptr)
		{
			std::cerr << "Attempt to deallocate a null pointer\n";
			return;
		}
		//检查一下用户传入的指针是否合法，
		if (user_ptr < os_page_address || user_ptr >= (char*)os_page_address + os_page_size)
		{
			std::cerr << "Attempt to deallocate a pointer that is out of bounds\n";
			return;
		}

		BinHeader* header_ptr = (BinHeader*)((char*)user_ptr - sizeof(BinHeader));
		//是否非法越界写入
		uint64_t* canary_ptr = reinterpret_cast<uint64_t*>((char*)header_ptr + header_ptr->size);
		if (*canary_ptr != 0xDEADBEEFCAFEBABEULL)
		{
			std::cerr << "Memory corruption detected: canary value has been altered\n";
			return;
		}

		// 是否被重复释放等
		if (!header_ptr->is_used)
		{
			std::cerr << "Attempt to deallocate a pointer that has already been deallocated\n";
			return;
		}
		header_ptr->is_used = false;

		//合并相邻的空闲块
		header_ptr = header_ptr->merge();

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