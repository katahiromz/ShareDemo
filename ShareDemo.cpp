#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cassert>

typedef struct ITEM
{
    int id;
    DWORD pid;
} ITEM;

#define BLOCK_CAPACITY 3

typedef struct BLOCK
{
    int num_items;
    ITEM items[BLOCK_CAPACITY];
    HANDLE hNext;
    DWORD ref_pid;
} BLOCK;

/* shared data section */

#ifdef _MSC_VER
    #define SHELL32SHARE
#else
    #define SHELL32SHARE __attribute__((section(".shared"), shared))
#endif

#ifdef _MSC_VER
    #pragma data_seg(".shared")
#endif
static int s_num_items SHELL32SHARE = 0;
static int s_next_id SHELL32SHARE = 0;
static BLOCK s_first_block SHELL32SHARE = { 0 };
#ifdef _MSC_VER
    #pragma data_seg()
    #pragma comment(linker, "/SECTION:.shared,RWS")
#endif

typedef struct SHARE_CONTEXT
{
    HANDLE hShare;
    BLOCK *block;
    DWORD pid;
    int id;
    LPARAM lParam;
} SHARE_CONTEXT;

typedef bool (*EACH_BLOCK_PROC)(SHARE_CONTEXT *context, BLOCK *block);

BOOL IsProcessRunning(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, pid);
    if (hProcess)
        CloseHandle(hProcess);
    return hProcess != NULL;
}

BLOCK *DoLockBlock(HANDLE hShare, DWORD pid)
{
    if (!hShare || !pid || !IsProcessRunning(pid))
        return NULL;

    LPVOID pv = SHLockShared(hShare, pid);
    printf("lock: %p, %u, %p\n", hShare, pid, pv);
    return (BLOCK *)pv;
}

void DoUnlockBlock(LPVOID block)
{
    if (!block || (BLOCK *)block == &s_first_block)
        return;

    printf("unlock: %p\n", block);
    SHUnlockShared(block);
}

void DoFreeBlock(HANDLE hShare, DWORD pid)
{
    if (!hShare || !pid || !IsProcessRunning(pid))
        return;

    printf("free: %p, %u\n", hShare, pid);
    SHFreeShared(hShare, pid);
}

void DoEnumItems(SHARE_CONTEXT *context, EACH_BLOCK_PROC proc)
{
    BLOCK *block = context->block;
    HANDLE hNext = block->hNext;

    while ((*proc)(context, block))
    {
        block = DoLockBlock(hNext, block->ref_pid);
        if (!block)
            return;

        DoUnlockBlock(context->block);
        context->hShare = hNext;
        context->block = block;
        hNext = block->hNext;
    }
}

void DoFreeBlocks(HANDLE hShare, DWORD ref_pid)
{
    if (!hShare || !ref_pid)
        return;

    do
    {
        BLOCK *block = DoLockBlock(hShare, ref_pid);
        if (!block)
            break;

        HANDLE hNext = block->hNext;
        ref_pid = block->ref_pid;
        DoUnlockBlock(block);
        DoFreeBlock(hShare, ref_pid);
        hShare = hNext;
    } while (hShare);
}

bool CompactingCallback(SHARE_CONTEXT *context, BLOCK *block)
{
    ITEM *pItems = (ITEM *)context->lParam;
    for (int i = 0; i < BLOCK_CAPACITY; ++i)
    {
        ITEM *item = &block->items[i];
        if (!item->id)
            continue;

        pItems[context->id++] = *item;
    }
    return true;
}

void DoCompactBlocks(void)
{
    assert(s_num_items <= BLOCK_CAPACITY);

    ITEM items[BLOCK_CAPACITY] = { { 0 } };
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId(), 0, (LPARAM)&items };
    DoEnumItems(&context, CompactingCallback);
    DoUnlockBlock(context.block);

    assert(context.id == s_num_items);
    assert(sizeof(s_first_block.items) == sizeof(items));

    HANDLE hNext = s_first_block.hNext;
    CopyMemory(&s_first_block.items, &items, sizeof(s_first_block.items));
    s_first_block.num_items = context.id;
    s_first_block.hNext = NULL;

    DoFreeBlocks(hNext, s_first_block.ref_pid);
    s_first_block.ref_pid = 0;
}

