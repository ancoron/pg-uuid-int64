/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>

#include "postgres.h"

#include "access/hash.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "uuid_int64.h"

PG_MODULE_MAGIC;

/*
 * The time offset between the UUID timestamp and the PostgreSQL epoch in
 * microsecond precision.
 *
 * This constant is the result of the following expression:
 * `122192928000000000 / 10 + ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * USECS_PER_SEC)`
 */
#define PG_UUID_OFFSET              INT64CONST(13165977600000000)

#define UUID_VERSION(uuid)              ((uuid->first >> 60) & 0x0F)
#define UUID_VARIANT_IS_NCS(uuid)       (((uuid->second >> 56) & 0x80) == 0x00)
#define UUID_VARIANT_IS_RFC4122(uuid)   (((uuid->second >> 56) & 0xC0) == 0x80)
#define UUID_VARIANT_IS_GUID(uuid)      (((uuid->second >> 56) & 0xE0) == 0xC0)
#define UUID_VARIANT_IS_FUTURE(uuid)    (((uuid->second >> 56) & 0xE0) == 0xE0)

#define UUID_V1_MSB_UNSCHUFFLE(msb)		(((msb << 48) & 0xFFFF000000000000) | ((msb << 16) & 0x0000FFFF00000000) | ((msb >> 32) & 0x00000000FFFFFFFF))
#define UUID_V1_MSB_SCHUFFLE(msb)		(((msb >> 48) & 0x000000000000FFFF) | ((msb >> 16) & 0x00000000FFFF0000) | ((msb << 32) & 0xFFFFFFFF00000000))

/* sortsupport for uuid */
typedef struct
{
	int64 input_count; /* number of non-null values seen */
	bool estimating; /* true if estimating cardinality */

	hyperLogLogState abbr_card; /* cardinality estimator */
} uuid_int64_sortsupport_state;

static void parse_uuid_int64(const char *source, pg_uuid_int64 *uuid);
static char* uuid_int64_to_string(pg_uuid_int64 *uuid);

static bool uuid_is_rfc_v1(const pg_uuid_int64 *uuid);
static int64 uuid_int64_timestamp0(const pg_uuid_int64 *uuid);
static TimestampTz uuid_int64_timestamptz(const pg_uuid_int64 *uuid);

static int uuid_int64_cmp0(const pg_uuid_int64 *a, const pg_uuid_int64 *b);
static int uuid_v1_cmp_abbrev(Datum x, Datum y, SortSupport ssup);
static bool uuid_v1_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum uuid_int64_abbrev_convert(Datum original, SortSupport ssup);
static int uuid_int64_sort_cmp(Datum x, Datum y, SortSupport ssup);

PG_FUNCTION_INFO_V1(uuid_int64_in);
PG_FUNCTION_INFO_V1(uuid_int64_out);
PG_FUNCTION_INFO_V1(uuid_int64_recv);
PG_FUNCTION_INFO_V1(uuid_int64_send);

PG_FUNCTION_INFO_V1(uuid_int64_conv_from_std);
PG_FUNCTION_INFO_V1(uuid_int64_conv_to_std);

PG_FUNCTION_INFO_V1(uuid_int64_timestamp);

PG_FUNCTION_INFO_V1(uuid_int64_sortsupport);

PG_FUNCTION_INFO_V1(uuid_int64_cmp);
PG_FUNCTION_INFO_V1(uuid_int64_eq);
PG_FUNCTION_INFO_V1(uuid_int64_ne);
PG_FUNCTION_INFO_V1(uuid_int64_lt);
PG_FUNCTION_INFO_V1(uuid_int64_le);
PG_FUNCTION_INFO_V1(uuid_int64_gt);
PG_FUNCTION_INFO_V1(uuid_int64_ge);

Datum
uuid_int64_in(PG_FUNCTION_ARGS)
{
	char *uuid_str = PG_GETARG_CSTRING(0);
	pg_uuid_int64 *uuid;

	uuid = (pg_uuid_int64 *) palloc(UUID_LEN);
	parse_uuid_int64(uuid_str, uuid);
	PG_RETURN_UUID_P(uuid);
}

Datum
uuid_int64_out(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *uuid = PG_GETARG_UUID64_P(0);

	PG_RETURN_CSTRING(uuid_int64_to_string(uuid));
}

