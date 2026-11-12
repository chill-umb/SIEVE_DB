#ifndef EVICTION_H
#define EVICTION_H

#include "utils/guc.h"
#include "storage/buf_internals.h"

/* Enum values MUST be visible to guc_tables.c */
enum EvictionAlgorithm
{
    CLOCK_ALGORITHM = 0,       /* each policy gets a number: clock = 0 */
                               /* and each line after it adds 1*/
    SIEVE_ALGORITHM,
    SIEVE_DB_ALGORITHM,
    LRU_ALGORITHM,
    CFLRU_ALGORITHM,
    LRUWSR_ALGORITHM,
    SIEVE_PROTECTED_ALGORITHM, /* SIEVE + protection bit, no aging */
    CLOCK_2BIT_ALGORITHM,      /* CLOCK with a 2-bit (0-3) reference count */
    TWOQ_ALGORITHM             /* Our policy; it gets the next number (8) */
};

#define CLOCK_2BIT_MAX_USAGE_COUNT 3
/* 2Q list membership values */
#define TWOQ_NONE 0
#define TWOQ_A1IN 1
#define TWOQ_AM 2

extern void SieveOnBufferHit(BufferDesc *buf);
extern void SieveDBOnBufferHit(BufferDesc *buf);
extern void SieveOnBufferInsert(BufferDesc *buf);
extern void SieveDBOnBufferInsert(BufferDesc *buf);
extern void LRUOnBufferHit(BufferDesc *buf);

extern void SieveProtectedOnBufferHit(BufferDesc *buf);
extern void SieveProtectedOnBufferInsert(BufferDesc *buf);
extern int  GetUsageCountCap(void);

/* GUC backing variable */
extern int eviction_algorithm;

/* Enum options table */
extern const struct config_enum_entry eviction_algorithm_options[];

#endif /* EVICTION_H */
