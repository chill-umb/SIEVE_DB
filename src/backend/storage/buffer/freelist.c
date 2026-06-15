/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc.h"
#include "utils/guc_tables.h" 
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#include "storage/eviction.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))

int eviction_algorithm = CLOCK_ALGORITHM;

const struct config_enum_entry eviction_algorithm_options[] = {
    {"clock",    CLOCK_ALGORITHM,   false},
	{"sieve",    SIEVE_ALGORITHM,   false},
    {"sieve_db", SIEVE_DB_ALGORITHM,false},
    {"lru",      LRU_ALGORITHM,     false},
    {"cflru",    CFLRU_ALGORITHM,   false},
	{"lruwsr",   LRUWSR_ALGORITHM,  false},
    {NULL, 0, false}
};

/*
 * The shared freelist control information.
 */
typedef struct
{
	int         eviction_algorithm;

	/* Queue-based eviction */
	int     queueHead;
	int     queueTail;
	int     sieveHand;


	/* Spinlock: protects the values below */
	slock_t		buffer_strategy_lock;

	/*
	 * clock-sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 */
	pg_atomic_uint32 nextVictimBuffer;

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock-sweep */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int			bgwprocno;
	bool sieve_db_all_protected; // relaxed sieve_db which acts like sieve

} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

static inline uint32 ClockSweepTick(void);
static inline uint32 SieveTick(void);
void SieveOnBufferHit(BufferDesc *buf);
void SieveOnBufferInsert(BufferDesc *buf);
void SieveDBOnBufferHit(BufferDesc *buf);
void SieveDBOnBufferInsert(BufferDesc *buf);
void LRUOnBufferHit(BufferDesc *buf);


/* SIEVE and SIEVE_DB - return next candidate buffer id from SIEVE queue. */
static inline uint32
SieveTick(void)
{
    uint32      victim;
    BufferDesc *buf;

    /* Queue must not be empty */
    Assert(StrategyControl->queueTail != POINTER_NOT_IN_QUEUE);

    if (StrategyControl->sieveHand == POINTER_NOT_IN_QUEUE)
        StrategyControl->sieveHand = StrategyControl->queueTail;

    victim = (uint32) StrategyControl->sieveHand;

    buf = GetBufferDescriptor(victim);

    /* Move hand backwards (toward older buffers) */
    StrategyControl->sieveHand = buf->queuePrev;

    /* Wraparound => completed a pass */
    if (StrategyControl->sieveHand == POINTER_NOT_IN_QUEUE)
    {
        StrategyControl->completePasses++;
        StrategyControl->sieveHand = StrategyControl->queueTail;
    }

    return victim;
}

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

//add to MRU (head)
static void
AddBufferToQueue(BufferDesc *buf)
{
    /* Empty queue */
    if (StrategyControl->queueHead == POINTER_NOT_IN_QUEUE)
    {
        buf->queuePrev = POINTER_NOT_IN_QUEUE;
        buf->queueNext = POINTER_NOT_IN_QUEUE;

        StrategyControl->queueHead = buf->buf_id;
        StrategyControl->queueTail = buf->buf_id;
    }
    else
    {
        BufferDesc *oldHead;

        oldHead = GetBufferDescriptor(StrategyControl->queueHead);

        oldHead->queuePrev = buf->buf_id;
        buf->queueNext = StrategyControl->queueHead;
        buf->queuePrev = POINTER_NOT_IN_QUEUE;

        StrategyControl->queueHead = buf->buf_id;
    }
}

