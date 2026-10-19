#include "worker_pool.h"
#include <chrono>
#include <stdexcept>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace studio::numerical {
namespace {
void relax() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}
}
WorkerPool::WorkerPool(int count):count_(count) {
    if(count<2||count>8)throw std::invalid_argument("Worker pool count must be 2..8.");
    try {for(int id=1;id<count;++id)workers.emplace_back([this,id]{work(id);});}
    catch(...) {
        stopping.store(true,std::memory_order_release);
        {std::lock_guard<std::mutex> lock(mutex);wake.notify_all();}
        for(auto& worker:workers)worker.join();throw;
    }
}
WorkerPool::~WorkerPool() {
    stopping.store(true,std::memory_order_release);
    {std::lock_guard<std::mutex> lock(mutex);wake.notify_all();}
    for(auto& worker:workers)worker.join();
}
void WorkerPool::block(int id) noexcept {
    slots[id].error=nullptr;
    try {invoke(context,size*id/count_,size*(id+1)/count_,id);}
    catch(...) {slots[id].error=std::current_exception();}
}
void WorkerPool::work(int id) {
    uint64_t seen=0;
    for(;;) {
        uint64_t next=generation.load(std::memory_order_acquire);
        // Brief spinning amortizes the many short dependent solver phases.
        // Park after inactivity, including GUI pauses and exports.
        for(int spin=0;next==seen&&!stopping.load(std::memory_order_acquire)&&spin<2000;++spin) {
            relax();next=generation.load(std::memory_order_acquire);
        }
        if(stopping.load(std::memory_order_acquire))return;
        if(next==seen) {
            std::unique_lock<std::mutex> lock(mutex);sleepers.fetch_add(1);
            wake.wait(lock,[&]{return stopping.load(std::memory_order_acquire)||generation.load(std::memory_order_acquire)!=seen;});
            sleepers.fetch_sub(1);
            if(stopping.load(std::memory_order_acquire))return;
            next=generation.load(std::memory_order_acquire);
        }
        block(id);seen=next;slots[id].finished.store(seen,std::memory_order_release);
    }
}
void WorkerPool::dispatch(size_t elements,void* data,void (*function)(void*,size_t,size_t,int)) {
    size=elements;context=data;invoke=function;
    uint64_t next;
    {
        // Publish under the same mutex used when entering the parked state.
        // This closes the missed-notification race on every memory model.
        std::lock_guard<std::mutex> lock(mutex);
        next=generation.fetch_add(1,std::memory_order_release)+1;
        if(sleepers.load())wake.notify_all();
    }
    block(0);
    for(int id=1;id<count_;++id) {
        int spin=0;
        while(slots[id].finished.load(std::memory_order_acquire)!=next) {
            if(++spin<4000)relax();else std::this_thread::yield();
        }
    }
    for(int id=0;id<count_;++id)if(slots[id].error)std::rethrow_exception(slots[id].error);
}
}
