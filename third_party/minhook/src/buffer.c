/*
 *  MinHook - The Minimalistic API Hooking Library for x64/x86
 *  Copyright (C) 2009-2017 Tsuda Kageyu.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
 *  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <windows.h>
#include "buffer.h"

// Size of each memory block. (= page size of VirtualAlloc)
#define MEMORY_BLOCK_SIZE 0x1000

// Max range for seeking a memory block. (= 1024MB)
#define MAX_MEMORY_RANGE 0x40000000

// Memory protection flags to check the executable address.
#define PAGE_EXECUTE_FLAGS \
    (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

// Memory slot.
typedef struct _MEMORY_SLOT
{
    union
    {
        struct _MEMORY_SLOT *pNext;
        UINT8 buffer[MEMORY_SLOT_SIZE];
    };
} MEMORY_SLOT, *PMEMORY_SLOT;

// Memory block info. Placed at the head of each block.
typedef struct _MEMORY_BLOCK
{
    struct _MEMORY_BLOCK *pNext;
    PMEMORY_SLOT pFree;         // First element of the free slot list.
    UINT usedCount;
} MEMORY_BLOCK, *PMEMORY_BLOCK;

//-------------------------------------------------------------------------
// Global Variables:
//-------------------------------------------------------------------------

// First element of the memory block list.
PMEMORY_BLOCK g_pMemoryBlocks;

//-------------------------------------------------------------------------
VOID InitializeBuffer(VOID)
{
    // Nothing to do for now.
}

//-------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
static BOOL ReleaseImageSlots(VOID);
static BOOL SkipNearImageAllocation(LPVOID origin);
#endif

BOOL UninitializeBuffer(VOID)
{
    PMEMORY_BLOCK pBlock = g_pMemoryBlocks;
#if defined(_M_X64) || defined(__x86_64__)
    if (!ReleaseImageSlots())
        return FALSE;
#endif
    g_pMemoryBlocks = NULL;

    while (pBlock)
    {
        PMEMORY_BLOCK pNext = pBlock->pNext;
        VirtualFree(pBlock, 0, MEM_RELEASE);
        pBlock = pNext;
    }
    return TRUE;
}

//-------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
static LPVOID FindPrevFreeRegion(LPVOID pAddress, LPVOID pMinAddr, DWORD dwAllocationGranularity)
{
    ULONG_PTR tryAddr = (ULONG_PTR)pAddress;

    // Round down to the allocation granularity.
    tryAddr -= tryAddr % dwAllocationGranularity;

    // Start from the previous allocation granularity multiply.
    tryAddr -= dwAllocationGranularity;

    while (tryAddr >= (ULONG_PTR)pMinAddr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPVOID)tryAddr, &mbi, sizeof(mbi)) == 0)
            break;

        if (mbi.State == MEM_FREE)
            return (LPVOID)tryAddr;

        if ((ULONG_PTR)mbi.AllocationBase < dwAllocationGranularity)
            break;

        tryAddr = (ULONG_PTR)mbi.AllocationBase - dwAllocationGranularity;
    }

    return NULL;
}
#endif

//-------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
static LPVOID FindNextFreeRegion(LPVOID pAddress, LPVOID pMaxAddr, DWORD dwAllocationGranularity)
{
    ULONG_PTR tryAddr = (ULONG_PTR)pAddress;

    // Round down to the allocation granularity.
    tryAddr -= tryAddr % dwAllocationGranularity;

    // Start from the next allocation granularity multiply.
    tryAddr += dwAllocationGranularity;

    while (tryAddr <= (ULONG_PTR)pMaxAddr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPVOID)tryAddr, &mbi, sizeof(mbi)) == 0)
            break;

        if (mbi.State == MEM_FREE)
            return (LPVOID)tryAddr;

        tryAddr = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;

        // Round up to the next allocation granularity.
        tryAddr += dwAllocationGranularity - 1;
        tryAddr -= tryAddr % dwAllocationGranularity;
    }

    return NULL;
}
#endif

//-------------------------------------------------------------------------
static PMEMORY_BLOCK GetMemoryBlock(LPVOID pOrigin)
{
    PMEMORY_BLOCK pBlock;
#if defined(_M_X64) || defined(__x86_64__)
    ULONG_PTR minAddr;
    ULONG_PTR maxAddr;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    minAddr = (ULONG_PTR)si.lpMinimumApplicationAddress;
    maxAddr = (ULONG_PTR)si.lpMaximumApplicationAddress;

    // pOrigin ± 512MB
    if ((ULONG_PTR)pOrigin > MAX_MEMORY_RANGE && minAddr < (ULONG_PTR)pOrigin - MAX_MEMORY_RANGE)
        minAddr = (ULONG_PTR)pOrigin - MAX_MEMORY_RANGE;

    if (maxAddr > (ULONG_PTR)pOrigin + MAX_MEMORY_RANGE)
        maxAddr = (ULONG_PTR)pOrigin + MAX_MEMORY_RANGE;

    // Make room for MEMORY_BLOCK_SIZE bytes.
    maxAddr -= MEMORY_BLOCK_SIZE - 1;
#endif

    // Look the registered blocks for a reachable one.
    for (pBlock = g_pMemoryBlocks; pBlock != NULL; pBlock = pBlock->pNext)
    {
#if defined(_M_X64) || defined(__x86_64__)
        // Ignore the blocks too far.
        if ((ULONG_PTR)pBlock < minAddr || (ULONG_PTR)pBlock >= maxAddr)
            continue;
#endif
        // The block has at least one unused slot.
        if (pBlock->pFree != NULL)
            return pBlock;
    }

#if defined(_M_X64) || defined(__x86_64__)
    if (SkipNearImageAllocation(pOrigin))
        return NULL;

    // Alloc a new block above if not found.
    {
        LPVOID pAlloc = pOrigin;
        while ((ULONG_PTR)pAlloc >= minAddr)
        {
            pAlloc = FindPrevFreeRegion(pAlloc, (LPVOID)minAddr, si.dwAllocationGranularity);
            if (pAlloc == NULL)
                break;

            pBlock = (PMEMORY_BLOCK)VirtualAlloc(
                pAlloc, MEMORY_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (pBlock != NULL)
                break;
        }
    }

    // Alloc a new block below if not found.
    if (pBlock == NULL)
    {
        LPVOID pAlloc = pOrigin;
        while ((ULONG_PTR)pAlloc <= maxAddr)
        {
            pAlloc = FindNextFreeRegion(pAlloc, (LPVOID)maxAddr, si.dwAllocationGranularity);
            if (pAlloc == NULL)
                break;

            pBlock = (PMEMORY_BLOCK)VirtualAlloc(
                pAlloc, MEMORY_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (pBlock != NULL)
                break;
        }
    }
#else
    // In x86 mode, a memory block can be placed anywhere.
    pBlock = (PMEMORY_BLOCK)VirtualAlloc(
        NULL, MEMORY_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#endif

    if (pBlock != NULL)
    {
        // Build a linked list of all the slots.
        PMEMORY_SLOT pSlot = (PMEMORY_SLOT)pBlock + 1;
        pBlock->pFree = NULL;
        pBlock->usedCount = 0;
        do
        {
            pSlot->pNext = pBlock->pFree;
            pBlock->pFree = pSlot;
            pSlot++;
        } while ((ULONG_PTR)pSlot - (ULONG_PTR)pBlock <= MEMORY_BLOCK_SIZE - MEMORY_SLOT_SIZE);

        pBlock->pNext = g_pMemoryBlocks;
        g_pMemoryBlocks = pBlock;
    }

    return pBlock;
}

//-------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
// Opt-in storage for verified code in the process executable. The executable
// stays mapped for the process lifetime. No removable DLL is eligible.
#define MAX_IMAGE_SLOTS (MEMORY_BLOCK_SIZE / MEMORY_SLOT_SIZE)
typedef struct _IMAGE_SLOT
{
    BOOL used;
    UINT8 contents[MEMORY_SLOT_SIZE];
} IMAGE_SLOT;

static ULONG_PTR g_imageCodeStart, g_imageCodeEnd, g_imageSlotStart;
static UINT g_imageSlotCount;
static BOOL g_imageNearFailed, g_imageRestorePending;
static IMAGE_SLOT g_imageSlots[MAX_IMAGE_SLOTS];

static BOOL QueryImageCodePage(ULONG_PTR address, MEMORY_BASIC_INFORMATION *memory)
{
    return VirtualQuery((LPCVOID)address, memory, sizeof(*memory))
        && memory->State == MEM_COMMIT && memory->Type == MEM_IMAGE
        && memory->AllocationBase == GetModuleHandleW(NULL);
}

BOOL EnableMainImageFallback(VOID)
{
    ULONG_PTR base = (ULONG_PTR)GetModuleHandleW(NULL);
    MEMORY_BASIC_INFORMATION memory;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sections;
    UINT i;

    if (g_imageSlotCount)
        return TRUE;

    if (!base || !QueryImageCodePage(base, &memory) ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        memory.RegionSize < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64))
        return FALSE;

    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < (LONG)sizeof(*dos) ||
        (SIZE_T)dos->e_lfanew > memory.RegionSize - sizeof(*nt))
        return FALSE;

    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(nt->OptionalHeader) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SectionAlignment < MEMORY_BLOCK_SIZE ||
        nt->OptionalHeader.SizeOfImage > MAX_MEMORY_RANGE ||
        nt->FileHeader.NumberOfSections >
            (memory.RegionSize - (SIZE_T)dos->e_lfanew - sizeof(*nt)) / sizeof(*sections))
        return FALSE;

    sections = (const IMAGE_SECTION_HEADER *)(nt + 1);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const IMAGE_SECTION_HEADER *section = &sections[i];
        const DWORD codeFlags = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        DWORD body, first, end, j;
        if (memcmp(section->Name, ".text\0\0", IMAGE_SIZEOF_SHORT_NAME) ||
            (section->Characteristics & (codeFlags | IMAGE_SCN_MEM_WRITE)) != codeFlags)
            continue;

        body = section->Misc.VirtualSize > section->SizeOfRawData ?
            section->Misc.VirtualSize : section->SizeOfRawData;
        if (!body || section->VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            body > nt->OptionalHeader.SizeOfImage - section->VirtualAddress)
            return FALSE;
        body += section->VirtualAddress;
        first = (body + MEMORY_SLOT_SIZE - 1) & ~(MEMORY_SLOT_SIZE - 1);
        end = (body + MEMORY_BLOCK_SIZE - 1) & ~(MEMORY_BLOCK_SIZE - 1);
        if (end > nt->OptionalHeader.SizeOfImage || first >= end)
            return FALSE;

        for (j = 0; j < nt->FileHeader.NumberOfSections; ++j)
        {
            if (j != i && sections[j].VirtualAddress >= section->VirtualAddress &&
                sections[j].VirtualAddress < end)
                return FALSE;
        }

        g_imageCodeStart = base + section->VirtualAddress;
        g_imageCodeEnd = base + body;
        g_imageSlotStart = base + first;
        g_imageSlotCount = (end - first) / MEMORY_SLOT_SIZE;
        return TRUE;
    }
    return FALSE;
}

static BOOL IsImageCode(ULONG_PTR address)
{
    return g_imageSlotCount && address >= g_imageCodeStart && address < g_imageCodeEnd;
}

DWORD HookProtectionAfterWrite(LPVOID patch, SIZE_T size, DWORD protection)
{
    ULONG_PTR address = (ULONG_PTR)patch;
    if (IsImageCode(address) && size <= g_imageCodeEnd - address &&
        (address & (MEMORY_BLOCK_SIZE - 1)) + size <= MEMORY_BLOCK_SIZE &&
        protection == PAGE_EXECUTE_WRITECOPY)
        return PAGE_EXECUTE_READ;
    return protection;
}

static IMAGE_SLOT *ImageSlotAt(ULONG_PTR address)
{
    ULONG_PTR offset;
    if (!g_imageSlotCount || address < g_imageSlotStart)
        return NULL;
    offset = address - g_imageSlotStart;
    if (offset % MEMORY_SLOT_SIZE || offset / MEMORY_SLOT_SIZE >= g_imageSlotCount)
        return NULL;
    return &g_imageSlots[offset / MEMORY_SLOT_SIZE];
}

static BOOL ImageSlotMatches(ULONG_PTR address, const IMAGE_SLOT *slot)
{
    MEMORY_BASIC_INFORMATION memory;
    UINT8 current[MEMORY_SLOT_SIZE];
    SIZE_T read;
    if (!QueryImageCodePage(address, &memory) ||
        (memory.Protect != PAGE_EXECUTE_READ && memory.Protect != PAGE_EXECUTE_WRITECOPY))
        return FALSE;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, current, sizeof(current), &read)
        && read == sizeof(current) && !memcmp(current, slot->contents, sizeof(current));
}

static BOOL RestoreImageSlotProtection(VOID)
{
    DWORD ignored;
    if (g_imageRestorePending)
    {
        MEMORY_BASIC_INFORMATION memory;
        if (!QueryImageCodePage(g_imageSlotStart, &memory))
            return FALSE;
        if (memory.Protect != PAGE_EXECUTE_READ)
        {
            if (memory.Protect != PAGE_EXECUTE_READWRITE && memory.Protect != PAGE_EXECUTE_WRITECOPY)
                return FALSE;
            if (!VirtualProtect((LPVOID)g_imageSlotStart, 1, PAGE_EXECUTE_READ, &ignored) ||
                !QueryImageCodePage(g_imageSlotStart, &memory) || memory.Protect != PAGE_EXECUTE_READ)
                return FALSE;
        }
        g_imageRestorePending = FALSE;
    }

    return TRUE;
}

static BOOL WriteImageSlot(ULONG_PTR address, IMAGE_SLOT *slot, const UINT8 *contents)
{
    DWORD ignored;
    BOOL flushed;
    if (!slot->used || !RestoreImageSlotProtection() || !ImageSlotMatches(address, slot) ||
        !VirtualProtect((LPVOID)address, MEMORY_SLOT_SIZE, PAGE_EXECUTE_READWRITE, &ignored))
        return FALSE;

    g_imageRestorePending = TRUE;
    memcpy((LPVOID)address, contents, MEMORY_SLOT_SIZE);
    memcpy(slot->contents, contents, MEMORY_SLOT_SIZE);
    flushed = FlushInstructionCache(GetCurrentProcess(), (LPVOID)address, MEMORY_SLOT_SIZE);

    // Restore the verified read/execute page before exposing the trampoline.
    // Retain failed protection work for the release or uninitialize retry.
    if (!RestoreImageSlotProtection())
        return FALSE;
    return flushed;
}

static LPVOID AllocateImageSlot(LPVOID origin)
{
    UINT i;
    if (!IsImageCode((ULONG_PTR)origin) || !RestoreImageSlotProtection())
        return NULL;

    g_imageNearFailed = TRUE;
    for (i = 0; i < g_imageSlotCount; ++i)
    {
        ULONG_PTR address = g_imageSlotStart + i * MEMORY_SLOT_SIZE;
        IMAGE_SLOT *slot = &g_imageSlots[i];
        if (!slot->used && ImageSlotMatches(address, slot))
        {
            slot->used = TRUE;
            return (LPVOID)address;
        }
    }

    return NULL;
}

static BOOL FreeImageSlot(ULONG_PTR address, IMAGE_SLOT *slot)
{
    static const UINT8 zero[MEMORY_SLOT_SIZE];
    if (!slot->used)
        return TRUE;
    if (!RestoreImageSlotProtection() ||
        (memcmp(slot->contents, zero, sizeof(zero)) && !WriteImageSlot(address, slot, zero)))
        return FALSE;
    slot->used = FALSE;
    return TRUE;
}

static BOOL ReleaseImageSlots(VOID)
{
    UINT i;
    if (!RestoreImageSlotProtection())
        return FALSE;
    for (i = 0; i < g_imageSlotCount; ++i)
    {
        if (!FreeImageSlot(g_imageSlotStart + i * MEMORY_SLOT_SIZE, &g_imageSlots[i]))
            return FALSE;
    }
    g_imageCodeStart = g_imageCodeEnd = g_imageSlotStart = 0;
    g_imageSlotCount = 0;
    g_imageNearFailed = FALSE;
    return TRUE;
}

static BOOL SkipNearImageAllocation(LPVOID origin)
{
    return g_imageNearFailed && IsImageCode((ULONG_PTR)origin);
}

UINT GetCaveSlotsInUse(VOID)
{
    UINT i, used = 0;
    for (i = 0; i < g_imageSlotCount; ++i)
    {
        if (g_imageSlots[i].used)
            used++;
    }
    return used;
}
#else
BOOL EnableMainImageFallback(VOID) { return FALSE; }
UINT GetCaveSlotsInUse(VOID) { return 0; }
DWORD HookProtectionAfterWrite(LPVOID patch, SIZE_T size, DWORD protection)
{
    (void)patch;
    (void)size;
    return protection;
}
#endif

BOOL CommitBuffer(LPVOID buffer, const UINT8 *contents)
{
#if defined(_M_X64) || defined(__x86_64__)
    IMAGE_SLOT *slot = ImageSlotAt((ULONG_PTR)buffer);
    if (slot)
        return WriteImageSlot((ULONG_PTR)buffer, slot, contents);
#endif
    memcpy(buffer, contents, MEMORY_SLOT_SIZE);
    return TRUE;
}

//-------------------------------------------------------------------------
LPVOID AllocateBuffer(LPVOID pOrigin)
{
    PMEMORY_SLOT  pSlot;
    PMEMORY_BLOCK pBlock;
    pBlock = GetMemoryBlock(pOrigin);
    if (pBlock == NULL)
    {
#if defined(_M_X64) || defined(__x86_64__)
        return AllocateImageSlot(pOrigin);
#else
        return NULL;
#endif
    }

    // Remove an unused slot from the list.
    pSlot = pBlock->pFree;
    pBlock->pFree = pSlot->pNext;
    pBlock->usedCount++;
#ifdef _DEBUG
    // Fill the slot with INT3 for debugging.
    memset(pSlot, 0xCC, sizeof(MEMORY_SLOT));
#endif
    return pSlot;
}

//-------------------------------------------------------------------------
BOOL FreeBuffer(LPVOID pBuffer)
{
    PMEMORY_BLOCK pBlock = g_pMemoryBlocks;
    PMEMORY_BLOCK pPrev = NULL;
    ULONG_PTR pTargetBlock = ((ULONG_PTR)pBuffer / MEMORY_BLOCK_SIZE) * MEMORY_BLOCK_SIZE;

#if defined(_M_X64) || defined(__x86_64__)
    IMAGE_SLOT *slot = ImageSlotAt((ULONG_PTR)pBuffer);
    if (slot)
        return FreeImageSlot((ULONG_PTR)pBuffer, slot);
#endif

    while (pBlock != NULL)
    {
        if ((ULONG_PTR)pBlock == pTargetBlock)
        {
            PMEMORY_SLOT pSlot = (PMEMORY_SLOT)pBuffer;
#ifdef _DEBUG
            // Clear the released slot for debugging.
            memset(pSlot, 0x00, sizeof(MEMORY_SLOT));
#endif
            // Restore the released slot to the list.
            pSlot->pNext = pBlock->pFree;
            pBlock->pFree = pSlot;
            pBlock->usedCount--;

            // Free if unused.
            if (pBlock->usedCount == 0)
            {
                if (pPrev)
                    pPrev->pNext = pBlock->pNext;
                else
                    g_pMemoryBlocks = pBlock->pNext;

                VirtualFree(pBlock, 0, MEM_RELEASE);
            }

            break;
        }

        pPrev = pBlock;
        pBlock = pBlock->pNext;
    }
    return TRUE;
}

//-------------------------------------------------------------------------
BOOL IsExecutableAddress(LPVOID pAddress)
{
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery(pAddress, &mi, sizeof(mi));

    return (mi.State == MEM_COMMIT && (mi.Protect & PAGE_EXECUTE_FLAGS));
}
