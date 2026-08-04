# include <iostream>
# include <thread>
# include <vector>
# include "../include/MemoryPool.h"

// 测试用例
class P1
{
    int id_;
};

class P2
{
    int id_[5];
};

class P3
{
    int id_[10];
};

class P4
{
    int id_[20];
};


void BenchmarkMemoryPool(size_t n_times, size_t n_works, size_t rounds)
{
    std::vector<std::thread> vthread(n_works);  // 线程池
    size_t total_cost_time = 0;
    for (size_t k = 0; k < n_works; ++k)    // 创建 n_works 个线程
    {
        vthread[k] = std::thread(
            [&]() {
                for (size_t j = 0; j < rounds; ++j)
                {
                    size_t begin1 = clock();
                    for (size_t i = 0; i < n_times; ++i)
                    {
                        P1* p1 = SeanMemoryPool::newElement<P1>();  // 内存池对外接口
                        SeanMemoryPool::deleteElement<P1>(p1);
                        P2* p2 = SeanMemoryPool::newElement<P2>();
                        SeanMemoryPool::deleteElement<P2>(p2);
                        P3* p3 = SeanMemoryPool::newElement<P3>();
                        SeanMemoryPool::deleteElement<P3>(p3);
                        P4* p4 = SeanMemoryPool::newElement<P4>();
                        SeanMemoryPool::deleteElement<P4>(p4);
                    }
                    size_t end1 = clock();

                    total_cost_time += end1 - begin1;
                }
            }
        );
    }
    for (auto& t : vthread)
    {
        t.join();
    }
    printf("%lu个线程并发执行%lu轮次, 每轮次newElement和deleteElement %lu次, 总计时长: %lu ms\n", n_works, rounds, n_times, total_cost_time);
}


void BenchmarkNew(size_t n_times, size_t n_works, size_t rounds)
{
    std::vector<std::thread> vthread(n_works);  // 线程池
    size_t total_cost_time = 0;
    for (size_t k = 0; k < n_works; ++k)    // 创建 n_works 个线程
    {
        vthread[k] = std::thread(
            [&]() {
                for (size_t j = 0; j < rounds; ++j)
                {
                    size_t begin1 = clock();
                    for (size_t i = 0; i < n_times; ++i)
                    {
                        P1* p1 = new P1;
                        delete p1;
                        P2* p2 = new P2;
                        delete p2;
                        P3* p3 = new P3;
                        delete p3;
                        P4* p4 = new P4;
                        delete p4;
                    }
                    size_t end1 = clock();

                    total_cost_time += end1 - begin1;
                }
            }
        );
    }
    for (auto& t : vthread)
    {
        t.join();
    }
    printf("%lu个线程并发执行%lu轮次, 每轮次new和delete %lu次, 总计时长: %lu ms\n", n_works, rounds, n_times, total_cost_time);
}


int main()
{
    SeanMemoryPool::HashBucket::initMemoryPool();   // 使用内存池接口前一定要先调用该函数
    BenchmarkMemoryPool(100, 1, 10); // 测试内存池
    BenchmarkMemoryPool(100, 5, 10); // 测试内存池
    BenchmarkMemoryPool(100, 10, 10); // 测试内存池
    BenchmarkMemoryPool(1000, 10, 100); // 测试内存池
    BenchmarkMemoryPool(1000, 30, 100); // 测试内存池
	std::cout << "===========================================================================" << std::endl;
	std::cout << "===========================================================================" << std::endl;
	BenchmarkNew(100, 1, 10); // 测试 new delete
    BenchmarkNew(100, 5, 10); // 测试 new delete
    BenchmarkNew(100, 10, 10); // 测试 new delete
    BenchmarkNew(1000, 10, 100); // 测试 new delete
    BenchmarkNew(1000, 30, 100); // 测试 new delete
    

    return 0;
}