bool AddItemCallback(SHARE_CONTEXT *context, BLOCK *block)
{
    for (int i = 0; i < BLOCK_CAPACITY; ++i)
    {
        ITEM *item = &block->items[i];
        if (item->id != 0)
            continue;

        block->num_items++;
        s_num_items++;

        int id = ++s_next_id;
        item->id = id;
        item->pid = context->pid;
        context->id = id;
        return false;
    }

    return true;
}

int AddItem(DWORD pid)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, pid };
    DoEnumItems(&context, AddItemCallback);
    BLOCK *block = context.block;

    int id = context.id;
    if (id == 0)
    {
        id = ++s_next_id;

        BLOCK new_block = { 1, { id, pid } };
        block->hNext = SHAllocShared(&new_block, sizeof(BLOCK), pid);
        block->ref_pid = pid;
        ++s_num_items;
    }

    DoUnlockBlock(block);
    return id;
}

bool RemoveByPidCallback(SHARE_CONTEXT *context, BLOCK *block)
{
    for (int i = 0; i < BLOCK_CAPACITY; ++i)
    {
        ITEM *item = &block->items[i];
        if (context->pid != item->pid || item->pid == 0)
            continue;

        block->num_items--;
        s_num_items--;
        item->id = 0;
        item->pid = 0;
    }
    return true;
}

bool DisplayCallback(SHARE_CONTEXT *context, BLOCK *block)
{
    printf("--- BLOCK %d ---\n", context->id);
    printf("num_items:%d, hNext:%p, ref_pid:%u\n", block->num_items, block->hNext, block->ref_pid);

    for (int i = 0; i < BLOCK_CAPACITY; ++i)
    {
        ITEM *item = &block->items[i];
        printf("id:%d, pid:%lu\n", item->id, item->pid);
    }

    context->id++;
    return true;
}

void DisplayBlocks(void)
{
    printf("s_num_items: %d\n", s_num_items);
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    DoEnumItems(&context, DisplayCallback);
    DoUnlockBlock(context.block);
}

void enter_key(void)
{
    printf("Press Enter key\n");
    fflush(stdout);
    getchar();
}

DWORD GetAnotherPid(DWORD pid)
{
    for (size_t i = 0; i < BLOCK_CAPACITY; ++i)
    {
        ITEM *item = &s_first_block.items[i];
        if (item->id != 0 && item->pid != pid && IsProcessRunning(item->pid))
        {
            return item->pid;
        }
    }
    return 0;
}

void MoveOwnership(DWORD pid)
{
    DWORD another_pid = GetAnotherPid(pid);
    if (another_pid == 0)
        return;

    BLOCK *block = &s_first_block;

    do
    {
        BLOCK *next_block = DoLockBlock(block->hNext, block->ref_pid);

        if (block->ref_pid == pid)
        {
            if (!next_block)
            {
                block->hNext = NULL;
                block->ref_pid = 0;
                break;
            }

            HANDLE hNewShare = SHAllocShared(next_block, sizeof(BLOCK), another_pid);
            block->hNext = hNewShare;
            block->ref_pid = another_pid;

            DoUnlockBlock(block);
            block = DoLockBlock(hNewShare, another_pid);
        }
        else
        {
            DoUnlockBlock(block);
            block = next_block;
        }
    } while (block);

    DoUnlockBlock(block);
}

void RemoveItemByPid(DWORD pid)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, pid };
    DoEnumItems(&context, RemoveByPidCallback);
    DoUnlockBlock(context.block);

    MoveOwnership(pid);

    if (s_num_items <= BLOCK_CAPACITY && s_first_block.hNext)
    {
        DoCompactBlocks();
    }
}

int main(void)
{
    DWORD pid = GetCurrentProcessId();
    printf("pid: %lu\n", pid);

    int id = AddItem(pid);
    printf("id %d added\n", id);
    DisplayBlocks();
    enter_key();

    RemoveItemByPid(pid);
    DisplayBlocks();
    enter_key();

    return 0;
}
