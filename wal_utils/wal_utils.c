/*-------------------------------------------------------------------------
 *
 * wal_utils.c
 *		Set of tools and utilities for handling of WAL-related data.
 *
 * Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  wal_utils/wal_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

static List *parseTimeLineHistory(char *buffer);

/*
 * Set of SQL-callable functions.
 */
PG_FUNCTION_INFO_V1(parse_wal_history);
PG_FUNCTION_INFO_V1(build_wal_segment_list);

/*
 * parseTimeLineHistory
 *
 * Using in input a buffer including a complete history file, parse its
 * data and return a list of TimeLineHistoryEntry entries filled correctly
 * with the data of the file.
 */
static List *
parseTimeLineHistory(char *buffer)
{
	char	   *fline;
	List	   *entries = NIL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * Parse the file...
	 */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline)
	{
		char	   *ptr;
		TimeLineHistoryEntry *entry;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		fline = bufptr;
		while (*bufptr && *bufptr != '\n')
			bufptr++;
		if (!(*bufptr))
			lastline = true;
		else
			*bufptr++ = '\0';

		/* skip leading whitespace and check for # comment */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			ereport(ERROR,
					(errmsg("syntax error in history file: %s", fline),
					 errhint("Expected a numeric timeline ID.")));
		}
		if (nfields != 3)
		{
			ereport(ERROR,
					(errmsg("syntax error in history file: %s", fline),
					 errhint("Expected a write-ahead log switchpoint location.")));
		}
		if (tli <= lasttli)
			ereport(ERROR,
					(errmsg("invalid data in history file: %s", fline),
					 errhint("Timeline IDs must be in increasing sequence.")));

		lasttli = tli;

		nlines++;

		entry = palloc(sizeof(TimeLineHistoryEntry));
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;

		entries = lappend(entries, entry);

		/* we ignore the remainder of each line */
	}

	return entries;
}


/*
 * parse_wal_history
 *
 * Parse input buffer of a history file and build a set of rows to
 * give a SQL representation of TimeLineHistoryEntry entries part
 * of a timeline history file.
 */
