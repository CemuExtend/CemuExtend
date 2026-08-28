#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>

namespace MemMapper
{
	const size_t sPageSize{[]() {
		return (size_t)getpagesize();
	}()};

	size_t GetPageSize()
	{
		return sPageSize;
	}

	int GetProt(PAGE_PERMISSION permissionFlags)
	{
		int p = 0;
		if (permissionFlags == PAGE_PERMISSION::P_NONE)
			p = PROT_NONE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE | PROT_EXEC;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE))
			p = PROT_READ | (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE) ? PROT_EXEC : 0);
		else
			cemu_assert_unimplemented();
		return p;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		return mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		munmap(baseAddr, size);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		void* r;
		if (fromReservation)
		{
			uint64 page_size = sysconf(_SC_PAGESIZE);
			void* page = baseAddr;
			if ((uint64)baseAddr % page_size != 0)
				page = (void*)((uint64)baseAddr & ~(page_size - 1));
			if (mprotect(page, size, GetProt(permissionFlags)) == 0)
				r = baseAddr;
			else
				r = nullptr;
		}
		else
			r = mmap(baseAddr, size, GetProt(permissionFlags), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		return r;
	}

	bool FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (fromReservation)
		{
			// mprotect(PROT_NONE) only makes the committed pages inaccessible; it
			// does not discard their contents. Re-enabling such a range therefore
			// exposed data from the previous title on Unix, unlike MEM_DECOMMIT on
			// Windows. Replace the range with fresh inaccessible anonymous pages so
			// the address-space reservation remains intact and the next commit is
			// zero-initialized.
			void* result = mmap(baseAddr, size, PROT_NONE,
							MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			return result == baseAddr;
		}
		return munmap(baseAddr, size) == 0;
	}

	bool SetMemoryPermission(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		if (!baseAddr || size == 0)
			return false;
		const auto pageSize = GetPageSize();
		auto address = reinterpret_cast<uintptr_t>(baseAddr);
		auto page = address & ~(pageSize - 1);
		auto end = (address + size + pageSize - 1) & ~(pageSize - 1);
		return mprotect(reinterpret_cast<void*>(page), end - page, GetProt(permissionFlags)) == 0;
	}

}; // namespace MemMapper
