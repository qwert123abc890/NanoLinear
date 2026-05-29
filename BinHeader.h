#pragma once
#include<iostream>
#include<cstddef>
#include<cstdint>
//sizeof(BinHeader) = 32;
struct BinHeader
{
	BinHeader* prev;
	BinHeader* next;
	size_t size;
	bool is_used;
	BinHeader() : prev(this), next(this), size(0), is_used(false) {}
	BinHeader(size_t s) : prev(this), next(this), size(s), is_used(false) {}
	void* user_ptr()
	{
		return (void*)(this + 1);
	}
	void* canary_ptr()
	{
		return (void*)((char*)(this) + size);
	}
	BinHeader* find_best_fit(size_t need)
	{
		// assume `this` is the sentinel head of a circular list
		BinHeader* start = this;
		BinHeader* current = start->next;
		BinHeader* best_fit = nullptr;
		while (current != start)
		{
			if (!current->is_used && current->size >= need)
			{
				if (!best_fit || current->size < best_fit->size)
				{
					best_fit = current;
				}
			}
			current = current->next;
		}
		return best_fit;
	}
    BinHeader* split(size_t needed_size) //needed_size includes the size of the header
	{
		if (this->size <= needed_size)
		{
			std::cerr << "Not enough space to split\n";
			return nullptr;
		}
		
		if (this->size - needed_size <= 32)
		{
			return nullptr;
		}
		BinHeader* remaining_block_ptr = (BinHeader*)((char*)this + needed_size);
		remaining_block_ptr->size = this->size - needed_size;
		remaining_block_ptr->is_used = false;
		
		remaining_block_ptr->prev = remaining_block_ptr;
		remaining_block_ptr->next = remaining_block_ptr;

        size_t* rem_tail_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(remaining_block_ptr) + remaining_block_ptr->size - sizeof(size_t));
		*rem_tail_size_ptr = remaining_block_ptr->size;

		this->size = needed_size;
		size_t* this_tail_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(this) + needed_size - sizeof(size_t));
		*this_tail_size_ptr = needed_size;

		return remaining_block_ptr;
	}
	BinHeader* merge(char* current_top)
	{
		BinHeader* merged_block = this;
		BinHeader* next_block = reinterpret_cast<BinHeader*>(reinterpret_cast<char*>(this) + this->size);
		if ((char*)next_block == current_top)
		{
			[[unlikely]];
		}
		else if (!next_block->is_used)
		{
			//if (next_block->prev) next_block->prev->next = next_block->next;
			//if (next_block->next) next_block->next->prev = next_block->prev;
			next_block->prev->next = next_block->next;
			next_block->next->prev = next_block->prev;
		}

		size_t* prev_size = reinterpret_cast<size_t*>((char*)(this) - sizeof(size_t));
		BinHeader* prev_block = reinterpret_cast<BinHeader*>(reinterpret_cast<char*>(this) - *prev_size);
		if (!prev_block->is_used)
		{
			prev_block->prev->next = prev_block->next;
			prev_block->next->prev = prev_block->prev;

			//if(prev_block->prev)   prev_block->prev->next = prev_block->next;
			//if(prev_block->next)   prev_block->next->prev = prev_block->prev;

			prev_block->size += this->size;
			//size_t* this_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(this) + prev_block->size);
			size_t* this_size_ptr = reinterpret_cast<size_t*>(reinterpret_cast<char*>(prev_block) + prev_block->size - 8);
			*this_size_ptr = prev_block->size;

			merged_block = prev_block;
		}

		return merged_block;
	}
	void remove_from_list()
	{
		//if (this->prev) this->prev->next = this->next;
		//if (this->next) this->next->prev = this->prev;
		this->prev->next = this->next;
		this->next->prev = this->prev;

		//this->prev = nullptr;
		//this->next = nullptr;
		this->is_used = true;
	}

};