static void
RemoveBufferFromQueue(BufferDesc *buf)
{
	BufferDesc *prevBuf;
    BufferDesc *nextBuf;

	/* If not in queue, nothing to do */
	if (buf->queuePrev == POINTER_NOT_IN_QUEUE &&
    	buf->queueNext == POINTER_NOT_IN_QUEUE &&
    	StrategyControl->queueHead != buf->buf_id)   /* head is special case */
    	return;

    /* Case 1: buffer is in the middle */
    if (buf->queuePrev != POINTER_NOT_IN_QUEUE &&
        buf->queueNext != POINTER_NOT_IN_QUEUE)
    {
        prevBuf = GetBufferDescriptor(buf->queuePrev);
        nextBuf = GetBufferDescriptor(buf->queueNext);

        prevBuf->queueNext = buf->queueNext;
        nextBuf->queuePrev = buf->queuePrev;
    }
    /* Case 2: buffer is tail */
    else if (buf->queuePrev != POINTER_NOT_IN_QUEUE)
    {
        prevBuf = GetBufferDescriptor(buf->queuePrev);
        prevBuf->queueNext = POINTER_NOT_IN_QUEUE;
        StrategyControl->queueTail = buf->queuePrev;
    }
    /* Case 3: buffer is head */
    else if (buf->queueNext != POINTER_NOT_IN_QUEUE)
    {
        nextBuf = GetBufferDescriptor(buf->queueNext);
        nextBuf->queuePrev = POINTER_NOT_IN_QUEUE;
        StrategyControl->queueHead = buf->queueNext;
    }
    /* Case 4: only element */
    else
    {
        StrategyControl->queueHead = POINTER_NOT_IN_QUEUE;
        StrategyControl->queueTail = POINTER_NOT_IN_QUEUE;
        StrategyControl->sieveHand = POINTER_NOT_IN_QUEUE;
    }

    buf->queuePrev = POINTER_NOT_IN_QUEUE;
    buf->queueNext = POINTER_NOT_IN_QUEUE;
}

void
SieveDBOnBufferHit(BufferDesc *buf)
{
    uint32 buf_state;

    if (StrategyControl->eviction_algorithm != SIEVE_DB_ALGORITHM)
        return;

    buf_state = LockBufHdr(buf);
    (void) buf_state;

    /* Protect only on 2nd reuse */
    if (buf->sieve_visited)
        buf->sieve_protected = true;

    buf->sieve_visited = true;

    /* On any hit, reset “protected-but-unvisited” timer */
    buf->sieve_protect_unvisited_since_pass = 0;

    UnlockBufHdr(buf);
}

/*
 * SieveDBOnBufferInsert
 *
 * Called when a buffer is chosen as victim and is about to be reused for a
 * completely different page (i.e., "miss/new page allocation" path).
 *
 * Responsibilities:
 *   1) Reset SIEVE_DB bits so the new page doesn't inherit hotness.
 *   2) Place the buffer in the SIEVE queue in your chosen position.
 */
