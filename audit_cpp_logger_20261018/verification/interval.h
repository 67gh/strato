#include <algorithm>
#include <vector>
#include <utility>
#include <span>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <stdexcept>
namespace skyline {using u8=uint8_t;template<class T> using span=std::span<T>;using SpinLock=std::mutex;}
#define TRACE_EVENT_BEGIN(...)
#define TRACE_EVENT_END(...)
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once




namespace skyline {
    /**
     * @brief Stores a list of non-overlapping intervals
     */
    template<typename SizeType>
    class IntervalList {
      public:
        using DifferenceType = decltype(std::declval<SizeType>() - std::declval<SizeType>());

        struct Interval {
            SizeType offset;
            SizeType end;

            Interval() = default;

            Interval(SizeType offset, SizeType end) : offset{offset}, end{end} {}

            Interval(span<u8> interval) : offset{interval.data()}, end{interval.data() + interval.size()} {}
        };

      private:
        std::vector<Interval> intervals; //!< A list of intervals sorted by their end offset

      public:
        struct QueryResult {
            bool enclosed; //!< If the given offset was enclosed by an interval
            DifferenceType size; //!< Size of the interval starting from the query offset, or distance to the next interval if `enclosed` is false (if there is no next interval size is 0)
        };

        /**
         * @brief Clears all inserted intervals from the map
         */
        void Clear() {
            intervals.clear();
        }

        /**
         * @brief Forces future accesses to the given interval to use the shadow copy
        */
        void Insert(Interval entry) {
            if (entry.offset >= entry.end)
                return; // Empty/reversed half-open intervals contain no bytes.
            auto firstIt{std::lower_bound(intervals.begin(), intervals.end(), entry, [](const auto &lhs, const auto &rhs) {
                return lhs.end < rhs.offset;
            })}; // Lowest offset entry that (maybe) overlaps with the new entry

            if (firstIt == intervals.end() || firstIt->offset >= entry.end) {
                intervals.insert(firstIt, entry);
                return;
            }
            // Now firstIt will always overlap

            auto lastIt{firstIt}; // Highest offset entry that overlaps with the new entry
            while (std::next(lastIt) != intervals.end() && std::next(lastIt)->offset < entry.end)
                lastIt++;

            // Since firstIt and lastIt both are guaranteed to overlap, max them to get the new entry's end
            SizeType end{std::max(std::max(firstIt->end, entry.end), lastIt->end)};

            // Erase all overlapping entries but the first
            auto eraseStartIt{std::next(firstIt)};
            auto eraseEndIt{std::next(lastIt)};
            if (eraseStartIt != eraseEndIt) {
                lastIt = intervals.erase(eraseStartIt, eraseEndIt);
                firstIt = std::prev(lastIt);
            }

            firstIt->offset = std::min(entry.offset, firstIt->offset);
            firstIt->end = end;
        }

        /**
         * @brief Merge the given interval into the list
         */
        void Merge(const IntervalList<SizeType> &list, DifferenceType displacement = {}) {
            if (&list == this) {
                if (!displacement)
                    return;
                auto copy{list};
                Merge(copy, displacement);
                return;
            }
            for (const auto &entry : list.intervals)
                Insert({entry.offset + displacement, entry.end + displacement});
        }

        /**
         * @return A struct describing the interval containing `offset`
         */
        QueryResult Query(SizeType offset) {
            auto it{std::lower_bound(intervals.begin(), intervals.end(), offset, [](const auto &lhs, const auto &rhs) {
                return lhs.end <= rhs;
            })}; // Lowest offset entry that (maybe) overlaps with the new entry

            if (it == intervals.end()) // No overlaps past offset
                return {false, {}};
            else if (it->offset > offset) // No overlap, return the distance to the next possible overlap
                return {false, it->offset - offset};
            else // Overlap, return the distance to the end of the overlap
                return {true, it->end - offset};
        }

        /**
         * @return If the given interval intersects with any of the intervals in the list
         */
        bool Intersect(Interval interval) {
            SizeType offset{interval.offset};
            while (offset < interval.end) {
                if (auto result{Query(offset)}; result.enclosed)
                    return true;
                else if (result.size)
                    offset += result.size;
                else
                    return false;
            }

            return false;
        }
    };
}
