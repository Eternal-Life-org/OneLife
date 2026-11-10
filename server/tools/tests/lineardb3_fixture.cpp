// Test oracle: create and verify exported files through the server's real DB API.
#include "lineardb3.h"
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    bool write = std::strcmp(argv[1], "write") == 0;
    unsigned int keySize = std::atoi(argv[3]);
    unsigned int valueSize = std::atoi(argv[4]);
    FILE *expected = std::fopen(argv[5], "rb");
    if (!expected) return 3;
    LINEARDB3 db;
    if (LINEARDB3_open(&db, argv[2], 0, 2, keySize, valueSize)) return 4;
    std::vector<unsigned char> key(keySize), value(valueSize), actual(valueSize);
    unsigned int records = 0;
    while (std::fread(key.data(), keySize, 1, expected) == 1) {
        if (std::fread(value.data(), valueSize, 1, expected) != 1) return 5;
        if (write) {
            if (LINEARDB3_put(&db, key.data(), value.data())) return 6;
        } else if (LINEARDB3_get(&db, key.data(), actual.data()) || actual != value) {
            return 7;
        }
        records++;
    }
    if (!write && LINEARDB3_getNumRecords(&db) != records) return 8;
    LINEARDB3_close(&db);
    std::fclose(expected);
    return 0;
}
