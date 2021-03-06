/*-------------------------------------------------------------------------
 *
 * blvacuum.c
 *		Bloom VACUUM functions.
 *
 * Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blvacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"

#include "bloom.h"

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
blbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno,
				npages;
	FreeBlockNumberArray notFullPage;
	int			countPage = 0;
	BloomState	state;
	Buffer		buffer;
	Page		page;
	GenericXLogState *gxlogState;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	initBloomState(&state, index);

	/*
	 * Interate over the pages. We don't care about concurrently added pages,
	 * they can't contain tuples to delete.
	 */
	npages = RelationGetNumberOfBlocks(index);
	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		BloomTuple *itup,
				   *itupPtr,
				   *itupEnd;

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		gxlogState = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

		if (BloomPageIsDeleted(page))
		{
			UnlockReleaseBuffer(buffer);
			GenericXLogAbort(gxlogState);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* Iterate over the tuples */
		itup = itupPtr = BloomPageGetTuple(&state, page, FirstOffsetNumber);
		itupEnd = BloomPageGetTuple(&state, page,
							  OffsetNumberNext(BloomPageGetMaxOffset(page)));
		while (itup < itupEnd)
		{
			/* Do we have to delete this tuple? */
			if (callback(&itup->heapPtr, callback_state))
			{
				stats->tuples_removed += 1;
				BloomPageGetOpaque(page)->maxoff--;
			}
			else
			{
				if (itupPtr != itup)
				{
					/*
					 * If we already delete something before, we have to move
					 * this tuple backward.
					 */
					memmove((Pointer) itupPtr, (Pointer) itup,
							state.sizeOfBloomTuple);
				}
				stats->num_index_tuples++;
				itupPtr = BloomPageGetNextTuple(&state, itupPtr);
			}

			itup = BloomPageGetNextTuple(&state, itup);
		}

		Assert(itupPtr == BloomPageGetTuple(&state, page,
							 OffsetNumberNext(BloomPageGetMaxOffset(page))));

		/*
		 * Add page to notFullPage list if we will not mark page as deleted
		 * and there is a free space on it
		 */
		if (BloomPageGetMaxOffset(page) != 0 &&
			BloomPageGetFreeSpace(&state, page) > state.sizeOfBloomTuple &&
			countPage < BloomMetaBlockN)
			notFullPage[countPage++] = blkno;

		/* Did we delete something? */
		if (itupPtr != itup)
		{
			/* Is it empty page now? */
			if (BloomPageGetMaxOffset(page) == 0)
				BloomPageSetDeleted(page);
			/* Adjust pg_lower */
			((PageHeader) page)->pd_lower = (Pointer) itupPtr - page;
			/* Finish WAL-logging */
			GenericXLogFinish(gxlogState);
		}
		else
		{
			/* Didn't change anything: abort WAL-logging */
			GenericXLogAbort(gxlogState);
		}
		UnlockReleaseBuffer(buffer);
		CHECK_FOR_INTERRUPTS();
	}

	if (countPage > 0)
	{
		BloomMetaPageData *metaData;

		buffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		gxlogState = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

		metaData = BloomPageGetMeta(page);
		memcpy(metaData->notFullPage, notFullPage, sizeof(BlockNumber) * countPage);
		metaData->nStart = 0;
		metaData->nEnd = countPage;

		GenericXLogFinish(gxlogState);
		UnlockReleaseBuffer(buffer);
	}

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
blvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	BlockNumber npages,
				blkno;
	BlockNumber totFreePages;

	if (info->analyze_only)
		return stats;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/*
	 * Iterate over the pages: insert deleted pages into FSM and collect
	 * statistics.
	 */
	npages = RelationGetNumberOfBlocks(index);
	totFreePages = 0;
	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point();

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (BloomPageIsDeleted(page))
		{
			RecordFreeIndexPage(index, blkno);
			totFreePages++;
		}
		else
		{
			stats->num_index_tuples += BloomPageGetMaxOffset(page);
			stats->estimated_count += BloomPageGetMaxOffset(page);
		}

		UnlockReleaseBuffer(buffer);
	}

	IndexFreeSpaceMapVacuum(info->index);
	stats->pages_free = totFreePages;
	stats->num_pages = RelationGetNumberOfBlocks(index);

	return stats;
}
