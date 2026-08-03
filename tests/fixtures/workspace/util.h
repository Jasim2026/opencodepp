#pragma once

namespace utils {

int triple(int x);

class Accumulator {
public:
    explicit Accumulator(int base);
    int add(int v);
    int total() const;

private:
    int sum_;
};

}
