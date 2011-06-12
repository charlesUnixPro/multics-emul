#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>

#include "hw6180.h"
#include "bitstream.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <file>\n", argv[0]);
        exit(1);
    }
    const char *fname = argv[1];

    bitstream_t* bp = bitstm_open(fname);
    if (bp == NULL) {
        fprintf(stderr, "Cannot open/map %s.\n", fname);
        exit(1);
    }
    t_uint64 word;
    while (bitstm_get(bp, 9, &word) == 0) {
        unsigned char c = word & 0xff;
        putchar(c);
    }
    return 0;
}