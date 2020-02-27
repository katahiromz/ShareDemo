#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
//#include <cassert>

#define assert(x) if (!(x)) MessageBoxA(NULL, #x, NULL, 0);

typedef struct ITEM
{
    int id;
    DWORD pid;
} ITEM;

#define BLOCK_CAPACITY 3

typedef struct BLOCK
{
    int num;
    HANDLE hNext;
    DWORD ref_pid;
    ITEM items[BLOCK_CAPACITY];
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
static int s_num SHELL32SHARE = 0;
static int s_next_id SHELL32SHARE = 0;
static BLOCK s_first_block SHELL32SHARE = { 0, NULL, 0 };
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

typedef bool (*EACH_ITEM_PROC)(SHARE_CONTEXT *context, int iBlock, BLOCK *block, ITEM *item);

BOOL IsProcessRunning(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, pid);
    if (hProcess)
        CloseHandle(hProcess);
    return hProcess != NULL;
}

BLOCK *DoLock(HANDLE hShare, DWORD pid)
{
    if (!hShare || !pid || !IsProcessRunning(pid))
        return NULL;

    LPVOID pv = SHLockShared(hShare, pid);
    printf("lock: %p, %u, %p\n", hShare, pid, pv);
    return (BLOCK *)pv;
}

void DoUnlock(LPVOID block)
{
    if (!block || (BLOCK *)block == &s_first_block)
        return;

    printf("unlock: %p\n", block);
    SHUnlockShared(block);
}

void DoFree(HANDLE hShare, DWORD pid)
{
    if (!hShare || !pid || !IsProcessRunning(pid))
        return;

    printf("free: %p, %u\n", hShare, pid);
    SHFreeShared(hShare, pid);
}

void DoEnumItems(SHARE_CONTEXT *context, EACH_ITEM_PROC proc)
{
    HANDLE hShare = context->hShare;
    BLOCK *block = context->block;
    HANDLE hNext = block->hNext;
    int iBlock = 0;

    for (;;)
    {
        for (int i = 0; i < BLOCK_CAPACITY; ++i)
        {
            if (!(*proc)(context, iBlock, block, &block->items[i]))
                return;
        }

        DWORD ref_pid = block->ref_pid;
        block = DoLock(hNext, ref_pid);
        if (!block)
            return;

        DoUnlock(context->block);

        hShare = hNext;
        context->hShare = hShare;
        context->block = block;
        hNext = block->hNext;

        ++iBlock;
    }
}

void DoFreeBlocks(HANDLE hShare, DWORD ref_pid)
{
    if (!hShare)
        return;

    do
    {
        BLOCK *block = DoLock(hShare, ref_pid);
        if (!block)
            break;

        HANDLE hNext = block->hNext;
        ref_pid = block->ref_pid;
        DoUnlock(block);
        DoFree(hShare, ref_pid);
        hShare = hNext;
    } while (hShare);
}

bool CompactingCallback(SHARE_CONTEXT *context, int iBlock, BLOCK *block, ITEM *item)
{
    if (!item->id)
        return true;

    INT num = context->id;
    ITEM *pItems = (ITEM *)context->lParam;
    pItems[num] = *item;
    context->id++;
    return true;
}

void DoCompactingBlocks()
{
    ITEM items[BLOCK_CAPACITY];
    ZeroMemory(&items, sizeof(items));

    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId(), 0, (LPARAM)&items };
    DoEnumItems(&context, CompactingCallback);
    DoUnlock(context.block);

    assert(context.id == s_num);
    assert(sizeof(s_first_block.items) == sizeof(items));

    HANDLE hNext = s_first_block.hNext;
    CopyMemory(&s_first_block.items, &items, sizeof(s_first_block.items));
    s_first_block.num = context.id;
    s_first_block.hNext = NULL;

    DoFreeBlocks(hNext, s_first_block.ref_pid);
    s_first_block.ref_pid = 0;
}

bool AddItemCallback(SHARE_CONTEXT *context, int iBlock, BLOCK *block, ITEM *item)
{
    if (item->id != 0)
        return true;

    block->num++;
    s_num++;

    ++s_next_id;
    int id = s_next_id;
    item->id = id;
    item->pid = context->pid;

    context->id = id;
    return false;
}

int AddItem(void)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    DoEnumItems(&context, AddItemCallback);

    BLOCK *block = context.block;
    int id = context.id;
    if (id == 0)
    {
        printf("BLOCK overflow\n");
        id = ++s_next_id;

        BLOCK new_block;
        ZeroMemory(&new_block, sizeof(new_block));
        new_block.num = 1;
        new_block.hNext = NULL;
        new_block.items[0].id = id;
        new_block.items[0].pid = GetCurrentProcessId();

        block->hNext = SHAllocShared(&new_block, sizeof(BLOCK), GetCurrentProcessId());
        block->ref_pid = GetCurrentProcessId();
        ++s_num;
    }

    DoUnlock(block);

    return id;
}

bool RemoveByPidCallback(SHARE_CONTEXT *context, int iBlock, BLOCK *block, ITEM *item)
{
    if (context->pid != item->pid || item->pid == 0)
        return true;

    block->num--;
    s_num--;
    item->id = 0;
    item->pid = 0;

    return true;
}

static int s_iBlock = -1;

bool DisplayCallback(SHARE_CONTEXT *context, int iBlock, BLOCK *block, ITEM *item)
{
    if (s_iBlock != iBlock)
    {
        s_iBlock = iBlock;
        printf("--- BLOCK %d ---\n", iBlock);
        printf("num:%d, hNext:%p, ref_pid:%u\n", block->num, block->hNext, block->ref_pid);
    }
    printf("id:%d, pid:%lu\n", item->id, item->pid);
    return true;
}

void DisplayBlocks()
{
    printf("s_num: %d\n", s_num);
    s_iBlock = -1;
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    DoEnumItems(&context, DisplayCallback);
    DoUnlock(context.block);
}

void enter_key()
{
    printf("Press Enter key\n");
    fflush(stdout);

    char buf[8];
    rewind(stdin);
    fgets(buf, 8, stdin);
}

void MoveOwnership(DWORD pid)
{
    DWORD another_pid = 0;

    for (size_t i = 0; i < BLOCK_CAPACITY; ++i)
    {
        if (s_first_block.items[i].id != 0 &&
            s_first_block.items[i].pid != pid &&
            IsProcessRunning(s_first_block.items[i].pid))
        {
            another_pid = s_first_block.items[i].pid;
            break;
        }
    }

    if (another_pid == 0)
        return;

    BLOCK *block = &s_first_block;

    while (block)
    {
        BLOCK *next_block = NULL;
        if (block->hNext)
        {
            next_block = DoLock(block->hNext, block->ref_pid);
        }

        DWORD ref_pid = block->ref_pid;
        if (ref_pid == pid)
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

            DoUnlock(block);

            block = DoLock(hNewShare, another_pid);
        }
        else
        {
            DoUnlock(block);

            block = next_block;
        }
    }

    DoUnlock(block);
}

void RemoveItemByPid(DWORD pid)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, pid };
    DoEnumItems(&context, RemoveByPidCallback);
    DoUnlock(context.block);

    MoveOwnership(pid);

    if (s_num < BLOCK_CAPACITY && s_first_block.hNext)
    {
        DoCompactingBlocks();
    }
}

int main(void)
{
    printf("pid: %lu\n", GetCurrentProcessId());
    int id = AddItem();
    printf("id %d added\n", id);

    DisplayBlocks();
    enter_key();

    RemoveItemByPid(GetCurrentProcessId());

    DisplayBlocks();
    enter_key();

    return 0;
}
