# memory-pool

基于 C++17 实现的自定义内存池框架,对标 `tcmalloc` 的分层缓存思想,提供比 `new`/`malloc` 更高效、更低碎片、并发更友好的内存分配方案。

项目包含两个迭代版本:

| 版本 | 目录 | 架构 | 说明 |
| ---- | ---- | ---- | ---- |
| v1 | `v1/` | 单层内存池 + 哈希桶 | 每个槽大小 8 字节对齐的固定内存池,无锁自由链表 |
| v2 | `v2/` | **三层缓存架构** | ThreadCache → CentralCache → PageCache,粒度更细的锁与延迟归还 |

v2 是当前重点,采用三级缓存结构,将不同大小的内存请求分发到不同层级,配合无锁 / 自旋锁 / 延迟回收等机制,在多线程场景下显著降低锁竞争与系统调用次数。

---

## 目录结构

```
memory-pool/
├── v1/                          # 单层内存池版本
│   ├── include/MemoryPool.h     # MemoryPool / HashBucket / newElement 接口
│   ├── src/MemoryPool.cpp       # 无锁自由链表实现
│   ├── tests/UnitTest.cpp       # 与 new/delete 的对比基准
│   └── CMakeLists.txt
├── v2/                          # 三层缓存版本(重点)
│   ├── include/
│   │   ├── Common.h             # 对齐、大小类、BlockHeader 等公共定义
│   │   ├── ThreadCache.h        # 线程本地缓存
│   │   ├── CentralCache.h       # 中心缓存(含 SpanTracker、延迟归还)
│   │   ├── PageCache.h          # 页缓存(按页分配、span 合并)
│   │   └── MemoryPool.h         # 对外统一接口
│   ├── src/
│   │   ├── ThreadCache.cpp
│   │   ├── CentralCache.cpp
│   │   └── PageCache.cpp
│   ├── tests/
│   │   ├── UnitTest.cpp         # 功能正确性测试
│   │   └── PerformanceTest.cpp  # 与 new/delete 的性能对比
│   └── CMakeLists.txt
└── README.md
```

---

## 项目架构

### v1:单层内存池 + 哈希桶

v1 的核心是 `HashBucket`,将内存按槽大小划分为 64 个 `MemoryPool`,每个池的槽大小为 `8` 的倍数(8 ~ 512 字节),超过 512 字节的请求直接走 `operator new`。

- **MemoryPool**:每个池维护一段 4096 字节的连续内存块(`block_size_`),内部通过 `current_slot_` / `last_slot_` 标记剩余可用槽,同时维护一个无锁的 `free_list_`(已释放、可复用的槽)。
- **HashBucket**:`useMemory(size)` 将请求大小向上取整到 8 的倍数后路由到对应索引的 `MemoryPool`,对外通过模板 `newElement<T>` / `deleteElement<T>` 完成「分配 + 对象构造 / 析构 + 回收」。

```
                      ┌─────────────────────────────┐
  newElement<T>() ───▶│  HashBucket::useMemory(size) │
                      └──────────────┬──────────────┘
                                     │ size > 512 ? ──▶ operator new
                                     ▼
              size/8 映射 ──▶ MemoryPool[i]  (8~512 字节,共 64 个)
                                     │
                       ┌─────────────┴──────────────┐
                       │  free_list_ (无锁)          │
                       │  current_slot_ / last_slot_ │
                       └────────────────────────────┘
```

### v2:三层缓存架构(重点)

v2 借鉴 `tcmalloc` 的思想,将分配路径拆成三个层次,越靠上越接近线程、越快,越靠下越接近系统、越慢但负责批量供给:

```
  MemoryPool::allocate(size)
        │  size > 256KB ──────────────▶ malloc()  大对象直接走系统
        ▼
┌─────────────────────────────────────────────────────────┐
│ ① ThreadCache  (thread_local,每线程独享,无锁)             │
│    freeList_[index] 按大小类缓存空闲块,命中即返回           │
│    未命中 → 向 CentralCache 批量取一批                     │
│    释放过多 → 归还一批给 CentralCache                      │
└──────────────────────────┬──────────────────────────────┘
                           │ fetchRange / returnRange
┌──────────────────────────▼──────────────────────────────┐
│ ② CentralCache  (全局单例,按大小类自旋锁)                  │
│    centralFreeList_[index] 中心自由链表                    │
│    空闲 → 向 PageCache 取一个 span 切成小块                │
│    记录 span 信息(SpanTracker),延迟归还空闲 span           │
└──────────────────────────┬──────────────────────────────┘
                           │ allocateSpan / deallocateSpan
┌──────────────────────────▼──────────────────────────────┐
│ ③ PageCache  (全局单例,std::mutex)                        │
│    按页(4KB)管理 span,空闲 span 用 map 按页数索引          │
│    拆分大 span、合并相邻 span,最终经 mmap 向系统申请        │
└─────────────────────────────────────────────────────────┘
```

