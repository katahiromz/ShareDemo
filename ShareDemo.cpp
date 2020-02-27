#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
//#include <cassert>

#define assert(x) if (!(x)) MessageBoxA(NULL, #x, NULL, 0);

#ifdef _MSC_VER
    #define SHELL32SHARE
#else
    #define SHELL32SHARE __attribute__((section(".shared"), shared))
#endif

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

typedef bool (*EACH_ITEM_PROC)(SHARE_CONTEXT *context, BLOCK *block, ITEM *item);

BOOL IsProcessRunning(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, pid);
    if (hProcess)
        CloseHandle(hProcess);
    return hProcess != NULL;
}

LPVOID DoLock(HANDLE hShare, DWORD pid)
{
    if (!IsProcessRunning(pid))
    {
        return NULL;
    }

    LPVOID pv = SHLockShared(hShare, pid);
    printf("lock: %p, %u, %p\n", hShare, pid, pv);
    return pv;
}

void DoUnlock(LPVOID block)
{
    printf("unlock: %p\n", block);
    SHUnlockShared(block);
}

void DoFree(HANDLE hShare, DWORD pid)
{
    if (!IsProcessRunning(pid))
    {
        return;
    }

    printf("free: %p, %u\n", hShare, pid);
    SHFreeShared(hShare, pid);
}

void FindRoom(SHARE_CONTEXT *context, INT id, EACH_ITEM_PROC proc)
{
    HANDLE hShare = context->hShare;
    BLOCK *block = context->block;
    HANDLE hNext = block->hNext;
    DWORD ref_pid = block->ref_pid;

    for (;;)
    {
        for (int i = 0; i < BLOCK_CAPACITY; ++i)
        {
            if (!(*proc)(context, block, &block->items[i]))
                return;
        }

        if (!hNext)
            break;

        ref_pid = block->ref_pid;
        if (!IsProcessRunning(ref_pid))
            break;
        block = (BLOCK *)DoLock(hNext, ref_pid);

        if (hShare && context->block)
            DoUnlock(context->block);

        if (!block)
            return;

        hShare = hNext;
        context->hShare = hShare;
        context->block = block;
        hNext = block->hNext;
    }
}

bool AddItemCallback(SHARE_CONTEXT *context, BLOCK *block, ITEM *item)
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

bool RemoveByPidCallback(SHARE_CONTEXT *context, BLOCK *block, ITEM *item)
{
    if (context->pid != item->pid || item->pid == 0)
        return true;

    block->num--;
    s_num--;
    item->id = 0;
    item->pid = 0;

    return true;
}

bool CompactingCallback(SHARE_CONTEXT *context, BLOCK *block, ITEM *item)
{
    if (!item->id)
        return true;

    INT num = context->id;
    ITEM *pItems = (ITEM *)context->lParam;
    pItems[num] = *item;
    context->id++;
    return true;
}

void DoFreeBlocks(HANDLE hShare, DWORD ref_pid)
{
    if (!hShare)
        return;

    do
    {
        BLOCK *block = (BLOCK *)DoLock(hShare, ref_pid);
        if (!block)
            break;
        HANDLE hNext = block->hNext;
        ref_pid = block->ref_pid;
        DoUnlock(block);
        DoFree(hShare, ref_pid);
        hShare = hNext;
    } while (hShare);
}

void DoCompactingBlocks()
{
    ITEM items[BLOCK_CAPACITY];
    ZeroMemory(&items, sizeof(items));

    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId(), 0, (LPARAM)&items };
    FindRoom(&context, 0, CompactingCallback);
    if (context.hShare && context.block)
        DoUnlock(context.block);

    assert(context.id == s_num);
    assert(sizeof(s_first_block.items) == sizeof(items));

    HANDLE hNext = s_first_block.hNext;
    CopyMemory(&s_first_block.items, &items, sizeof(s_first_block.items));
    s_first_block.num = context.id;
    s_first_block.hNext = NULL;

    DoFreeBlocks(hNext, s_first_block.ref_pid);
}

int AddItem(void)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    FindRoom(&context, 0, AddItemCallback);

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

    if (context.hShare && block)
        DoUnlock(block);

    return id;
}

void RemoveItemByPid(DWORD pid)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    FindRoom(&context, 0, RemoveByPidCallback);
    if (context.hShare && context.block)
        DoUnlock(context.block);

    if (s_num < BLOCK_CAPACITY && s_first_block.hNext)
        DoCompactingBlocks();
}

bool DisplayCallback(SHARE_CONTEXT *context, BLOCK *block, ITEM *item)
{
    static BLOCK *s_block = NULL;
    if (s_block != block)
    {
        s_block = block;
        printf("--- BLOCK ---\n");
        printf("num:%d, hNext:%p\n", block->num, block->hNext);
    }
    printf("id:%d, pid:%lu\n", item->id, item->pid);
    return true;
}

void DisplayBlocks()
{
    printf("s_num: %d\n", s_num);
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    FindRoom(&context, 0, DisplayCallback);
    if (context.hShare && context.block)
        DoUnlock(context.block);
}

int main(void)
{
    int id = AddItem();
    printf("id %d added\n", id);
    DisplayBlocks();

    printf("Press Enter key\n");
    fflush(stdout);
    getchar();

    RemoveItemByPid(GetCurrentProcessId());
    return 0;
}
