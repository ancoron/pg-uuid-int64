/*-------------------------------------------------------------------------
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
 *
 * uuid_int64.h
 *	  Header file for the "uuid_int64" ADT. In C, we use the name pg_uuid_int64,
 *	  to avoid conflicts with any uuid_t type that might be defined by
 *	  the system headers.
 *
 * uuid_int64.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UUID_INT64_H
#define UUID_INT64_H

#define UUID_NODE_LEN 6

typedef struct pg_uuid_int64
{
    uint64           first;
    uint64           second;
} pg_uuid_int64;

/* fmgr interface macros */
#define UUID64PGetDatum(X)		PointerGetDatum(X)
#define PG_RETURN_UUID64_P(X)	return UUID64PGetDatum(X)
#define DatumGetUUID64P(X)		((pg_uuid_int64 *) DatumGetPointer(X))
#define PG_GETARG_UUID64_P(X)	DatumGetUUID64P(PG_GETARG_DATUM(X))

extern pg_uuid_int64* uuid_std_to_64(const pg_uuid_t *uuid);
extern pg_uuid_t* uuid_64_to_std(const pg_uuid_int64 *uuid);

#endif							/* UUID_INT64_H */
