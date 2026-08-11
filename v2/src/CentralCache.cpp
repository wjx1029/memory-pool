#include "../include/CentralCache.h"
#include "../include/PageCache.h"

#include <cassert>
#include <thread>
#include <chrono>

namespace SeanMemoryPool
{

const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;



}