void
SieveDBOnBufferInsert(BufferDesc *buf)
{
    if (StrategyControl->eviction_algorithm != SIEVE_DB_ALGORITHM)
        return;

    SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

    /*
     * Victim becomes a “new page slot”:
     * - ensure it is present exactly once in the SIEVE queue
     * - reset SIEVE_DB metadata
     */
	/* Only remove if it is currently in queue */
    if (buf->queuePrev != POINTER_NOT_IN_QUEUE ||
        buf->queueNext != POINTER_NOT_IN_QUEUE ||
        StrategyControl->queueHead == buf->buf_id)
        RemoveBufferFromQueue(buf);

    AddBufferToQueue(buf);   /* AddBufferToQueue() adds at HEAD */

    /* new page -> forget previous hotness/protection */
    buf->sieve_visited = false;
    buf->sieve_protected = false;
	buf->sieve_protect_unvisited_since_pass = 0;

    SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

void
SieveOnBufferHit(BufferDesc *buf)
{
    if (StrategyControl->eviction_algorithm != SIEVE_ALGORITHM)
        return;

    LockBufHdr(buf);

    buf->visited = true;

    UnlockBufHdr(buf);
}

void
SieveOnBufferInsert(BufferDesc *buf)
{
    if (StrategyControl->eviction_algorithm != SIEVE_ALGORITHM)
        return;

    SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

    /* victim becomes the “new x”: must be inserted at head, visited=0 */
	/* Only remove if it is currently in queue */
	if (buf->queuePrev != POINTER_NOT_IN_QUEUE ||
		buf->queueNext != POINTER_NOT_IN_QUEUE ||
		StrategyControl->queueHead == buf->buf_id)
		RemoveBufferFromQueue(buf);
	
	AddBufferToQueue(buf);   /* AddBufferToQueue() adds at HEAD */
    buf->visited = false;

    SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * LRUOnBufferHit
 *
 * On a shared-buffer hit, move the buffer to MRU position in the LRU queue.
 * Only manipulates queue pointers under buffer_strategy_lock.
 *
 *  - This is called after PinBuffer() succeeded (so the buffer is valid/pinned).
 *  - queuePrev/queueNext are protected by StrategyControl->buffer_strategy_lock.
 */
void
LRUOnBufferHit(BufferDesc *buf)
{
	if (StrategyControl->eviction_algorithm != LRU_ALGORITHM && 
		StrategyControl->eviction_algorithm != CFLRU_ALGORITHM &&
		StrategyControl->eviction_algorithm != LRUWSR_ALGORITHM)
		return;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/* Already MRU/head? (Optional fast path; safe to omit) */
	if (StrategyControl->queueHead == buf->buf_id)
	{
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);
		return;
	}

	/*
	 * If buffer isn't currently in the queue, just enqueue it as MRU.
	 * (This can happen if your code temporarily removes buffers, or if
	 * you haven't yet inserted all buffers into the queue at init.)
	 */
	if (buf->queuePrev == POINTER_NOT_IN_QUEUE &&
		buf->queueNext == POINTER_NOT_IN_QUEUE)
	{
		AddBufferToQueue(buf);
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);
		return;
	}

	/* Move to MRU/head */
	RemoveBufferFromQueue(buf);
	AddBufferToQueue(buf);

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	GetVictimBuffer(). The only hard requirement GetVictimBuffer() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	It is the callers responsibility to ensure the buffer ownership can be
 *	tracked via TrackNewBufferPin().
 *
 *	The buffer is pinned and marked as owned, using TrackNewBufferPin(),
 *	before returning.
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	if (StrategyControl->eviction_algorithm == CLOCK_ALGORITHM)
	{
		/* Use the "clock sweep" algorithm to find a free buffer */
		trycounter = NBuffers;
		for (;;)
		{
			uint32		old_buf_state;
			uint32		local_buf_state;

			buf = GetBufferDescriptor(ClockSweepTick());

			/*
			* Check whether the buffer can be used and pin it if so. Do this
			* using a CAS loop, to avoid having to lock the buffer header.
			*/
			old_buf_state = pg_atomic_read_u32(&buf->state);
			for (;;)
			{
				local_buf_state = old_buf_state;

				/*
				* If the buffer is pinned or has a nonzero usage_count, we cannot
				* use it; decrement the usage_count (unless pinned) and keep
				* scanning.
				*/

				if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
				{
					if (--trycounter == 0)
					{
						/*
						* We've scanned all the buffers without making any state
						* changes, so all the buffers are pinned (or were when we
						* looked at them). We could hope that someone will free
						* one eventually, but it's probably better to fail than
						* to risk getting stuck in an infinite loop.
						*/
						elog(ERROR, "no unpinned buffers available");
					}
					break;
				}

				/* See equivalent code in PinBuffer() */
				if (unlikely(local_buf_state & BM_LOCKED))
				{
					old_buf_state = WaitBufHdrUnlocked(buf);
					continue;
				}

				if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
				{
					local_buf_state -= BUF_USAGECOUNT_ONE;

					if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
													local_buf_state))
					{
						trycounter = NBuffers;
						break;
					}
				}
				else
				{
					/* pin the buffer if the CAS succeeds */
					local_buf_state += BUF_REFCOUNT_ONE;

					if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
													local_buf_state))
					{
						/* Found a usable buffer */
						if (strategy != NULL)
							AddBufferToRing(strategy, buf);
						*buf_state = local_buf_state;

						TrackNewBufferPin(BufferDescriptorGetBuffer(buf));						
						return buf;
					}
				}
			}
		}
	}

	else if (StrategyControl->eviction_algorithm == SIEVE_ALGORITHM)
	{
		for (;;)
		{
			uint32 local_buf_state;

			/* Pick a candidate from SIEVE queue */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			buf = GetBufferDescriptor(SieveTick());
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/* Examine candidate buffer - MUST use lock to check/modify visited bit */
			local_buf_state = LockBufHdr(buf);

			/* Skip pinned buffers */
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			{
				UnlockBufHdr(buf);
				continue;
			}

			/*
			* SIEVE eviction logic:
			* - If visited: clear the bit and continue searching
			* - If not visited: this is our victim
			*/
			if (buf->visited)
			{
				/* Clear visited bit and give it a second chance */
				buf->visited = false;
				UnlockBufHdr(buf);
				continue;
			}

			/* === Victim selected === */
			/* Buffer is unpinned and unvisited - evict it */
			
			Assert(local_buf_state & BM_LOCKED);
			
			/* Pin the buffer (increment refcount to 1) and unlock */
			UnlockBufHdrExt(buf, local_buf_state, 0, 0, 1);

			/* Read final state after unlock */
			local_buf_state = pg_atomic_read_u32(&buf->state);
			*buf_state = local_buf_state;

			if (strategy != NULL)
				AddBufferToRing(strategy, buf);

			TrackNewBufferPin(BufferDescriptorGetBuffer(buf));
			return buf;
		}
	}
	else if (StrategyControl->eviction_algorithm == SIEVE_DB_ALGORITHM)
	{
		for (;;)
		{
			uint32 local_buf_state;
			uint64 cur_pass;

			/* Pick a candidate from SIEVE queue */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			cur_pass = StrategyControl->completePasses;
			buf = GetBufferDescriptor(SieveTick());
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/* Examine candidate buffer.	*/
			local_buf_state = LockBufHdr(buf);

			/* Skip pinned buffers */
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			{
				UnlockBufHdr(buf);
				continue;
			}

			/*
			* SIEVE_DB eviction sweep
			*/
			if (buf->sieve_visited)
			{
				buf->sieve_visited = false;

				/* If it's protected, it just became unvisited, start aging timer */
				if (buf->sieve_protected)
					buf->sieve_protect_unvisited_since_pass = cur_pass;

				UnlockBufHdr(buf);
				continue;
			}

			if (buf->sieve_protected)
			{
				/* Only age out protection if it's currently unvisited */
				if (!buf->sieve_visited)
				{
					if (buf->sieve_protect_unvisited_since_pass == 0)
						buf->sieve_protect_unvisited_since_pass = cur_pass;

					if (cur_pass - buf->sieve_protect_unvisited_since_pass >= 3)
					{
						buf->sieve_protected = false;
						buf->sieve_protect_unvisited_since_pass = 0;
						/* now it can fall through and be evictable soon */
					}
					else
					{
						UnlockBufHdr(buf);
						continue;
					}
				}
				else
				{
					/* If it’s visited again, stop the “unvisited protected” timer */
					buf->sieve_protect_unvisited_since_pass = 0;
					UnlockBufHdr(buf);
					continue;
				}
			}

			/* === Victim selected === */
			/*
			* We currently hold the buffer header lock (BM_LOCKED is set in local_buf_state).
			* Contract for GetVictimBuffer():
			*   - return buffer pinned (refcount==1)
			*   - header lock must be released before return
			*/
			/* Sanity: still unpinned */
			Assert(local_buf_state & BM_LOCKED);

			UnlockBufHdrExt(buf, local_buf_state, 0, 0, 1);

			local_buf_state = pg_atomic_read_u32(&buf->state);
			*buf_state = local_buf_state;

			if (strategy != NULL)
				AddBufferToRing(strategy, buf);

			TrackNewBufferPin(BufferDescriptorGetBuffer(buf));
			return buf;
		}
	}
	else if (StrategyControl->eviction_algorithm == LRU_ALGORITHM)
	{
		uint32  cur_idx;
		uint32  local_buf_state;

		trycounter = NBuffers;

		/* Start from LRU tail, under strategy lock */
		SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
		cur_idx = StrategyControl->queueTail;
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);

		for (;;)
		{
			BufferDesc *cur;
			uint32      next_idx;

			cur = GetBufferDescriptor(cur_idx);
			local_buf_state = LockBufHdr(cur);

			/* Unpinned candidate found */
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
			{
				/*
				* Promote to MRU: queue manipulation happens under
				* buffer_strategy_lock.
				*/
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				RemoveBufferFromQueue(cur);
				AddBufferToQueue(cur);
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				/* Upgrade this candidate into a pinned victim */
				UnlockBufHdrExt(cur, local_buf_state, 0, 0, 1);

				local_buf_state = pg_atomic_read_u32(&cur->state);
				/* Another backend may have BM_LOCKED again; just ensure we have a pin */
				Assert(BUF_STATE_GET_REFCOUNT(local_buf_state) >= 1);

				*buf_state = local_buf_state;

				if (strategy != NULL)
					AddBufferToRing(strategy, cur);

				TrackNewBufferPin(BufferDescriptorGetBuffer(cur));
				return cur;
			}

			/* pinned — skip */
			UnlockBufHdr(cur);

			if (--trycounter == 0)
				elog(ERROR, "no unpinned buffers available");

			/* Advance LRU-wards safely under strategy lock */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
				next_idx = StrategyControl->queueTail;
			else
				next_idx = cur->queuePrev;
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			cur_idx = next_idx;
		}
	}
	else if (StrategyControl->eviction_algorithm == CFLRU_ALGORITHM)
	{
		int     cold_scan = NBuffers / 3;
		uint32  cur_idx;
		BufferDesc *cur;
		uint32  local_buf_state;

		trycounter = NBuffers;

		/*
		* PHASE 1: scan the cold region (tail direction) but skip dirty pages.
		* Start from LRU tail, under strategy lock.
		*/
		SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
		cur_idx = StrategyControl->queueTail;
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);

		for (int i = 0; i < cold_scan; i++)
		{
			uint32 next_idx;

			cur = GetBufferDescriptor(cur_idx);
			local_buf_state = LockBufHdr(cur);

			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
			{
				/* Skip dirty pages in CFLRU cold region */
				if (local_buf_state & BM_DIRTY)
				{
					UnlockBufHdr(cur);

					/* advance to previous (LRU-wards) safely */
					SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
					if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
						next_idx = StrategyControl->queueTail;
					else
						next_idx = cur->queuePrev;
					SpinLockRelease(&StrategyControl->buffer_strategy_lock);

					cur_idx = next_idx;
					continue;
				}

				/* Unpinned clean candidate found */
				/* queue update: promote to MRU */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				RemoveBufferFromQueue(cur);
				AddBufferToQueue(cur);
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				/* upgrade to pinned + unlock header */
				UnlockBufHdrExt(cur, local_buf_state, 0, 0, 1);

				local_buf_state = pg_atomic_read_u32(&cur->state);
				Assert(BUF_STATE_GET_REFCOUNT(local_buf_state) >= 1);

				*buf_state = local_buf_state;

				if (strategy != NULL)
					AddBufferToRing(strategy, cur);

				TrackNewBufferPin(BufferDescriptorGetBuffer(cur));
				return cur;
			}

			/* pinned → skip */
			UnlockBufHdr(cur);

			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
				next_idx = StrategyControl->queueTail;
			else
				next_idx = cur->queuePrev;
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			cur_idx = next_idx;
		}

		/*
		* PHASE 2: fallback to full LRU scan if the cold region yielded nothing.
		*/
		SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
		cur_idx = StrategyControl->queueTail;
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);

		for (;;)
		{
			uint32 next_idx;

			cur = GetBufferDescriptor(cur_idx);
			local_buf_state = LockBufHdr(cur);

			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
			{
				/* promote to MRU */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				RemoveBufferFromQueue(cur);
				AddBufferToQueue(cur);
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				UnlockBufHdrExt(cur, local_buf_state, 0, 0, 1);

				local_buf_state = pg_atomic_read_u32(&cur->state);
				Assert(BUF_STATE_GET_REFCOUNT(local_buf_state) >= 1);

				*buf_state = local_buf_state;

				if (strategy != NULL)
					AddBufferToRing(strategy, cur);

				TrackNewBufferPin(BufferDescriptorGetBuffer(cur));
				return cur;
			}

			UnlockBufHdr(cur);

			if (--trycounter == 0)
				elog(ERROR, "no unpinned buffers available");

			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
				next_idx = StrategyControl->queueTail;
			else
				next_idx = cur->queuePrev;
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			cur_idx = next_idx;
		}
	}
	else if (StrategyControl->eviction_algorithm == LRUWSR_ALGORITHM)
	{
		uint32  cur_idx;
		uint32  local_buf_state;

		trycounter = NBuffers;

		/* Start from LRU tail, under strategy lock */
		SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
		cur_idx = StrategyControl->queueTail;
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);

		for (;;)
		{
			BufferDesc *cur;
			uint32      next_idx;

			cur = GetBufferDescriptor(cur_idx);
			local_buf_state = LockBufHdr(cur);

			// if (unlikely(local_buf_state & BM_LOCKED))
			// {
			// 	UnlockBufHdr(cur);
			// 	/* wait until unlocked, then retry same cur_idx */
			// 	(void) WaitBufHdrUnlocked(cur);
			// 	continue;
			// }

			/*
			* If pinned, we can't touch it (including WSR aging); just skip.
			*/
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			{
				UnlockBufHdr(cur);

				if (--trycounter == 0)
					elog(ERROR, "no unpinned buffers available");

				/* Advance LRU-wards safely under strategy lock */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
					next_idx = StrategyControl->queueTail;
				else
					next_idx = cur->queuePrev;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				cur_idx = next_idx;
				continue;
			}

			/*
			* WSR rule:
			* If buffer is dirty AND has usage_count > 0,
			* zero out usage_count and push toward MRU (aging), then continue.
			*/
			if ((local_buf_state & BM_TAG_VALID) &&
				(local_buf_state & BM_DIRTY) &&
				BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				/* Move on (from current's prev; recompute safely) */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				if (cur->queuePrev == POINTER_NOT_IN_QUEUE)
					next_idx = StrategyControl->queueTail;
				else
					next_idx = cur->queuePrev;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				/* Compute the new state: same, but usagecount -> 0 */
				//int uc = BUF_STATE_GET_USAGECOUNT(local_buf_state);

				/* Release header lock while setting usagecount delta */
				//UnlockBufHdrExt(cur, local_buf_state, 0, -uc, 0);

				/* zero usage_count safely */
				for (;;)
				{
					uint32 old_state = local_buf_state;
					uint32 old_uc = BUF_STATE_GET_USAGECOUNT(old_state);

					if (old_uc == 0)
						break;

					uint32 new_state =
						old_state -
						(old_uc * BUF_USAGECOUNT_ONE);

					if (pg_atomic_compare_exchange_u32(&cur->state,
													&old_state,
													new_state))
						break;

					local_buf_state = old_state;
				}

				UnlockBufHdr(cur);

				/* Promote to MRU under strategy lock */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
				RemoveBufferFromQueue(cur);
				AddBufferToQueue(cur);
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);

				/* count this as "made progress" like clock does */
				trycounter = NBuffers;

				cur_idx = next_idx;
				continue;
			}
	

			/*
			* Unpinned candidate found (and either clean or usagecount==0).
			*/

			/* promote to MRU */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
			RemoveBufferFromQueue(cur);
			AddBufferToQueue(cur);
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/* Pin and return */
			UnlockBufHdrExt(cur, local_buf_state, 0, 0, 1);

			local_buf_state = pg_atomic_read_u32(&cur->state);
			Assert(BUF_STATE_GET_REFCOUNT(local_buf_state) >= 1);

			*buf_state = local_buf_state;

			if (strategy != NULL)
				AddBufferToRing(strategy, cur);

			TrackNewBufferPin(BufferDescriptorGetBuffer(cur));
			return cur;

		}
	}
	else
	{
		elog(ERROR, "unknown eviction algorithm");
	}
}

