// SPDX-License-Identifier: MPL-2.0
#include "vfs/movie_extent.h"
#include "services/fssrv/mp4_probe.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

static void entry(Bytes &table, std::uint64_t start, std::uint64_t size) {
    auto index = table.size();
    table.resize(index + 36);
    for (unsigned i = 0; i < 8; ++i) {
        table[index + 8 + i] = start >> (8 * i);
        table[index + 16 + i] = size >> (8 * i);
    }
    table[index + 28] = 3;
    std::copy_n("vid", 3, table.begin() + index + 32);
}

int main(int argc, char **argv) {
    using skyline::vfs::MovieExtent;
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    Bytes movie{std::istreambuf_iterator<char>(input), {}};
    assert(skyline::service::fssrv::IsMp4Candidate(movie));
    Bytes partition(64, 0xfe);
    partition.insert(partition.end(), movie.begin(), movie.end());
    partition.resize(partition.size() + 16, 0xfd);
    Bytes table;
    entry(table, 0, 64);
    entry(table, 64, movie.size());
    entry(table, 64 + movie.size(), 16);
    auto size = MovieExtent(table, partition.size(), 64);
    assert(size && *size == movie.size());
    assert(std::equal(movie.begin(), movie.end(), partition.begin() + 64, partition.begin() + 64 + *size));
    assert(!MovieExtent(table, partition.size(), 65));
    assert(!MovieExtent(table, partition.size() - 1, 64));
    assert(!MovieExtent(std::span(table).first(table.size() - 1), partition.size(), 64));
    entry(table, 64, 16);
    assert(!MovieExtent(table, partition.size(), 64));
    table.clear();
    entry(table, 70, 1);
    entry(table, 64, 12);
    assert(!MovieExtent(table, 128, 64));
    assert(!MovieExtent(table, 128, 70));
    table.clear();
    entry(table, UINT64_MAX - 3, 16);
    assert(!MovieExtent(table, UINT64_MAX, UINT64_MAX - 3));
    table[28] = 255;
    assert(!MovieExtent(table, partition.size(), 64));
}
