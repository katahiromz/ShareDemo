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
        {
            break;
        }
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
    s_first_block.ref_pid = 0;
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

bool DisplayCallback(SHARE_CONTEXT *context, BLOCK *block, ITEM *item)
{
    static BLOCK *s_block = NULL;
    if (s_block != block)
    {
        s_block = block;
        printf("--- BLOCK ---\n");
        printf("num:%d, hNext:%p, ref_pid:%u\n", block->num, block->hNext, block->ref_pid);
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

void enter_key()
{
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
    HANDLE hShare = NULL;
    DWORD ref_pid = block->ref_pid;

    while (block->hNext)
    {
        BLOCK *next_block = (BLOCK *)DoLock(block->hNext, block->ref_pid);
        if (!next_block)
            break;

        if (ref_pid == pid)
        {
            HANDLE hNewShare = SHAllocShared(next_block, sizeof(BLOCK), another_pid);
            if (hShare)
                DoFree(hShare, pid);

            block->hNext = hNewShare;
            block->ref_pid = another_pid;
            DoUnlock(block);

            hShare = hNewShare;
            block = (BLOCK *)DoLock(hShare, ref_pid);
        }
        else
        {
            hShare = next_block->hNext;
            DoUnlock(block);
            block = next_block;
        }
    }

    if (!IsProcessRunning(block->ref_pid))
    {
        block->hNext = NULL;
        block->ref_pid = 0;
    }

    if (block != &s_first_block)
    {
        DoUnlock(block);
    }
}

void RemoveItemByPid(DWORD pid)
{
    SHARE_CONTEXT context = { NULL, &s_first_block, GetCurrentProcessId() };
    FindRoom(&context, 0, RemoveByPidCallback);
    if (context.hShare && context.block)
        DoUnlock(context.block);

    MoveOwnership(pid);

    if (s_num < BLOCK_CAPACITY && s_first_block.hNext)
        DoCompactingBlocks();
}

int main(void)
{
    printf("pid<>: %lu\n", GetCurrentProcessId());
    int id = AddItem();
    printf("id %d added\n", id);
    DisplayBlocks();

    printf("Press Enter key\n");
    fflush(stdout);
    enter_key();

    RemoveItemByPid(GetCurrentProcessId());

    DisplayBlocks();
    printf("Press Enter key\n");
    fflush(stdout);
    enter_key();
    return 0;
}
