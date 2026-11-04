// SPDX-License-Identifier: MPL-2.0
#include "services/fssrv/mp4_probe.h"
#include <algorithm>
#include <array>
#include <cassert>

int main() {
    using skyline::service::fssrv::IsMp4Candidate;
    std::array<std::uint8_t, 20> box{
        0, 0, 0, 20, 'f','t','y','p', 'a','v','c','1', 0,0,0,0, 'm','p','4','2'
    };
    assert(IsMp4Candidate(box));
    for (std::size_t end = 0; end < box.size(); ++end)
        assert(!IsMp4Candidate(std::span(box).first(end)));
    box[8] = 'x';
    assert(IsMp4Candidate(box)); // compatible brand
    box[16] = 'x';
    assert(!IsMp4Candidate(box));
    std::copy_n("isom", 4, box.begin() + 12);
    assert(!IsMp4Candidate(box)); // minor version is numeric
    std::copy_n("isom", 4, box.begin() + 8);
    for (unsigned length : {0U, 1U, 15U, 17U, 21U, 255U}) {
        box[3] = static_cast<std::uint8_t>(length);
        assert(!IsMp4Candidate(box));
    }
    box[3] = 20;
    box[0] = 255;
    assert(!IsMp4Candidate(box));
    box[0] = 0;
    box[4] = 'x';
    assert(!IsMp4Candidate(box));
}
