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

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "CREATE EXTENSION uuid_int64" to load this file. \quit


-- create new data type "uuid_int64"
CREATE FUNCTION uuid_int64_in(cstring)
RETURNS uuid_int64
AS 'MODULE_PATHNAME', 'uuid_int64_in'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

CREATE FUNCTION uuid_int64_out(uuid_int64)
RETURNS cstring
AS 'MODULE_PATHNAME', 'uuid_int64_out'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

CREATE FUNCTION uuid_int64_recv(internal)
RETURNS uuid_int64
AS 'MODULE_PATHNAME', 'uuid_int64_recv'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

CREATE FUNCTION uuid_int64_send(uuid_int64)
RETURNS bytea
AS 'MODULE_PATHNAME', 'uuid_int64_send'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

CREATE TYPE uuid_int64 (
    INTERNALLENGTH = 16,
    INPUT = uuid_int64_in,
    OUTPUT = uuid_int64_out,
    RECEIVE = uuid_int64_recv,
    SEND = uuid_int64_send,
    STORAGE = plain,
    ALIGNMENT = double
);

-- type conversion helper functions
CREATE FUNCTION uuid_int64_convert(uuid_int64) RETURNS uuid
AS 'MODULE_PATHNAME', 'uuid_int64_conv_to_std'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

CREATE FUNCTION uuid_int64_convert(uuid) RETURNS uuid_int64
AS 'MODULE_PATHNAME', 'uuid_int64_conv_from_std'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

-- create casts
CREATE CAST (uuid AS uuid_int64) WITH FUNCTION uuid_int64_convert(uuid) AS IMPLICIT;

CREATE CAST (uuid_int64 AS uuid) WITH FUNCTION uuid_int64_convert(uuid_int64) AS IMPLICIT;

-- extract information
CREATE FUNCTION uuid_int64_timestamp(uuid_int64) RETURNS timestamp with time zone
AS 'MODULE_PATHNAME', 'uuid_int64_timestamp'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

-- equal
CREATE FUNCTION uuid_int64_eq(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_eq'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_eq(uuid_int64, uuid_int64) IS 'equal to';

CREATE OPERATOR = (
    LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_eq,
    COMMUTATOR = '=',
    NEGATOR = '<>',
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    MERGES
);

-- not equal
CREATE FUNCTION uuid_int64_ne(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_ne'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_ne(uuid_int64, uuid_int64) IS 'not equal to';

CREATE OPERATOR <> (
	LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_ne,
    COMMUTATOR = '<>',
    NEGATOR = '=',
    RESTRICT = neqsel,
    JOIN = neqjoinsel
);

-- lower than
CREATE FUNCTION uuid_int64_lt(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_lt'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_lt(uuid_int64, uuid_int64) IS 'lower than';

CREATE OPERATOR < (
	LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_lt,
	COMMUTATOR = '>',
    NEGATOR = '>=',
	RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel
);

-- greater than
CREATE FUNCTION uuid_int64_gt(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_gt'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_gt(uuid_int64, uuid_int64) IS 'greater than';

CREATE OPERATOR > (
	LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_gt,
	COMMUTATOR = '<',
    NEGATOR = '<=',
	RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel
);

-- lower than or equal
CREATE FUNCTION uuid_int64_le(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_le'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_le(uuid_int64, uuid_int64) IS 'lower than or equal to';

CREATE OPERATOR <= (
	LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_le,
	COMMUTATOR = '>=',
    NEGATOR = '>',
	RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel
);

-- greater than or equal
CREATE FUNCTION uuid_int64_ge(uuid_int64, uuid_int64)
RETURNS bool
AS 'MODULE_PATHNAME', 'uuid_int64_ge'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_ge(uuid_int64, uuid_int64) IS 'greater than or equal to';

CREATE OPERATOR >= (
	LEFTARG = uuid_int64,
    RIGHTARG = uuid_int64,
    PROCEDURE = uuid_int64_ge,
	COMMUTATOR = '<=',
    NEGATOR = '<',
	RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel
);

-- generic comparison function
CREATE FUNCTION uuid_int64_cmp(uuid_int64, uuid_int64)
RETURNS int4
AS 'MODULE_PATHNAME', 'uuid_int64_cmp'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_cmp(uuid_int64, uuid_int64) IS 'UUID v1 comparison function';

-- sort support function
CREATE FUNCTION uuid_int64_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'uuid_int64_sortsupport'
LANGUAGE C IMMUTABLE LEAKPROOF STRICT PARALLEL SAFE;

COMMENT ON FUNCTION uuid_int64_sortsupport(internal) IS 'btree sort support function';


-- create operator class
CREATE OPERATOR CLASS uuid_int64_ops DEFAULT FOR TYPE uuid_int64
    USING btree AS
        OPERATOR        1       <,
        OPERATOR        2       <=,
        OPERATOR        3       =,
        OPERATOR        4       >=,
        OPERATOR        5       >,
        FUNCTION        1       uuid_int64_cmp(uuid_int64, uuid_int64),
        FUNCTION        2       uuid_int64_sortsupport(internal)
;
