// See LICENSE for license details.

#include "cachesim.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>

#include "common.h"

cache_sim_t::cache_sim_t(size_t _sets, size_t _ways, size_t _linesz,
                         const char* _name)
    : sets(_sets), ways(_ways), linesz(_linesz), name(_name), log(false) {
    init();
}

static void help() {
    std::cerr << "Cache configurations must be of the form" << std::endl;
    std::cerr << "  sets:ways:blocksize" << std::endl;
    std::cerr << "where sets, ways, and blocksize are positive integers, with"
              << std::endl;
    std::cerr << "sets and blocksize both powers of two and blocksize at least 8." << std::endl;
    exit(1);
}

cache_sim_t* cache_sim_t::construct(const char* config, const char* name) {
    const char* wp = strchr(config, ':');
    if (!wp++) help();
    const char* bp = strchr(wp, ':');
    if (!bp++) help();

    size_t sets = atoi(std::string(config, wp).c_str());
    size_t ways = atoi(std::string(wp, bp).c_str());
    size_t linesz = atoi(bp);

    if (ways > 4 /* empirical */ && sets == 1)
        return new fa_cache_sim_t(ways, linesz, name);
    return new cache_sim_t(sets, ways, linesz, name);
}

void cache_sim_t::init() {
    if (sets == 0 || (sets & (sets - 1))) help();
    if (linesz < 8 || (linesz & (linesz - 1))) help();

    idx_shift = 0;
    for (size_t x = linesz; x > 1; x >>= 1) idx_shift++;

    tags = new uint64_t[sets * ways]();

    // lru array
    lru = new uint64_t[sets * ways]();
    for (size_t i = 0; i < sets; i++) {
        for (size_t j = 0; j < ways; j++) {
            lru[i * ways + j] = ways - j - 1;
        }
    }

    read_accesses = 0;
    read_misses = 0;
    bytes_read = 0;
    write_accesses = 0;
    write_misses = 0;
    bytes_written = 0;
    writebacks = 0;

    miss_handler = NULL;
}

cache_sim_t::cache_sim_t(const cache_sim_t& rhs)
    : sets(rhs.sets),
      ways(rhs.ways),
      linesz(rhs.linesz),
      idx_shift(rhs.idx_shift),
      name(rhs.name),
      log(false) {
    tags = new uint64_t[sets * ways];
    memcpy(tags, rhs.tags, sets * ways * sizeof(uint64_t));

    lru = new uint64_t[sets * ways];
    memcpy(lru, rhs.lru, sets * ways * sizeof(uint64_t));
}

cache_sim_t::~cache_sim_t() {
    print_stats();
    delete[] tags;
    delete[] lru;
}

void cache_sim_t::print_stats() {
    if (read_accesses + write_accesses == 0) return;

    float mr = 100.0f * (read_misses + write_misses) /
               (read_accesses + write_accesses);

    std::cout << std::setprecision(3) << std::fixed;
    std::cout << name << " ";
    std::cout << "Bytes Read:            " << bytes_read << std::endl;
    std::cout << name << " ";
    std::cout << "Bytes Written:         " << bytes_written << std::endl;
    std::cout << name << " ";
    std::cout << "Read Accesses:         " << read_accesses << std::endl;
    std::cout << name << " ";
    std::cout << "Write Accesses:        " << write_accesses << std::endl;
    std::cout << name << " ";
    std::cout << "Read Misses:           " << read_misses << std::endl;
    std::cout << name << " ";
    std::cout << "Write Misses:          " << write_misses << std::endl;
    std::cout << name << " ";
    std::cout << "Writebacks:            " << writebacks << std::endl;
    std::cout << name << " ";
    std::cout << "Miss Rate:             " << mr << '%' << std::endl;
}

uint64_t* cache_sim_t::check_tag(uint64_t addr) {
    size_t idx = (addr >> idx_shift) & (sets - 1);
    size_t tag = (addr >> idx_shift) | VALID;

    for (size_t k = 0; k < ways; k++)
        if (tag == (tags[idx * ways + k] & ~DIRTY)) {
            size_t updated_way = k;
            for (size_t i = 0; i < ways; i++) {
                if (lru[idx * ways + i] == updated_way) {
                    // Shift all elements that are before the updated way down one
                    for (size_t j = i; j > 0; j--) {
                        lru[idx * ways + j] = lru[idx * ways + j - 1];
                    }
                    lru[idx * ways] = updated_way;
                    break;
                }
            }
            return &tags[idx * ways + k];
        }

    return NULL;
}

uint64_t cache_sim_t::victimize(uint64_t addr) {
    size_t idx = (addr >> idx_shift) & (sets - 1);
    size_t lru_way = lru[idx * ways + ways - 1];

    // shift all the ways down one
    for (size_t i = ways - 1; i > 0; i--) {
        lru[idx * ways + i] = lru[idx * ways + i - 1];
    }

    // put the new way in the front
    lru[idx * ways] = lru_way;

    uint64_t victim = tags[idx * ways + lru_way];
    tags[idx * ways + lru_way] = (addr >> idx_shift) | VALID;
    return victim;
}

void cache_sim_t::access(uint64_t addr, size_t bytes, bool store) {
    store ? write_accesses++ : read_accesses++;
    (store ? bytes_written : bytes_read) += bytes;

    uint64_t* hit_way = check_tag(addr);
    if (likely(hit_way != NULL)) {
        size_t idx = (addr >> idx_shift) & (sets - 1);
        if (store) *hit_way |= DIRTY;
        return;
    }

    store ? write_misses++ : read_misses++;
    if (log) {
        std::cerr << name << " " << (store ? "write" : "read") << " miss 0x"
                  << std::hex << addr << std::endl;
    }

    uint64_t victim = victimize(addr);

    if ((victim & (VALID | DIRTY)) == (VALID | DIRTY)) {
        uint64_t dirty_addr = (victim & ~(VALID | DIRTY)) << idx_shift;
        if (miss_handler) miss_handler->access(dirty_addr, linesz, true);
        writebacks++;
    }

    if (miss_handler) miss_handler->access(addr & ~(linesz - 1), linesz, false);

    if (store) *check_tag(addr) |= DIRTY;
}

fa_cache_sim_t::fa_cache_sim_t(size_t ways, size_t linesz, const char* name)
    : cache_sim_t(1, ways, linesz, name) {}

uint64_t* fa_cache_sim_t::check_tag(uint64_t addr) {
    size_t idx = (addr >> idx_shift) & (sets - 1);
    size_t tag = (addr >> idx_shift) | VALID;

    for (size_t k = 0; k < ways; k++)
        if (tag == (tags[idx * ways + k] & ~DIRTY)) {
            size_t updated_way = k;
            for (size_t i = 0; i < ways; i++) {
                if (lru[idx * ways + i] == updated_way) {
                    // Shift all elements that are before the updated way down one
                    for (size_t j = i; j > 0; j--) {
                        lru[idx * ways + j] = lru[idx * ways + j - 1];
                    }
                    lru[idx * ways] = updated_way;
                    break;
                }
            }
            return &tags[idx * ways + k];
        }

    return NULL;
}

uint64_t fa_cache_sim_t::victimize(uint64_t addr) {
    size_t lru_way = lru[ways - 1];  // LRU way is at the end

    // Evict the least recently used tag
    uint64_t old_tag = tags[lru_way];
    tags[lru_way] = (addr >> idx_shift) | VALID;

    // Update the LRU order: move the evicted way to the front
    for (size_t i = ways - 1; i > 0; i--) {
        lru[i] = lru[i - 1];
    }
    lru[0] = lru_way;

    return old_tag;
}