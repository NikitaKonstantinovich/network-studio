#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <cstdint>
#include <type_traits>
#include <mutex>
#include <thread>
#include <vector>
#include "compute_threads.h"

namespace studio::numerical {
// One synchronous phase at a time, submitted exclusively by the controller.
// Count INCLUDES the controller (block 0). No nested dispatch, task queue, or
// per-phase allocation. Callables/inputs live until all blocks have joined.
class WorkerPool {
    struct alignas(64) Slot {
        std::atomic<uint64_t> finished{0};
        std::exception_ptr error;
    };
    std::array<Slot,compute::MaxThreads> slots;
    std::vector<std::thread> workers;
    alignas(64) std::atomic<uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    std::atomic<int> sleepers{0};
    std::mutex mutex;
    std::condition_variable wake;
    void* context=nullptr;
    void (*invoke)(void*,size_t,size_t,int)=nullptr;
    size_t size=0;
    int count_;
    void work(int id);
    void block(int id) noexcept;
    void dispatch(size_t size,void* context,void (*invoke)(void*,size_t,size_t,int));
public:
    explicit WorkerPool(int count);
    ~WorkerPool();
    WorkerPool(const WorkerPool&)=delete;
    WorkerPool& operator=(const WorkerPool&)=delete;
    int count() const {return count_;}
    template<class Function> void run(size_t size,Function&& function) {
        dispatch(size,&function,[](void* p,size_t begin,size_t end,int id){
            (*static_cast<std::remove_reference_t<Function>*>(p))(begin,end,id);
        });
    }
};
}