static char*
uuid_int64_to_string(pg_uuid_int64 *uuid)
{
	static const char hex_chars[] = "0123456789abcdef";
	char* str;
	int i, out;
	unsigned char* bytes;
	uint64 tmp;
	uint8 hi, lo;

	str = (char*) palloc(37);
	out = 0;

	/* we need to reverse the version 1 optimization here */
	tmp = pg_hton64(UUID_V1_MSB_SCHUFFLE(uuid->first));
	bytes = (unsigned char*) &tmp;

	for (i = 0; i < 4; i++)
	{
		hi = bytes[i] >> 4;
		lo = bytes[i] & 0x0F;

		str[out++] = hex_chars[hi];
		str[out++] = hex_chars[lo];
	}

	str[out++] = '-';

	for (i = 4; i < 6; i++)
	{
		hi = bytes[i] >> 4;
		lo = bytes[i] & 0x0F;

		str[out++] = hex_chars[hi];
		str[out++] = hex_chars[lo];
	}

	str[out++] = '-';

	for (i = 6; i < 8; i++)
	{
		hi = bytes[i] >> 4;
		lo = bytes[i] & 0x0F;

		str[out++] = hex_chars[hi];
		str[out++] = hex_chars[lo];
	}

	str[out++] = '-';

	tmp = pg_hton64(uuid->second);
	bytes = (unsigned char*) &tmp;

	for (i = 0; i < 2; i++)
	{
		hi = bytes[i] >> 4;
		lo = bytes[i] & 0x0F;

		str[out++] = hex_chars[hi];
		str[out++] = hex_chars[lo];
	}

	str[out++] = '-';

	for (i = 2; i < 8; i++)
	{
		hi = bytes[i] >> 4;
		lo = bytes[i] & 0x0F;

		str[out++] = hex_chars[hi];
		str[out++] = hex_chars[lo];
	}

	str[36] = '\0';

	return str;
}

static void
parse_uuid_int64(const char *source, pg_uuid_int64 *uuid)
{
	/* reverse bitmask for allowed positions of the hyphen character */
	static const int32 hyphen_allowed = 0b00000000000100010001000100000000;

	/* mapping of the ASCII character to the hexadecimal value */
	static const int8 hex_to_int8[256] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -3, -1, -1,
		 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
	};
	const char *src = source;
	uint64 tmp;
	int8 val;
	int i;

	tmp = 0;
	val = 0;

	if (src[0] == '{')
		src++;

	for (i = 0; i < 32 && *src;)
	{
		/* read next character and map it */
		val = hex_to_int8[(uint8) *src++];

		if (val > -1) {
			/* a valid hexadecimal character found and mapped */
			tmp = (tmp << 4) | val;
			i++;
			if (i == 16) {
				/* we optimize for version 1 timestamp UUID's here */
				uuid->first = UUID_V1_MSB_UNSCHUFFLE(tmp);
				tmp = 0;
			} else if (i == 32) {
				uuid->second = tmp;
				break;
			}
		}
		else if (val == -3 && (hyphen_allowed >> i) & 0b1)
			/* check for '-' character at expected positions, fail otherwise */
			continue;
		else
			goto syntax_error;
	}

	/* not enough input... */
	if (i < 32)
		goto syntax_error;

	return;

syntax_error:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			errmsg("invalid input syntax for type %s: \"%s\"",
			"uuid_int64", source)));
}

Datum
uuid_int64_recv(PG_FUNCTION_ARGS)
{
	StringInfo buffer = (StringInfo) PG_GETARG_POINTER(0);
	pg_uuid_int64 *uuid;

	uuid = (pg_uuid_int64 *) palloc(UUID_LEN);
	uuid->first = (uint64) pq_getmsgint64(buffer);
	uuid->second = (uint64) pq_getmsgint64(buffer);
	PG_RETURN_POINTER(uuid);
}

