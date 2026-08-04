# pragma once

# include <atomic>
# include <cassert>
# include <cstdint>
# include <iostream>
# include <memory>
# include <mutex>


namespace SeanMemoryPool
{
# define MEMORY_POOL_NUM 64
# define SLOT_BASE_SIZE 8
# define MAX_SLOT_SIZE 512


/* 具体内存池的槽大小没法确定，因为每个内存池的槽大小不同(8的倍数)
   所以这个槽结构体的sizeof 不是实际的槽大小 */
struct Slot
{
    std::atomic<Slot*> next;    // 原子指针 
};


class MemoryPool
{
public:
    MemoryPool(size_t block_size = 4096);
    ~MemoryPool();

    void init(size_t);

    void* allocate();
    void  deallocate(void*);
private:
    void   allocateNewBlock();
    size_t padPointer(char* p, size_t align);

    // 使用CAS操作进行无锁入队和出队
    bool  pushFreeList(Slot* slot);
    Slot* popFreeList();
private:
    int                 block_size_;        // 内存块大小
    int                 slot_size_;         // 槽大小
    Slot*               first_block_;       // 指向内存池管理的首个实际块
    Slot*               current_slot_;      // 指向当前未被使用的槽
    std::atomic<Slot*>  free_list_;         // 指向空闲的槽(被使用过后又被释放的槽)
    Slot*               last_slot_;         // 作为当前内存块中最后能够存放元素的位置标识(超过该位置需申请新的内存块)
    std::mutex          mutex_for_block_;   // 保证多线程情况下避免不必要的重复开辟内存导致的浪费行为
    std::mutex          mutex_for_freelist_;// 保证多线程情况下正确访问 free_list
};


class HashBucket
{
public:
    static void initMemoryPool()
    {
        for (int i = 0; i < MEMORY_POOL_NUM; ++i)
        {
            getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
        }
    }

    // 单例模式
    static MemoryPool& getMemoryPool(int index)
    {
        static MemoryPool memory_pools[MEMORY_POOL_NUM];
        return memory_pools[index];
    }

    static void* useMemory(size_t size)
    {
        if (size <= 0)
            return nullptr;
        if (size > MAX_SLOT_SIZE)   // 大于512字节的内存，则使用new
            return operator new(size);

        // 相当于size / 8 向上取整（因为分配内存只能大不能小)
        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    static void freeMemory(void* ptr, size_t size)
    {
        if (!ptr)
            return;
        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }

        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }

    template<typename T, typename... Args>
    friend T* newElement(Args&&... args);

    template<typename T>
    friend void deleteElement(T* p);
};

template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* p = nullptr;
    // 1.根据元素大小选取合适的内存池分配内存
    p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)));
    if (p != nullptr)
    {
        // 2.在分配的内存上构造对象
        new(p) T(std::forward<Args>(args)...);
    }

    return p;
}

template<typename T>
void deleteElement(T* p)
{
    
    if (p)
    {
        // 1.对象析构
        p->~T();
        // 2.回收内存
        HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));
    }
}

}