#ifndef EVICTION_H
#define EVICTION_H

#include "utils/guc.h"
#include "storage/buf_internals.h"

/* Enum values MUST be visible to guc_tables.c */
enum EvictionAlgorithm
{
    CLOCK_ALGORITHM = 0,
    SIEVE_ALGORITHM,
    SIEVE_DB_ALGORITHM,
    LRU_ALGORITHM,
    CFLRU_ALGORITHM,
    LRUWSR_ALGORITHM
};

extern void SieveOnBufferHit(BufferDesc *buf);
extern void SieveDBOnBufferHit(BufferDesc *buf);
extern void SieveOnBufferInsert(BufferDesc *buf);
extern void SieveDBOnBufferInsert(BufferDesc *buf);
extern void LRUOnBufferHit(BufferDesc *buf);

/* GUC backing variable */
extern int eviction_algorithm;

/* Enum options table */
extern const struct config_enum_entry eviction_algorithm_options[];

#endif /* EVICTION_H */