/*
 * StrategySyncStart -- tell BgBufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BgBufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/* Copy enum GUC directly */
    	StrategyControl->eviction_algorithm = eviction_algorithm;

		/* SIEVE and LRU initial state (flags, counters, queues, etc.) */
		StrategyControl->queueHead = POINTER_NOT_IN_QUEUE;
		StrategyControl->queueTail = POINTER_NOT_IN_QUEUE;
		StrategyControl->sieveHand = POINTER_NOT_IN_QUEUE;

		/* SIEVE_DB initial state (flags, counters, queues, etc.) */
		SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
		StrategyControl->sieve_db_all_protected = false;
		SpinLockRelease(&StrategyControl->buffer_strategy_lock);

		/* Initialize the clock-sweep pointer */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* Clear statistics */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* No pending notification */
		StrategyControl->bgwprocno = -1;

		for (int i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);
			buf->queuePrev = POINTER_NOT_IN_QUEUE;
			buf->queueNext = POINTER_NOT_IN_QUEUE;
			buf->visited   = false;
			AddBufferToQueue(buf);
		}
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			{
				int			ring_max_kb;

				/*
				 * The ring always needs to be large enough to allow some
				 * separation in time between providing a buffer to the user
				 * of the strategy and that buffer being reused. Otherwise the
				 * user's pin will prevent reuse of the buffer, even without
				 * concurrent activity.
				 *
				 * We also need to ensure the ring always is large enough for
				 * SYNC_SCAN_REPORT_INTERVAL, as noted above.
				 *
				 * Thus we start out a minimal size and increase the size
				 * further if appropriate.
				 */
				ring_size_kb = 256;

				/*
				 * There's no point in a larger ring if we won't be allowed to
				 * pin sufficiently many buffers.  But we never limit to less
				 * than the minimal size above.
				 */
				ring_max_kb = GetPinLimit() * (BLCKSZ / 1024);
				ring_max_kb = Max(ring_size_kb, ring_max_kb);

				/*
				 * We would like the ring to additionally have space for the
				 * configured degree of IO concurrency. While being read in,
				 * buffers can obviously not yet be reused.
				 *
				 * Each IO can be up to io_combine_limit blocks large, and we
				 * want to start up to effective_io_concurrency IOs.
				 *
				 * Note that effective_io_concurrency may be 0, which disables
				 * AIO.
				 */
				ring_size_kb += (BLCKSZ / 1024) *
					io_combine_limit * effective_io_concurrency;

				if (ring_size_kb > ring_max_kb)
					ring_size_kb = ring_max_kb;
				break;
			}
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 2048;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit -- get cap of number of buffers that should be pinned
 *
 * When pinning extra buffers to look ahead, users of a ring-based strategy are
 * in danger of pinning too much of the ring at once while performing look-ahead.
 * For some strategies, that means "escaping" from the ring, and in others it
 * means forcing dirty data to disk very frequently with associated WAL
 * flushing.  Since external code has no insight into any of that, allow
 * individual strategy types to expose a clamp that should be applied when
 * deciding on a maximum number of buffers to pin at once.
 *
 * Callers should combine this number with other relevant limits and take the
 * minimum.
 */
