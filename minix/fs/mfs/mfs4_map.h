#ifndef _MFS4_MAP_H
#define _MFS4_MAP_H

#include <stdint.h>

#define MFS4_MAP_DIRECT_COUNT 7U
#define MFS4_MAP_POINTERS 1024U
#define MFS4_MAP_MAX_BLOCKS (7ULL + 1024ULL + 1024ULL * 1024ULL + \
    1024ULL * 1024ULL * 1024ULL)

#endif