三层各自的职责与关键设计:

#### 1. ThreadCache —— 线程本地缓存

- 通过 `thread_local` 单例实现,每个线程持有独立实例,**分配/释放全程无锁**,是性能的第一道屏障。
- 内部维护两个数组:`freeList_[FREE_LIST_SIZE]`(各大小类的空闲链表头)与 `freeListSize_[FREE_LIST_SIZE]`(链表长度统计)。
- 释放时采用头插法入链表;当某大小类空闲块数量超过阈值(256)时,调用 `returnToCentralCache` 把多余块(保留 1/4)批量归还给中心缓存,避免线程囤积过多内存。

#### 2. CentralCache —— 中心缓存

- 全局单例,为每个大小类维护一个中心自由链表 `centralFreeList_[index]` 和一把自旋锁 `locks_[index]`。
- 中心链表为空时,向 `PageCache` 申请一个 span,再按 `size` 切成 `blockNum` 个小块串成链表。**span 的分割与小块链表的构建都发生在锁内**,保证只被一个线程执行一次。
- 通过 `SpanTracker`(无锁结构)记录每个 span 的起始地址、页数、总块数与空闲块数,为「把整块空闲 span 归还 PageCache」做准备。

#### 3. PageCache —— 页缓存

- 以 4KB 页为最小单位,`allocateSpan(numPages)` 从 `freeSpans_` 中找到「大于等于所需页数」的最小空闲 span(近似 best-fit),多余部分拆分后放回空闲链表。
- `deallocateSpan` 在归还时尝试与物理相邻的 span 合并,减少外部碎片;`spanMap_` 维护页地址到 span 的映射用于定位回收。
- 最终通过 `mmap`(`MAP_PRIVATE | MAP_ANONYMOUS`)向操作系统申请内存并清零。

#### 大小类管理(SizeClass)

`Common.h` 中的 `SizeClass` 将请求大小对齐到 8 字节并映射到数组索引:

- `ALIGNMENT = 8`、`MAX_BYTES = 256 * 1024`,故 `FREE_LIST_SIZE = 32768` 个大小类。
- `roundUp(bytes)`:向上对齐到 8 的倍数;`getIndex(bytes)`:将大小映射为 `[0, FREE_LIST_SIZE)` 的下标。
- 对齐保证每个小块内部可用 `void*` 直接复用「下一块地址」来串成隐式链表,无需额外头部开销(8 字节对齐正好容纳一个指针)。

---

## 同步机制

v2 的并发安全是分层的,每一层依据其访问频率与竞争程度选择了不同的同步策略:

| 层级 | 数据结构 | 同步方式 | 设计动机 |
| ---- | -------- | -------- | -------- |
| ThreadCache | `freeList_` 数组 | **无锁**(线程私有) | `thread_local` 隔离,天然无竞争,零同步开销 |
| CentralCache | `centralFreeList_` + `locks_` | **自旋锁**(`std::atomic_flag` 的 `test_and_set`) | 竞争中等,临界区短,自旋 + `yield` 比互斥锁更轻 |
| PageCache | `freeSpans_` / `spanMap_` | **互斥锁**(`std::mutex`) | 低频操作,临界区较长,直接用互斥锁简单可靠 |
| SpanTracker | 多个 `std::atomic<...>` | **无锁原子操作** | span 元信息跨层读写,用原子变量避免额外锁 |

### 具体实现要点

**1. 无锁自由链表(v1)**

v1 的 `free_list_` 使用 `std::atomic<Slot*>` + CAS(`compare_exchange_weak`)实现 Treiber 栈式的无锁入队/出队:

- `pushFreeList`:循环读取头节点 → 新节点 `next` 指向旧头 → CAS 尝试更新头,失败则重试。
- `popFreeList`:循环读取头节点 → 读取其 `next` → CAS 将头更新为 `next`,失败则重试。
- 而「开辟新内存块」这一低频且不可重复的操作,由 `mutex_for_block_` 保护,避免多个线程重复开辟浪费内存。

