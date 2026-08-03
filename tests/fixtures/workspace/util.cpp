#include "util.h"

namespace utils {

int triple(int x) {
    return x * 3;
}

Accumulator::Accumulator(int base) : sum_(base) {}

int Accumulator::add(int v) {
    sum_ += v;
    return triple(sum_);
}

int Accumulator::total() const {
    return sum_;
}

}
