# include "../include/MemoryPool.h"

namespace SeanMemoryPool
{

MemoryPool::MemoryPool(size_t block_size)
    : block_size_(block_size)
    , slot_size_(0)
    , first_block_(nullptr)
    , current_slot_(nullptr)
    , free_list_(nullptr)
    , last_slot_(nullptr)
    {}

MemoryPool::~MemoryPool()
{
    // 删除连续的block链表
    Slot* cur = first_block_;
    while(cur)
    {
        Slot* next = cur->next;
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}

void MemoryPool::init(size_t size)
{
    assert(size > 0);
    slot_size_ = size;
    first_block_ = nullptr;
    current_slot_ = nullptr;
    free_list_ = nullptr;
    last_slot_ = nullptr;
}

void* MemoryPool::allocate()
{
    // 优先使用空闲链表的内存槽
    Slot* slot = popFreeList();
    if (slot != nullptr)
        return slot;

    Slot* temp;
    {
        std::lock_guard<std::mutex> lock(mutex_for_block_);
        if (current_slot_ >= last_slot_)
        {
            // 当前内存块已无内存槽可用,开辟一块新的内存块
            allocateNewBlock();
        }

        temp = current_slot_;
        // 这里不能直接 curSlot_ += SlotSize_ 因为curSlot_是Slot*类型，所以需要除以sizeof(Slot)
        current_slot_ = current_slot_ + slot_size_ / sizeof(Slot);
    }

    return temp;
}

void MemoryPool::deallocate(void* ptr)
{
    if (!ptr) return;

    Slot* slot = reinterpret_cast<Slot*>(ptr);
    pushFreeList(slot);
}

void MemoryPool::allocateNewBlock()
{
    // 头插法插入新的内存块
    void* new_block = operator new(block_size_);
    reinterpret_cast<Slot*>(new_block)->next = first_block_;
    first_block_ = reinterpret_cast<Slot*>(new_block);

    char* body = reinterpret_cast<char*>(new_block) + sizeof(Slot*);
    size_t padding_size = padPointer(body, slot_size_);   // 计算对齐需要填充内存的大小
    current_slot_ = reinterpret_cast<Slot*>(body + padding_size);

    last_slot_ = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(new_block) + block_size_ - slot_size_ + 1);
}

// 让指针对齐到槽大小的倍数位置
size_t MemoryPool::padPointer(char* p, size_t align)
{
    size_t rem = (reinterpret_cast<size_t>(p) % align);
    return rem == 0 ? 0 : (align - rem);
}

// 实现无锁入队操作
bool MemoryPool::pushFreeList(Slot* slot)
{
    while(true)
    {
        // 获取当前头节点
        Slot* old_head = free_list_.load(std::memory_order_relaxed);
        // 将新节点的 next 指向当前头节点
        slot->next.store(old_head, std::memory_order_relaxed);
        // 尝试将新节点设置为头节点
        if (free_list_.compare_exchange_weak(old_head, slot, std::memory_order_release, std::memory_order_relaxed))
            return true;
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}

// 实现无锁出队操作
Slot* MemoryPool::popFreeList()
{
    while(true)
    {
        Slot* old_head = free_list_.load(std::memory_order_acquire);
        if (old_head == nullptr)
            return nullptr;

        // 在访问 newHead 之前再次验证 oldHead 的有效性
        Slot* new_head = nullptr;
        try
        {
            new_head = old_head->next.load(std::memory_order_relaxed);
        }
        catch(...)
        {   // 如果返回失败，则continue重新尝试申请内存
            continue;
        }

        // 尝试更新头结点
        // 原子性地尝试将 freeList_ 从 oldHead 更新为 newHead
        if (free_list_.compare_exchange_weak(old_head, new_head, std::memory_order_acquire, std::memory_order_relaxed))
        {
            return old_head;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}

}