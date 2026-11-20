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
VOID UninitializeBuffer(VOID)
{
    PMEMORY_BLOCK pBlock = g_pMemoryBlocks;
    g_pMemoryBlocks = NULL;

    while (pBlock)
    {
        PMEMORY_BLOCK pNext = pBlock->pNext;
        VirtualFree(pBlock, 0, MEM_RELEASE);
        pBlock = pNext;
    }
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
//-------------------------------------------------------------------------
// EDUP-NHOE: section-padding fallback.
//
// Some Wine/FEX environments (CrossOver ARM64 bottles) report the address
// space around the game image as MEM_FREE but reject every VirtualAlloc at a
// fixed address there (ERROR_INVALID_ADDRESS), so no relay/trampoline block
// can be placed within rel32 reach. When that happens, take slots from the
// zero padding between a section's VirtualSize and the end of its last page
// in the module that contains the target. That padding is mapped as part of
// the image, is never used by the loader or the program, and is always within
// rel32 reach of the module's own code. Executable sections are used first.
// Each slot is only taken while all of its bytes are still zero.

#define MAX_CAVE_SLOTS 64

static ULONG_PTR g_caveSlots[MAX_CAVE_SLOTS];
// Module whose neighbourhood already failed near allocation; later targets in
// it skip the (slow, futile) +/-1 GB VirtualAlloc scan.
static ULONG_PTR g_nearFailedBase, g_nearFailedEnd;
static BOOL      g_caveUsed[MAX_CAVE_SLOTS];
static UINT      g_caveCount;

static BOOL IsZeroSlot(ULONG_PTR address)
{
    const UINT8 *p = (const UINT8 *)address;
    UINT i;
    for (i = 0; i < MEMORY_SLOT_SIZE; ++i)
    {
        if (p[i] != 0)
            return FALSE;
    }
    return TRUE;
}

static BOOL IsKnownCaveSlot(ULONG_PTR address)
{
    UINT i;
    for (i = 0; i < g_caveCount; ++i)
    {
        if (g_caveSlots[i] == address)
            return TRUE;
    }
    return FALSE;
}

static VOID AddCaveSlots(ULONG_PTR base, BOOL wantExecutable)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER section;
    UINT i;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    section = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        BOOL executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        ULONG_PTR end, pageEnd, slot;

        if (executable != wantExecutable || section->Misc.VirtualSize == 0)
            continue;

        end     = base + section->VirtualAddress + section->Misc.VirtualSize;
        pageEnd = (end + 0xFFF) & ~(ULONG_PTR)0xFFF;
        slot    = (end + 15) & ~(ULONG_PTR)15;

        for (; slot + MEMORY_SLOT_SIZE <= pageEnd && g_caveCount < MAX_CAVE_SLOTS; slot += MEMORY_SLOT_SIZE)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (IsKnownCaveSlot(slot))
                continue;
            if (VirtualQuery((LPVOID)slot, &mbi, sizeof(mbi)) == 0
                || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE
                || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
                continue;
            if (!IsZeroSlot(slot))
                continue;
            g_caveSlots[g_caveCount] = slot;
            g_caveUsed[g_caveCount]  = FALSE;
            g_caveCount++;
        }
    }
}

static BOOL ModuleRange(LPVOID pOrigin, ULONG_PTR *base, ULONG_PTR *end)
{
    HMODULE module = NULL;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)pOrigin, &module) || module == NULL)
        return FALSE;
    dos = (PIMAGE_DOS_HEADER)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    nt = (PIMAGE_NT_HEADERS)((ULONG_PTR)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return FALSE;
    *base = (ULONG_PTR)module;
    *end  = (ULONG_PTR)module + nt->OptionalHeader.SizeOfImage;
    return TRUE;
}

static BOOL NearAllocationKnownToFail(LPVOID pOrigin)
{
    return g_nearFailedBase != 0
        && (ULONG_PTR)pOrigin >= g_nearFailedBase && (ULONG_PTR)pOrigin < g_nearFailedEnd;
}

static LPVOID AllocateCaveSlot(LPVOID pOrigin)
{
    ULONG_PTR base = 0, end = 0;
    UINT pass, i;

    for (pass = 0; pass < 2; ++pass)
    {
        for (i = 0; i < g_caveCount; ++i)
        {
            ULONG_PTR slot = g_caveSlots[i];
            ULONG_PTR distance = slot > (ULONG_PTR)pOrigin ? slot - (ULONG_PTR)pOrigin : (ULONG_PTR)pOrigin - slot;
            DWORD oldProtect;

            if (g_caveUsed[i] || distance >= (ULONG_PTR)MAX_MEMORY_RANGE * 2 - MEMORY_SLOT_SIZE)
                continue;
            if (!IsZeroSlot(slot))
                continue;
            if (!VirtualProtect((LPVOID)slot, MEMORY_SLOT_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect))
                continue;
            g_caveUsed[i] = TRUE;
            return (LPVOID)slot;
        }

        if (pass == 0)
        {
            if (!ModuleRange(pOrigin, &base, &end))
                return NULL;
            g_nearFailedBase = base;
            g_nearFailedEnd  = end;
            AddCaveSlots(base, TRUE);
            AddCaveSlots(base, FALSE);
        }
    }
    return NULL;
}

static BOOL FreeCaveSlot(LPVOID pBuffer)
{
    UINT i;
    for (i = 0; i < g_caveCount; ++i)
    {
        if (g_caveSlots[i] == (ULONG_PTR)pBuffer)
        {
            memset(pBuffer, 0, MEMORY_SLOT_SIZE);
            FlushInstructionCache(GetCurrentProcess(), pBuffer, MEMORY_SLOT_SIZE);
            g_caveUsed[i] = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

UINT GetCaveSlotsInUse(VOID)
{
    UINT i, used = 0;
    for (i = 0; i < g_caveCount; ++i)
    {
        if (g_caveUsed[i])
            used++;
    }
    return used;
}
#else
UINT GetCaveSlotsInUse(VOID)
{
    return 0;
}
#endif

//-------------------------------------------------------------------------
LPVOID AllocateBuffer(LPVOID pOrigin)
{
    PMEMORY_SLOT  pSlot;
    PMEMORY_BLOCK pBlock;
#if defined(_M_X64) || defined(__x86_64__)
    if (NearAllocationKnownToFail(pOrigin))
        return AllocateCaveSlot(pOrigin);
#endif
    pBlock = GetMemoryBlock(pOrigin);
    if (pBlock == NULL)
    {
#if defined(_M_X64) || defined(__x86_64__)
        return AllocateCaveSlot(pOrigin);
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
VOID FreeBuffer(LPVOID pBuffer)
{
    PMEMORY_BLOCK pBlock = g_pMemoryBlocks;
    PMEMORY_BLOCK pPrev = NULL;
    ULONG_PTR pTargetBlock = ((ULONG_PTR)pBuffer / MEMORY_BLOCK_SIZE) * MEMORY_BLOCK_SIZE;

#if defined(_M_X64) || defined(__x86_64__)
    if (FreeCaveSlot(pBuffer))
        return;
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
}

//-------------------------------------------------------------------------
BOOL IsExecutableAddress(LPVOID pAddress)
{
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery(pAddress, &mi, sizeof(mi));

    return (mi.State == MEM_COMMIT && (mi.Protect & PAGE_EXECUTE_FLAGS));
}