int
GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
		case BAS_BULKREAD:

			/*
			 * Since BAS_BULKREAD uses StrategyRejectBuffer(), dirty buffers
			 * shouldn't be a problem and the caller is free to pin up to the
			 * entire ring at once.
			 */
			return strategy->nbuffers;

		default:

			/*
			 * Tell caller not to pin more than half the buffers in the ring.
			 * This is a trade-off between look ahead distance and deferring
			 * writeback and associated WAL traffic.
			 */
			return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The buffer is pinned and marked as owned, using TrackNewBufferPin(), before
 * returning.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		old_buf_state;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	buf = GetBufferDescriptor(bufnum - 1);

	/*
	 * Check whether the buffer can be used and pin it if so. Do this using a
	 * CAS loop, to avoid having to lock the buffer header.
	 */
	old_buf_state = pg_atomic_read_u32(&buf->state);
	for (;;)
	{
		local_buf_state = old_buf_state;

		/*
		 * If the buffer is pinned we cannot use it under any circumstances.
		 *
		 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
		 * since our own previous usage of the ring element would have left it
		 * there, but it might've been decremented by clock-sweep since then).
		 * A higher usage_count indicates someone else has touched the buffer,
		 * so we shouldn't re-use it.
		 */
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0
			|| BUF_STATE_GET_USAGECOUNT(local_buf_state) > 1)
			break;

		/* See equivalent code in PinBuffer() */
		if (unlikely(local_buf_state & BM_LOCKED))
		{
			old_buf_state = WaitBufHdrUnlocked(buf);
			continue;
		}

		/* pin the buffer if the CAS succeeds */
		local_buf_state += BUF_REFCOUNT_ONE;

		if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
										   local_buf_state))
		{
			*buf_state = local_buf_state;

			TrackNewBufferPin(BufferDescriptorGetBuffer(buf));
			return buf;
		}
	}

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}