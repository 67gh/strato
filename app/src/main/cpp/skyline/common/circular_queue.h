// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <memory>
#include <stdexcept>
#include <common/trace.h>
#include <common/spin_lock.h>
#include <common/span.h>

namespace skyline {
    /**
     * @brief An efficient consumer-producer oriented queue with internal synchronization
     */
    template<typename Type>
    class CircularQueue {
      private:
        std::allocator<Type> allocator;
        const size_t capacity;
        Type *storage; //!< Aligned uninitialized storage; only queued items are alive
        std::atomic<Type *> start; //!< Sentinel before the oldest item
        std::atomic<bool> closed{};
        std::atomic<Type *> end; //!< Newest item
        SpinLock consumptionMutex;
        std::condition_variable_any consumeCondition;
        SpinLock productionMutex;
        std::condition_variable_any produceCondition;

      public:
        /**
         * @note The internal allocation is an item larger as we require a sentinel value
         */
        CircularQueue(size_t size)
            : capacity{size && size < std::allocator_traits<std::allocator<Type>>::max_size(allocator)
                ? size + 1 : throw std::invalid_argument("Invalid circular queue capacity")},
              storage{allocator.allocate(capacity)}, start{storage}, end{storage} {}

        CircularQueue(const CircularQueue &) = delete;

        CircularQueue &operator=(const CircularQueue &) = delete;

        CircularQueue(CircularQueue &&) = delete;

        ~CircularQueue() {
            while (start != end) {
                auto next{start + 1};
                next = (next == storage + capacity) ? storage : next;
                std::destroy_at(next);
                start = next;
            }
            allocator.deallocate(storage, capacity);
        }

        /**
         * @return Whether the queue is empty
         */
        // Call before joining consumers; existing items are drained by Process.
        void Close() {
            { std::scoped_lock lock{productionMutex, consumptionMutex}; closed = true; }
            produceCondition.notify_all();
            consumeCondition.notify_all();
        }

        bool Empty() {
            return start == end;
        }

        /**
         * @brief A blocking for-each that runs on every item and waits till new items to run on them as well
         * @param function A function that is called for each item (with the only parameter as a reference to that item)
         * @param preWait An optional function that's called prior to waiting on more items to be queued
         */
        template<typename F1, typename F2>
        void Process(F1 function, F2 preWait) {
            TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");

            while (true) {
                if (start == end) {
                    std::unique_lock productionLock{productionMutex};
                    TRACE_EVENT_END("containers");
                    preWait();
                    produceCondition.wait(productionLock, [this]() { return closed || start != end; });
                    if (closed && start == end)
                        return;
                    TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");
                }

                std::scoped_lock comsumptionLock{consumptionMutex};
                while (start != end) {
                    auto next{start + 1};
                    next = (next == storage + capacity) ? storage : next;
                    function(*next);
                    std::destroy_at(next);
                    start = next;
                }

                consumeCondition.notify_all();
            }
        }

        Type Pop() {
            for (;;) {
                {
                    std::unique_lock productionLock{productionMutex};
                    produceCondition.wait(productionLock, [this]() { return closed || start != end; });
                    if (closed && start == end)
                        throw std::runtime_error("Pop on closed circular queue");
                }
                std::scoped_lock consumptionLock{consumptionMutex};
                if (start == end)
                    continue; // Another consumer won; never move an unconstructed slot.
                auto next{start + 1};
                next = (next == storage + capacity) ? storage : next;
                Type item{std::move(*next)};
                std::destroy_at(next);
                start = next;
                consumeCondition.notify_all();
                return item;
            }
        }

        void Push(const Type &item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, item);
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        void Push(Type &&item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, std::move(item));
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        template<typename... Args>
        void Emplace(Args &&... args) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return closed || waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                if (closed)
                    throw std::runtime_error("Push on closed circular queue");
                auto next{end + 1};
                next = (next == storage + capacity) ? storage : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, std::forward<Args>(args)...);
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        /**
         * @note The appended elements may not necessarily be directly contiguous as another thread could push elements in between those in the span
         */
        void Append(span<Type> buffer) {
            for (const auto &item : buffer)
                Push(item);
        }

        /**
         * @brief Appends a buffer with an alternative input type while supplied transformation function
         * @param tranformation A function that takes in an item of TransformedType as input and returns an item of Type
         */
        template<typename TransformedType, typename Transformation>
        void AppendTranform(TransformedType &container, Transformation transformation) {
            for (const auto &item : container)
                Push(transformation(item));
        }
    };
}