Datum
parse_wal_history(PG_FUNCTION_ARGS)
{
	char	   *history_buf = TextDatumGetCString(PG_GETARG_DATUM(0));
	TupleDesc   tupdesc;
	Tuplestorestate *tupstore;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	List	   *entries = NIL;
	ListCell   *entry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/* parse the history file */
	entries = parseTimeLineHistory(history_buf);

	/* represent its data as a set of tuples */
	foreach(entry, entries)
	{
		Datum		values[3];
		bool		nulls[3];
		TimeLineHistoryEntry *history = (TimeLineHistoryEntry *) lfirst(entry);

		/* Initialize values and NULL flags arrays */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* timeline number */
		values[0] = Int32GetDatum(history->tli);

		/* begin position */
		if (XLogRecPtrIsInvalid(history->begin))
			nulls[1] = true;
		else
			values[1] = LSNGetDatum(history->begin);

		/* end position */
		if (XLogRecPtrIsInvalid(history->end))
			nulls[2] = true;
		else
			values[2] = LSNGetDatum(history->end);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * build_wal_segment_list
 *
 * Taking in input an origin timeline and LSN, as well as a target timeline
 * and LSN, build a list of WAL segments able to allow a standby pointing to
 * the origin timeline to reach the target timeline.
 *
 * Note that the origin and the target timelines need to be direct parents,
 * and user needs to provide in input a buffer corresponding to a history
 * file in text format, on which is performed a set of tests, checking for
 * timeline jumps to build the correct list of segments to join the origin
 * and the target.
 *
 * The target timeline needs normally to match the history file name given
 * in input, but this is let up to the user to combine both correctly for
 * flexibility, still this routine checks if the target LSN is newer than
 * the last entry in the history file, as well as it checks if the last
 * timeline entry is higher than the target.
 */
Datum
build_wal_segment_list(PG_FUNCTION_ARGS)
{
	TimeLineID	origin_tli = PG_GETARG_INT32(0);
	XLogRecPtr	origin_lsn = PG_GETARG_LSN(1);
	TimeLineID	target_tli = PG_GETARG_INT32(2);
	XLogRecPtr	target_lsn = PG_GETARG_LSN(3);
	char	   *history_buf = TextDatumGetCString(PG_GETARG_DATUM(4));
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	TupleDesc		tupdesc;
	Tuplestorestate *tupstore;
	List		   *entries;
	ListCell	   *entry;
	TimeLineHistoryEntry *history;
	bool			history_match = false;
	XLogRecPtr		current_seg_lsn;
	TimeLineID		current_tli;
	char			xlogfname[MAXFNAMELEN];
	Datum			values[1];
	bool			nulls[1];
	XLogSegNo		logSegNo;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupdesc = CreateTemplateTupleDesc(1, false);

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "wal_segs", TEXTOID, -1, 0);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* First do sanity checks on target and origin data */
	if (origin_lsn > target_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("origin LSN %X/%X newer than target LSN %X/%X",
						(uint32) (origin_lsn >> 32),
						(uint32) origin_lsn,
						(uint32) (target_lsn >> 32),
						(uint32) target_lsn)));
	if (origin_tli > target_tli)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("origin timeline %u newer than target timeline %u",
						origin_tli, target_tli)));

	/* parse the history file */
	entries = parseTimeLineHistory(history_buf);

	/*
	 * Check that the target data is newer than the last entry in the history
	 * file. Better safe than sorry.
	 */
	history = (TimeLineHistoryEntry *) llast(entries);
	if (history->tli >= target_tli)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timeline of last history entry %u newer than or "
						"equal to target timeline %u",
						history->tli, target_tli)));
	if (history->end > target_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("LSN %X/%X of last history entry newer than target LSN %X/%X",
						(uint32) (history->end >> 32),
						(uint32) history->end,
						(uint32) (target_lsn >> 32),
						(uint32) target_lsn)));

	/*
	 * Check that origin and target are direct parents, we already know that
	 * the target fits with the history file.
	 */
	foreach(entry, entries)
	{
		history = (TimeLineHistoryEntry *) lfirst(entry);

		if (history->begin <= origin_lsn &&
			history->end >= origin_lsn &&
			history->tli == origin_tli)
		{
			history_match = true;
			break;
		}
	}

	if (!history_match)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("origin data not a direct parent of target")));

	/*
	 * Before listing the list of files, add a last history entry using the
	 * target data, this simplifies the logic below to build the segment list.
	 */
	current_seg_lsn = history->end;	/* abuse this variable as temporary
									 * storage */
	history = (TimeLineHistoryEntry *) palloc(sizeof(TimeLineHistoryEntry));
	history->tli = target_tli;
	history->begin = current_seg_lsn;
	history->end = target_lsn;
	entries = lappend(entries, history);

	/*
	 * Fill in the data by finding all segments between the origin and the
	 * target. First segment is the one of the origin LSN, with origin
	 * timeline. Note that when jumping to a new timeline, Postgres
	 * switches immediately to a new segment with the new timeline, giving
	 * up on the last, partial segment.
	 */

	/* Begin tracking at the beginning of the next segment */
	current_seg_lsn = origin_lsn + XLOG_SEG_SIZE;
	current_seg_lsn -= current_seg_lsn % XLOG_SEG_SIZE;
	current_tli = origin_tli;

	foreach(entry, entries)
	{
		history = (TimeLineHistoryEntry *) lfirst(entry);

		current_tli = history->tli;

		/* save the segment value */
		while (current_seg_lsn >= history->begin &&
			   current_seg_lsn < history->end)
		{
			XLByteToPrevSeg(current_seg_lsn, logSegNo);
			XLogFileName(xlogfname, current_tli, logSegNo);
			nulls[0] = false;
			values[0] = CStringGetTextDatum(xlogfname);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);

			/*
			 * Add equivalent of one segment, and just track the beginning
			 * of it.
			 */
			current_seg_lsn += XLOG_SEG_SIZE;
			current_seg_lsn -= current_seg_lsn % XLOG_SEG_SIZE;
		}
	}

	/*
	 * Add as well the last segment possible, this is needed to reach
	 * consistency up to the target point.
	 */
	XLByteToPrevSeg(target_lsn, logSegNo);
	XLogFileName(xlogfname, target_tli, logSegNo);
	nulls[0] = false;
	values[0] = CStringGetTextDatum(xlogfname);
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