Datum
uuid_int64_send(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *uuid = PG_GETARG_UUID64_P(0);
	StringInfoData buffer;

	pq_begintypsend(&buffer);
	pq_sendint64(&buffer, uuid->first);
	pq_sendint64(&buffer, uuid->second);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

Datum
uuid_int64_conv_from_std(PG_FUNCTION_ARGS)
{
	pg_uuid_t *input = PG_GETARG_UUID_P(0);
	pg_uuid_int64 *output;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	output = uuid_std_to_64(input);

	PG_RETURN_UUID64_P(output);
}

Datum
uuid_int64_conv_to_std(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *input = PG_GETARG_UUID64_P(0);
	pg_uuid_t *output;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	output = uuid_64_to_std(input);

	PG_RETURN_UUID_P(output);
}

static bool
uuid_is_rfc_v1(const pg_uuid_int64 *uuid)
{
	return 1 == UUID_VERSION(uuid) && UUID_VARIANT_IS_RFC4122(uuid);
}

static int64
uuid_int64_timestamp0(const pg_uuid_int64 *uuid)
{
	/* we just need to strip off the version bits */
	return (int64) (uuid->first & 0x0FFFFFFFFFFFFFFF);
}

/*
 * uuid_v1_timestamptz
 *	Extract the timestamp from a version 1 UUID.
 */
static TimestampTz
uuid_int64_timestamptz(const pg_uuid_int64 *uuid)
{
	/* from 100 ns precision to PostgreSQL epoch */
	TimestampTz timestamp = uuid_int64_timestamp0(uuid) / 10 - PG_UUID_OFFSET;

	return timestamp;
}

/*
 * uuid_v1_timestamp
 *	extract the timestamp of a version 1 UUID
 *
 */
Datum
uuid_int64_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = 0L;
	pg_uuid_int64 *uuid = PG_GETARG_UUID64_P(0);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	/* version and variant check */
	if (!uuid_is_rfc_v1(uuid))
		PG_RETURN_NULL();

	timestamp = uuid_int64_timestamptz(uuid);

	/* Recheck in case roundoff produces something just out of range */
	if (!IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(timestamp);
}


/*
 * uuid_std_to_64
 *	Convert a standard UUID into a UUID int64, if possible.
 */
pg_uuid_int64*
uuid_std_to_64(const pg_uuid_t *uuid)
{
	pg_uuid_int64 *uuid_64;

	const unsigned char *data = &(uuid->data)[0];

	uuid_64 = (pg_uuid_int64 *) palloc(UUID_LEN);

	uuid_64->first = UUID_V1_MSB_UNSCHUFFLE(pg_ntoh64(*(uint64 *) data));
	uuid_64->second = pg_ntoh64(*(uint64 *) (data + 8));

	return uuid_64;
}

pg_uuid_t*
uuid_64_to_std(const pg_uuid_int64 *uuid)
{
	pg_uuid_t *std;
	uint64 tmp;

	std = (pg_uuid_t *) palloc(UUID_LEN);

	tmp = UUID_V1_MSB_SCHUFFLE(uuid->first);
	tmp = pg_hton64(tmp);
	memcpy(&(std->data)[0], &tmp, 8);

	tmp = pg_hton64(uuid->second);
	memcpy(&(std->data)[8], &tmp, 8);

	return std;
}

static int
uuid_int64_cmp0(const pg_uuid_int64 *a, const pg_uuid_int64 *b)
{
	if (a->first < b->first)
		return -1;
	else if (a->first > b->first)
		return 1;

	if (a->second < b->second)
		return -1;
	else if (a->second > b->second)
		return 1;

	return 0;
}

Datum
uuid_int64_cmp(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_INT32(uuid_int64_cmp0(a, b));
}

Datum
uuid_int64_eq(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(a->first == b->first && a->second == b->second);
}

Datum
uuid_int64_ne(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(a->first != b->first || a->second != b->second);
}

Datum
uuid_int64_lt(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(uuid_int64_cmp0(a, b) < 0);
}

Datum
uuid_int64_le(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(uuid_int64_cmp0(a, b) <= 0);
}

Datum
uuid_int64_gt(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(uuid_int64_cmp0(a, b) > 0);
}

Datum
uuid_int64_ge(PG_FUNCTION_ARGS)
{
	pg_uuid_int64 *a = PG_GETARG_UUID64_P(0);
	pg_uuid_int64 *b = PG_GETARG_UUID64_P(1);

	PG_RETURN_BOOL(uuid_int64_cmp0(a, b) >= 0);
}


/*
 * Parts of below code have been shamelessly copied (and modified) from:
 *	  src/backend/utils/adt/uuid.c
 *
 * ...and are:
 * Copyright (c) 2007-2019, PostgreSQL Global Development Group
 */

/*
 * Sort support strategy routine
 */
Datum
uuid_int64_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = uuid_int64_sort_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		uuid_int64_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(uuid_int64_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = uuid_v1_cmp_abbrev;
		ssup->abbrev_converter = uuid_int64_abbrev_convert;
		ssup->abbrev_abort = uuid_v1_abbrev_abort;
		ssup->abbrev_full_comparator = uuid_int64_sort_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport comparison func
 */
static int
uuid_int64_sort_cmp(Datum x, Datum y, SortSupport ssup)
{
	pg_uuid_int64 *arg1 = DatumGetUUID64P(x);
	pg_uuid_int64 *arg2 = DatumGetUUID64P(y);

	return uuid_int64_cmp0(arg1, arg2);
}

/*
 * Conversion routine for sortsupport.
 *
 * Converts original uuid representation to abbreviated key representation.
 *
 * Our encoding strategy is simple: if the UUID is an RFC 4122 version 1 then
 * extract the 60-bit timestamp. Otherwise, pack the first `sizeof(Datum)`
 * bytes of uuid data into a Datum (on little-endian machines, the bytes are
 * stored in reverse order), and treat it as an unsigned integer.
 */
static Datum
uuid_int64_abbrev_convert(Datum original, SortSupport ssup)
{
	uuid_int64_sortsupport_state *uss = ssup->ssup_extra;
	pg_uuid_int64 *authoritative = DatumGetUUID64P(original);
	Datum res;
	uint64 msb = authoritative->first;

#if SIZEOF_DATUM == 8
	memcpy(&res, &msb, sizeof(Datum));
#else       /* SIZEOF_DATUM != 8 */
	/* use last 4 bytes of int64 as they are more significant */
	memcpy(&res, &msb + 4, sizeof(Datum));
#endif

	uss->input_count += 1;

	if (uss->estimating)
	{
		uint32 tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else       /* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	/*
	 * Byteswap on little-endian machines.
	 *
	 * This is needed so that uuid_ts_cmp_abbrev() (an unsigned integer 3-way
	 * comparator) works correctly on all platforms.  If we didn't do this,
	 * the comparator would have to call memcmp() with a pair of pointers to
	 * the first byte of each abbreviated key, which is slower.
	 */
	res = DatumBigEndianToNative(res);

	return res;
}

/*
 * Abbreviated key comparison func
 */
static int
uuid_v1_cmp_abbrev(Datum x, Datum y, SortSupport ssup)
{
	if (x > y)
		return 1;
	else if (x == y)
		return 0;
	else
		return -1;
}

/*
 * Callback for estimating effectiveness of abbreviated key optimization.
 *
 * We pay no attention to the cardinality of the non-abbreviated data, because
 * there is no equality fast-path within authoritative uuid comparator.
 */
static bool
uuid_v1_abbrev_abort(int memtupcount, SortSupport ssup)
{
	uuid_int64_sortsupport_state *uss = ssup->ssup_extra;
	double abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * If we have >100k distinct values, then even if we were sorting many
	 * billion rows we'd likely still break even, and the penalty of undoing
	 * that many rows of abbrevs would probably not be worth it.  Stop even
	 * counting at that point.
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				"uuid_int64_abbrev: estimation ends at cardinality %f"
				" after " INT64_FORMAT " values (%d rows)",
				abbr_card, uss->input_count, memtupcount);
#endif
		uss->estimating = false;
		return false;
	}

	/*
	 * Target minimum cardinality is 1 per ~2k of non-null inputs.  0.5 row
	 * fudge factor allows us to abort earlier on genuinely pathological data
	 * where we've had exactly one abbreviated value in the first 2k
	 * (non-null) rows.
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				"uuid_int64_abbrev: aborting abbreviation at cardinality %f"
				" below threshold %f after " INT64_FORMAT " values (%d rows)",
				abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			"uuid_int64_abbrev: cardinality %f after " INT64_FORMAT
			" values (%d rows)", abbr_card, uss->input_count, memtupcount);
#endif

	return false;
}
