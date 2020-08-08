// This file is part of PGM-index <https://github.com/gvinciguerra/PGM-index>.
// Copyright (c) 2018 Giorgio Vinciguerra.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <limits>
#include <vector>
#include <utility>
#include <algorithm>
#include "piecewise_linear_model.hpp"

#define ADD_ERR(x, epsilon, size) ((x) + (epsilon) >= (size) ? (size) : (x) + (epsilon))
#define SUB_ERR(x, epsilon) ((x) <= (epsilon) ? 0 : ((x) - (epsilon)))

/**
 * A struct that stores the result of a query to a @ref PGMIndex, that is, a range [@ref lo, @ref hi)
 * centered around an approximate position @ref pos of the sought key.
 */
struct ApproxPos {
    size_t pos; ///< The approximate position of the key.
    size_t lo;  ///< The lower bound of the range where the key can be found.
    size_t hi;  ///< The upper bound of the range where the key can be found.
};

/**
 * A space-efficient index that finds the position of a key within a radius of @p Epsilon.
 *
 * The index is constructed on a sorted sequence of keys. A query returns a struct @ref ApproxPos containing an
 * approximate position of the sought key and the bounds of the range of size 2*Epsilon where the sought key is
 * guaranteed to be found if present. In the case of repeated keys, the index finds the position of the first occurrence
 * of a key.
 *
 * The @p Epsilon template parameter should be set according to the desired space-time trade-off. A smaller value
 * makes the estimation more precise and the range smaller but at the cost of increased space usage.
 *
 * Internally the index uses a succinct piecewise linear mapping from keys to their position in the sorted order.
 * This mapping is represented as a sequence of linear models (segments) which, if @p EpsilonRecursive is not zero, are
 * themselves recursively indexed by other piecewise linear mappings.
 *
 * @tparam K the type of the indexed elements
 * @tparam Epsilon the maximum error allowed in the last level of the index
 * @tparam EpsilonRecursive the maximum error allowed in the upper levels of the index
 * @tparam Floating the floating-point type to use for slopes
 */
template<typename K, size_t Epsilon = 64, size_t EpsilonRecursive = 4, typename Floating = double>
class PGMIndex {
protected:
    template<typename, size_t, size_t, typename>
    friend class InMemoryPGMIndex;

    static_assert(Epsilon > 0);
    struct Segment;

    size_t n;                           ///< The number of elements this index was built on.
    K first_key;                        ///< The smallest element.
    std::vector<Segment> segments;      ///< The segments composing the index.
    std::vector<size_t> levels_sizes;   ///< The number of segment in each level, in reverse order.
    std::vector<size_t> levels_offsets; ///< The starting position of each level in segments[], in reverse order.

    template<typename RandomIt>
    void build(RandomIt first, RandomIt last, size_t epsilon, size_t epsilon_recursive) {
        if (n == 0)
            return;

        first_key = *first;
        levels_offsets.push_back(0);
        segments.reserve(n / (epsilon * epsilon));

        auto ignore_last = *std::prev(last) == std::numeric_limits<K>::max(); // max is reserved for padding
        auto last_n = n - ignore_last;
        last -= ignore_last;

        auto back_check = [this, last](size_t n_segments, size_t prev_level_size) {
            if (segments.back().slope == 0) {
                // Here, we need to ensure that keys > *(last-1) are approximated to a position == prev_level_size
                segments.emplace_back(*std::prev(last) + 1, 0, prev_level_size);
                ++n_segments;
            }
            segments.emplace_back(prev_level_size);
            return n_segments;
        };

        // Build first level
        auto in_fun = [this, first](auto i) {
            auto x = first[i];
            if (i > 0 && i + 1u < n && x == first[i - 1] && x != first[i + 1] && x + 1 != first[i + 1])
                return std::pair<K, size_t>(x + 1, i);
            return std::pair<K, size_t>(x, i);
        };
        auto out_fun = [this](auto cs) { segments.emplace_back(cs); };
        last_n = back_check(make_segmentation_par(last_n, epsilon, in_fun, out_fun), last_n);
        levels_offsets.push_back(levels_offsets.back() + last_n + 1);
        levels_sizes.push_back(last_n);

        // Build upper levels
        while (epsilon_recursive && last_n > 1) {
            auto offset = levels_offsets[levels_offsets.size() - 2];
            auto in_fun_rec = [this, offset](auto i) { return std::pair<K, size_t>(segments[offset + i].key, i); };
            last_n = back_check(make_segmentation(last_n, epsilon_recursive, in_fun_rec, out_fun), last_n);
            levels_offsets.push_back(levels_offsets.back() + last_n + 1);
            levels_sizes.push_back(last_n);
        }

        levels_offsets.pop_back();
    }