**2. 自旋锁(CentralCache)**

每个大小类一把独立的自旋锁,把不同大小类之间的竞争也拆开:

```cpp
while (locks_[index].test_and_set(std::memory_order_acquire))
    std::this_thread::yield();   // 忙等时让出 CPU,避免空转耗电
```

- `test_and_set` 获取锁,`clear` 释放锁,使用 acquire/release 内存序保证临界区内的可见性。
- 用 `try/catch` 包裹临界区,异常路径也保证释放锁,避免锁泄漏。

**3. 延迟归还机制(CentralCache)**

频繁归还小块会产生不必要的锁竞争与内存抖动,因此 CentralCache 采用「计数 + 时间」双重触发的**延迟归还**:

- 每次 `returnRange` 使 `delayCounts_[index]` 自增;当计数达到 `MAX_DELAY_COUNT = 48`,或距上次归还超过 `DELAY_INTERVAL = 1000ms` 时,触发 `performDelayedReturn`。
- `performDelayedReturn` 遍历中心自由链表,统计每个 span 的空闲块数;若某个 span 的**所有块均已空闲**,则将其从中心链表整体摘下,归还给 PageCache 进行页合并。
- 这样既减少了归还频率,又能在某个 span 完全空闲时及时回收整页内存,兼顾时间与空间。

**4. 跨层原子可见性(SpanTracker)**

`SpanTracker` 用 `std::atomic` 存储 span 的地址、页数、块数与空闲计数。当 CentralCache 从中心链表取出小块时对 `freeCount` 做 `fetch_sub`,延迟归还时再累加,避免为 span 元信息引入额外互斥锁。

---

## 我的主要工作

本项目 v2 的三层缓存架构是核心成果,主要工作集中在以下方面:

1. **设计并实现三级缓存架构**
   - 将内存分配拆分为 ThreadCache / CentralCache / PageCache 三层,按「线程本地 → 中心 → 页 → 系统」逐级降速供给,使高频分配命中线程缓存、批量操作收敛到中心层、系统调用最小化。
   - 定义 `SizeClass` 大小类机制,用 8 字节对齐把任意大小映射到 `FREE_LIST_SIZE` 个下标,块内用 `void*` 隐式串链,实现零额外头部的空闲链表。

2. **细粒度并发控制**
   - 为 ThreadCache 引入 `thread_local` 无锁缓存,把最频繁的分配/释放路径从全局锁中解放出来。
   - 为 CentralCache 每个大小类实现独立自旋锁,并加入 `yield` 避免忙等空转;用原子变量实现 `SpanTracker`,让 span 元信息跨层读写无需加锁。
   - 为 PageCache 使用互斥锁管理 span 分配、拆分与合并。

3. **内存回收与碎片治理**
   - 实现「批量取块 / 批量归还」策略:ThreadCache 从中心一次取一批、超阈值时保留 1/4 后归还,减少跨层调用频率。
   - 实现**延迟归还机制**(计数 + 时间双阈值),将完全空闲的 span 整页回收给 PageCache,并对物理相邻的 span 做合并,降低外部碎片。

4. **系统内存对接**
   - 在 PageCache 底层通过 `mmap` 向操作系统申请整页内存并清零,大对象(> 256KB)直接转发 `malloc`/`free`。

5. **测试与验证**
   - 编写功能单元测试(基础分配、写入、多线程、边界、压力)与性能对比基准(小对象、多线程、混合大小),将内存池与 `new/delete` 逐项对比,验证正确性与性能收益。

---

## 构建与运行

### v2(推荐)

```bash
cd v2
cmake -S . -B build
cmake --build build

./build/unit_test   # 功能正确性测试
./build/perf_test   # 与 new/delete 的性能对比
```

### v1

```bash
cd v1
cmake -S . -B build
cmake --build build

./build/MemoryPool-v1
```

---

## 对外接口(v2)

```cpp
#include "MemoryPool.h"

void* p = SeanMemoryPool::MemoryPool::allocate(128);   // 分配 128 字节
SeanMemoryPool::MemoryPool::deallocate(p, 128);        // 归还(需传入原大小)
```

> 说明:释放时需传入与分配时一致的大小,内部据此定位对应大小类。超过 `MAX_BYTES`(256KB)的请求自动转发给 `malloc`/`free`,大于 32KB 的中大请求由 PageCache 按实际页数分配。
