#include "core/metrics.h"

#include <cmath>

namespace opencode::core {

Metrics::Entry* Metrics::find(std::string_view name) noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name) return &entries_[i];
    }
    return nullptr;
}

const Metrics::Entry* Metrics::find(std::string_view name) const noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name) return &entries_[i];
    }
    return nullptr;
}

Metrics::Entry* Metrics::ensure(std::string_view name, Kind kind) noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].name == name)
            return entries_[i].kind == kind ? &entries_[i] : nullptr;
    }
    if (count_ >= sizeof(entries_) / sizeof(entries_[0])) return nullptr;
    Entry* e = &entries_[count_++];
    e->name = name;
    e->kind = kind;
    e->value = 0;
    for (auto& b : e->hcount) b = 0;
    e->hsamples = 0;
    e->hsum = 0.0;
    return e;
}

void Metrics::inc(std::string_view name, uint64_t by) noexcept {
    if (Entry* e = ensure(name, Kind::counter)) e->value += static_cast<int64_t>(by);
}

void Metrics::dec(std::string_view name, uint64_t by) noexcept {
    if (Entry* e = ensure(name, Kind::counter)) e->value -= static_cast<int64_t>(by);
}

void Metrics::set(std::string_view name, int64_t value) noexcept {
    if (Entry* e = ensure(name, Kind::gauge)) e->value = value;
}

size_t Metrics::bucket_for(double seconds) noexcept {
    if (seconds <= 1e-6) return 0;
    const double us = seconds * 1e6;
    const double idx = std::log2(us);
    if (idx < 0) return 0;
    const double ceil = std::ceil(idx);
    const size_t i = static_cast<size_t>(ceil);
    return i >= kBuckets ? kBuckets - 1 : i;
}

void Metrics::observe(std::string_view name, double seconds) noexcept {
    Entry* e = ensure(name, Kind::histogram);
    if (e == nullptr) return;
    e->hcount[bucket_for(seconds)]++;
    e->hsamples++;
    e->hsum += seconds;
}

double Metrics::percentile_entry(const Entry& e, double pct) const noexcept {
    if (e.kind != Kind::histogram || e.hsamples == 0) return -1.0;
    uint64_t target =
        static_cast<uint64_t>(static_cast<double>(e.hsamples) * pct / 100.0);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
        cum += e.hcount[i];
        if (cum >= target) return upper_edge(i);
    }
    return upper_edge(kBuckets - 1);
}

void Metrics::snapshot_raw(void* userdata, Sink sink) const noexcept {
    for (size_t i = 0; i < count_; ++i) {
        const Entry& e = entries_[i];
        switch (e.kind) {
            case Kind::counter:
            case Kind::gauge:
                sink(userdata, e.name, e.kind, static_cast<double>(e.value), 0);
                break;
            case Kind::histogram:
                sink(userdata, e.name, e.kind, percentile_entry(e, 50.0),
                     e.hsamples);
                break;
        }
    }
}

double Metrics::percentile(std::string_view name, double pct) const noexcept {
    const Entry* e = find(name);
    if (e == nullptr) return -1.0;
    return percentile_entry(*e, pct);
}

double Metrics::upper_edge(size_t i) noexcept {
    if (i == 0) return 1e-6;
    return std::ldexp(1.0, static_cast<int>(i)) * 1e-6;
}

} /* namespace opencode::core */