    /**
     * Returns the segment responsible for a given key, that is, the rightmost segment having key <= the sought key.
     * @param key the value of the element to search for
     * @return an iterator to the segment responsible for the given key
     */
    auto segment_for_key(const K &key) const {
        if constexpr (EpsilonRecursive == 0) {
            auto it = std::upper_bound(segments.begin(), segments.begin() + levels_sizes[0], key);
            return it == segments.begin() ? it : std::prev(it);
        }

        auto it = segments.begin() + levels_offsets.back();

        for (auto l = int(height()) - 2; l >= 0; --l) {
            auto level_begin = segments.begin() + levels_offsets[l];
            auto pos = std::min<size_t>((*it)(key), std::next(it)->intercept);
            auto lo = level_begin + SUB_ERR(pos, EpsilonRecursive + 1);

            static constexpr size_t linear_search_threshold = 8 * 64 / sizeof(Segment);
            if constexpr (EpsilonRecursive <= linear_search_threshold) {
                for (; std::next(lo)->key <= key; ++lo);
                it = lo;
            } else {
                auto level_size = levels_sizes[l];
                auto hi = level_begin + ADD_ERR(pos, EpsilonRecursive + 2, level_size);
                it = std::upper_bound(lo, hi, key);
                it = it == level_begin ? it : std::prev(it);
            }
        }
        return it;
    }

public:

    static constexpr size_t epsilon_value = Epsilon;

    /**
     * Constructs an empty index.
     */
    PGMIndex() = default;

    /**
     * Constructs the index on the given sorted data.
     * @param data the vector of keys, must be sorted
     */
    explicit PGMIndex(const std::vector<K> &data) : PGMIndex(data.begin(), data.end()) {}

    /**
     * Constructs the index on the sorted data in the range [first, last).
     * @param first, last the range containing the sorted elements to be indexed
     */
    template<typename RandomIt>
    PGMIndex(RandomIt first, RandomIt last)
        : n(std::distance(first, last)),
          first_key(),
          segments(),
          levels_sizes(),
          levels_offsets() {
        build(first, last, Epsilon, EpsilonRecursive);
    }

    /**
     * Returns the approximate position of a key.
     * @param key the value of the element to search for
     * @return a struct with the approximate position
     */
    ApproxPos find_approximate_position(const K &key) const {
        auto k = std::max(first_key, key);
        auto it = segment_for_key(k);
        auto pos = std::min<size_t>((*it)(k), std::next(it)->intercept);
        auto lo = SUB_ERR(pos, Epsilon);
        auto hi = ADD_ERR(pos, Epsilon + 1, n);
        return {pos, lo, hi};
    }

    /**
     * Returns the number of segments in the last level of the index.
     * @return the number of segments
     */
    size_t segments_count() const {
        return segments.empty() ? 0 : levels_sizes.front();
    }

    /**
     * Returns the number of levels in the index.
     * @return the number of levels in the index
     */
    size_t height() const {
        return levels_sizes.size();
    }

    /**
     * Returns the size of the index in bytes.
     * @return the size of the index in bytes
     */
    size_t size_in_bytes() const {
        return segments.size() * sizeof(Segment);
    }
};

#pragma pack(push, 1)

template<typename K, size_t Epsilon, size_t EpsilonRecursive, typename Floating>
struct PGMIndex<K, Epsilon, EpsilonRecursive, Floating>::Segment {
    K key;             ///< The first key that the segment indexes.
    Floating slope;    ///< The slope of the segment.
    int32_t intercept; ///< The intercept of the segment.

    Segment() = default;

    Segment(size_t n) : key(std::numeric_limits<K>::max()), slope(), intercept(n) {};

    Segment(K key, Floating slope, Floating intercept) : key(key), slope(slope), intercept(intercept) {};

    explicit Segment(const typename OptimalPiecewiseLinearModel<K, size_t>::CanonicalSegment &cs)
        : key(cs.get_first_x()) {
        auto[cs_slope, cs_intercept] = cs.get_floating_point_segment(key);
        if (cs_intercept > std::numeric_limits<decltype(intercept)>::max())
            throw std::overflow_error("Change the type of Segment::intercept to int64");
        slope = cs_slope;
        intercept = std::round(cs_intercept);
    }

    friend inline bool operator<(const Segment &s, const K &k) { return s.key < k; }
    friend inline bool operator<(const K &k, const Segment &s) { return k < s.key; }

    /**
     * Returns the approximate position of the specified key.
     * @param k the key whose position must be approximated
     * @return the approximate position of the specified key
     */
    inline size_t operator()(const K &k) const {
        auto pos = int64_t(slope * (k - key)) + intercept;
        return pos > 0 ? size_t(pos) : 0ull;
    }
};

#pragma pack(pop)

/**
 * A space-efficient index that finds the position of a sought key within a radius of @p Epsilon. This variant uses a
 * binary search in the last level, and it should only be used when BinarySearchBasedPGMIndex::size_in_bytes() is low
 * (for example, less than the last level cache size).
 * @tparam K the type of the indexed elements
 * @tparam Epsilon the maximum error allowed in the last level of the index
 * @tparam Floating the floating-point type to use for slopes
 */
template<typename K, size_t Epsilon, typename Floating = double>
using BinarySearchBasedPGMIndex = PGMIndex<K, Epsilon, 0, Floating>;
