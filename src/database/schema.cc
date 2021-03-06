/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "database/schema.h"

#include <algorithm>                              // for std::move
#include <cassert>                                // for assert
#include <cmath>                                  // for std::pow
#include <cstdint>                                // for uint64_t
#include <cstring>                                // for size_t, strlen
#include <cctype>                                 // for tolower
#include <functional>                             // for ref, reference_wrapper
#include <limits>                                 // for std::numeric_limits
#include <mutex>                                  // for std::mutex
#include <ostream>                                // for operator<<, basic_ostream
#include <set>                                    // for std::set
#include <stdexcept>                              // for std::out_of_range
#include <type_traits>                            // for remove_reference<>::type
#include <utility>

#include "base_x.hh"                              // for Base64
#include "cast.h"                                 // for Cast
#include "cuuid/uuid.h"                           // for UUIDGenerator
#include "database/handler.h"                     // for DatabaseHandler
#include "database/lock.h"                        // for lock_shard
#include "database/shard.h"                       // for Shard
#include "datetime.h"                             // for isDate, isDatetime, tm_t
#include "exception.h"                            // for ClientError
#include "geospatial/geospatial.h"                // for GeoSpatial
#include "manager.h"                              // for XapiandManager, XapiandMan...
#include "multivalue/generate_terms.h"            // for integer, geo, datetime, positive
#include "opts.h"                                 // for opts::*
#include "reserved/schema.h"                      // for RESERVED_
#include "script.h"                               // for Script
#include "serialise_list.h"                       // for StringList
#include "split.h"                                // for Split
#include "static_string.hh"                       // for static_string
#include "stopper.h"                              // for getStopper
#include "strings.hh"                             // for strings::format, strings::inplace_lower

#define L_SCHEMA L_NOTHING


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY
// #undef L_INDEX
// #define L_INDEX L_CHOCOLATE


constexpr static auto EMPTY      = static_string::string(EMPTY_CHAR);
constexpr static auto STRING     = static_string::string(STRING_CHAR);
constexpr static auto TIMEDELTA  = static_string::string(TIMEDELTA_CHAR);
constexpr static auto ARRAY      = static_string::string(ARRAY_CHAR);
constexpr static auto BOOLEAN    = static_string::string(BOOLEAN_CHAR);
constexpr static auto DATE       = static_string::string(DATE_CHAR);
constexpr static auto DATETIME   = static_string::string(DATETIME_CHAR);
constexpr static auto FOREIGN    = static_string::string(FOREIGN_CHAR);
constexpr static auto FLOATING   = static_string::string(FLOATING_CHAR);
constexpr static auto GEO        = static_string::string(GEO_CHAR);
constexpr static auto INTEGER    = static_string::string(INTEGER_CHAR);
constexpr static auto OBJECT     = static_string::string(OBJECT_CHAR);
constexpr static auto POSITIVE   = static_string::string(POSITIVE_CHAR);
constexpr static auto TEXT       = static_string::string(TEXT_CHAR);
constexpr static auto KEYWORD    = static_string::string(KEYWORD_CHAR);
constexpr static auto UUID       = static_string::string(UUID_CHAR);
constexpr static auto SCRIPT     = static_string::string(SCRIPT_CHAR);
constexpr static auto TIME       = static_string::string(TIME_CHAR);


const std::string NAMESPACE_PREFIX_ID_FIELD_NAME = get_prefix(ID_FIELD_NAME);


/*
 * index() algorithm outline:
 * 1. Try reading schema from the metadata, if there is already a schema jump to 3.
 * 2. Write properties and feed specification_t using write_*, this step could
 *    use some process_* (for some properties). Jump to 5.
 * 3. Feed specification_t with the read schema using feed_*;
 *    sets field_found for all found fields.
 * 4. Complement specification_t with the object sent by the user using process_*,
 *    except those that are already fixed because are reserved to be and
 *    they already exist in the metadata, those are simply checked with consistency_*.
 * 5. If the field in the schema is normal and still has no RESERVED_TYPE (concrete)
 *    and a value is received for the field, call validate_required_data() to
 *    initialize the specification with validated data sent by the user.
 * 6. If the field is namespace or has partial paths call validate_required_namespace_data() to
 *    initialize the specification with default specifications and sent by the user.
 * 7. If there are values sent by user, fills the document to be indexed via
 *    index_item_value()
 * 8. If the path has uuid field name the values are indexed according to index_uuid_field.
 * 9. index_object() does step 2 to 8 and for each field it calls index_object(...).
 * 10. index() does steps 2 to 4 and for each field it calls index_object(...)
 *
 * write_schema() algorithm outline:
 * 1. Try reading schema from the metadata.
 * 2. If there is already a schema, feed specification_t with the read schema
 *    using feed_*; sets field_found for all found fields.
 * 3. Write properties and feed specification_t using write_*, this step could
 *    use some process_* (for some properties).
 * 4. write_object() does step 2 to 3 and for each field it calls update_schema(...).
 */


/*
 * Unordered Maps used for reading user data specification.
 */


namespace std {
	template<typename T, size_t N>
	struct hash<const array<T, N>> {
		size_t operator()(const array<T, N>& a) const {
			size_t h = 0;
			for (auto e : a) {
				h ^= hash<T>{}(e) + 0x9e3779b9 + (h << 6) + (h >> 2);
			}
			return h;
		}
	};
}

/*
 * Default accuracies.
 */

/* Python script to generate the def_accuracy_num:
 * deltas = [8, 7, 7, 7, 7, 7, 5, 5, 2, 1]; w = [(0, 64)]; print('\n'.join(reversed(['\t(1ULL << {}),                       // {} (max_terms: {}, delta: {})'.format(y, 1 << y, int((1 << x) / (1 << y)), x - y) for x, y in ((lambda delta: w.append((w[-1][1], w[-1][1] - delta)))(delta) or w[-1] for delta in reversed(deltas))])))
 */
static const std::vector<uint64_t> def_accuracy_num({
	100ULL,
	1000ULL,
	10000ULL,
	100000ULL,
	1000000ULL,
	100000000ULL,
 });


static const std::vector<uint64_t> def_accuracy_date({
	toUType(UnitTime::day),             // 86400 s
	toUType(UnitTime::month),           // 2592000 s
	toUType(UnitTime::year),            // 31536000 s
	toUType(UnitTime::decade),          // 315360000 s
	toUType(UnitTime::century),         // 3153600000 s
});


static const std::vector<uint64_t> def_accuracy_datetime({
	toUType(UnitTime::hour),            // 3600 s
	toUType(UnitTime::day),             // 86400 s
	toUType(UnitTime::month),           // 2592000 s
	toUType(UnitTime::year),            // 31536000 s
	toUType(UnitTime::decade),          // 315360000 s
	toUType(UnitTime::century),         // 3153600000 s
});


static const std::vector<uint64_t> def_accuracy_time({
	toUType(UnitTime::minute),          // 60 s
	toUType(UnitTime::hour),            // 3600 s
});


/* HTM terms (Hierarchical Triangular Mesh)
 * HTM terms (Hierarchical Triangular Mesh)
 * Any integer value in the range 0-25 can be used to specify an HTM level
 * An approximation of the accuracy obtained by a level can be estimated as:
 *    0.30 * 2 ** (25 - level)
 *
 * Python script to generate the def_accuracy_geo:
 * levels = [3, 5, 8, 10, 12, 15]; print('\n'.join('\t{},                                 // ~ {} m'.format(level, 0.30 * 2 ** (25 - level)) for level in levels))
 */
static const std::vector<uint64_t> def_accuracy_geo({
	3,                                  //  ~ 1,258,291.2 m
	5,                                  //    ~ 314,572.8 m
	8,                                  //     ~ 39,321.6 m
	10,                                 //      ~ 9,830.4 m
	12,                                 //      ~ 2,457.6 m
	15,                                 //        ~ 307.2 m
});


required_spc_t _get_data_id(required_spc_t& spc_id, const MsgPack& id_properties);


static inline bool
validate_acc_date(UnitTime unit) noexcept
{
	switch (unit) {
		case UnitTime::second:
		case UnitTime::minute:
		case UnitTime::hour:
		case UnitTime::day:
		case UnitTime::month:
		case UnitTime::year:
		case UnitTime::decade:
		case UnitTime::century:
		case UnitTime::millennium:
			return true;
		default:
			return false;
	}
}


/*
 * Helper functions to print readable form of enums
 */

static inline const std::string&
_get_str_acc_date(UnitTime unit) noexcept
{
	switch (unit) {
		case UnitTime::second: {
			static const std::string second("second");
			return second;
		}
		case UnitTime::minute: {
			static const std::string minute("minute");
			return minute;
		}
		case UnitTime::hour: {
			static const std::string hour("hour");
			return hour;
		}
		case UnitTime::day: {
			static const std::string day("day");
			return day;
		}
		case UnitTime::month: {
			static const std::string month("month");
			return month;
		}
		case UnitTime::year: {
			static const std::string year("year");
			return year;
		}
		case UnitTime::decade: {
			static const std::string decade("decade");
			return decade;
		}
		case UnitTime::century: {
			static const std::string century("century");
			return century;
		}
		case UnitTime::millennium: {
			static const std::string millennium("millennium");
			return millennium;
		}
		default: {
			static const std::string unknown("unknown");
			return unknown;
		}
	}
}


static inline const std::string&
_get_str_index(TypeIndex index) noexcept
{
	switch (index) {
		case TypeIndex::NONE: {
			static const std::string none("none");
			return none;
		}
		case TypeIndex::FIELD_TERMS: {
			static const std::string field_terms("field_terms");
			return field_terms;
		}
		case TypeIndex::FIELD_VALUES: {
			static const std::string field_values("field_values");
			return field_values;
		}
		case TypeIndex::FIELD_ALL: {
			static const std::string field("field");
			return field;
		}
		case TypeIndex::GLOBAL_TERMS: {
			static const std::string global_terms("global_terms");
			return global_terms;
		}
		case TypeIndex::TERMS: {
			static const std::string terms("terms");
			return terms;
		}
		case TypeIndex::GLOBAL_TERMS_FIELD_VALUES: {
			static const std::string global_terms_field_values("global_terms,field_values");
			return global_terms_field_values;
		}
		case TypeIndex::GLOBAL_TERMS_FIELD_ALL: {
			static const std::string global_terms_field("global_terms,field");
			return global_terms_field;
		}
		case TypeIndex::GLOBAL_VALUES: {
			static const std::string global_values("global_values");
			return global_values;
		}
		case TypeIndex::GLOBAL_VALUES_FIELD_TERMS: {
			static const std::string global_values_field_terms("global_values,field_terms");
			return global_values_field_terms;
		}
		case TypeIndex::VALUES: {
			static const std::string values("values");
			return values;
		}
		case TypeIndex::GLOBAL_VALUES_FIELD_ALL: {
			static const std::string global_values_field("global_values,field");
			return global_values_field;
		}
		case TypeIndex::GLOBAL_ALL: {
			static const std::string global("global");
			return global;
		}
		case TypeIndex::GLOBAL_ALL_FIELD_TERMS: {
			static const std::string global_field_terms("global,field_terms");
			return global_field_terms;
		}
		case TypeIndex::GLOBAL_ALL_FIELD_VALUES: {
			static const std::string global_field_values("global,field_values");
			return global_field_values;
		}
		case TypeIndex::ALL: {
			static const std::string all("all");
			return all;
		}
		default: {
			static const std::string unknown("unknown");
			return unknown;
		}
	}
}


static const std::string str_set_acc_date(strings::join<std::string>({
	"second",
	"minute",
	"hour",
	"day",
	"month",
	"year",
	"decade",
	"century",
	"millennium",
}, ", ", " or "));


inline UnitTime
_get_accuracy_date(std::string_view str_accuracy_date)
{
	constexpr static auto _ = phf::make_phf({
		hhl("day"),
		hhl("month"),
		hhl("year"),
		hhl("decade"),
		hhl("century"),
		hhl("millennium"),
	});

	switch (_.fhhl(str_accuracy_date)) {
		case _.fhhl("day"):
			return UnitTime::day;
		case _.fhhl("month"):
			return UnitTime::month;
		case _.fhhl("year"):
			return UnitTime::year;
		case _.fhhl("decade"):
			return UnitTime::decade;
		case _.fhhl("century"):
			return UnitTime::century;
		case _.fhhl("millennium"):
			return UnitTime::millennium;
		default:
			return UnitTime::INVALID;
	}
}


UnitTime
get_accuracy_date(std::string_view str_accuracy_date)
{
	return _get_accuracy_date(str_accuracy_date);
}


inline UnitTime
_get_accuracy_datetime(std::string_view str_accuracy_datetime)
{
	constexpr static auto _ = phf::make_phf({
		hhl("second"),
		hhl("minute"),
		hhl("hour"),
		hhl("day"),
		hhl("month"),
		hhl("year"),
		hhl("decade"),
		hhl("century"),
		hhl("millennium"),
	});

	switch (_.fhhl(str_accuracy_datetime)) {
		case _.fhhl("second"):
			return UnitTime::second;
		case _.fhhl("minute"):
			return UnitTime::minute;
		case _.fhhl("hour"):
			return UnitTime::hour;
		case _.fhhl("day"):
			return UnitTime::day;
		case _.fhhl("month"):
			return UnitTime::month;
		case _.fhhl("year"):
			return UnitTime::year;
		case _.fhhl("decade"):
			return UnitTime::decade;
		case _.fhhl("century"):
			return UnitTime::century;
		case _.fhhl("millennium"):
			return UnitTime::millennium;
		default:
			return UnitTime::INVALID;
	}
}


UnitTime
get_accuracy_datetime(std::string_view str_accuracy_datetime)
{
	return _get_accuracy_datetime(str_accuracy_datetime);
}


static const std::string str_set_acc_time(strings::join<std::string>({
	"second",
	"minute",
	"hour",
}, ", ", " or "));

inline UnitTime
_get_accuracy_time(std::string_view str_accuracy_time)
{
	constexpr static auto _ = phf::make_phf({
		hhl("second"),
		hhl("minute"),
		hhl("hour"),
	});

	switch (_.fhhl(str_accuracy_time)) {
		case _.fhhl("second"):
			return UnitTime::second;
		case _.fhhl("minute"):
			return UnitTime::minute;
		case _.fhhl("hour"):
			return UnitTime::hour;
		default:
			return UnitTime::INVALID;
	}
}


UnitTime
get_accuracy_time(std::string_view str_accuracy_time)
{
	return _get_accuracy_time(str_accuracy_time);
}


static const std::string str_set_stop_strategy(strings::join<std::string>({
	"stop_none",
	"none",
	"stop_all",
	"all",
	"stop_stemmed",
	"stemmed",
}, ", ", " or "));

inline StopStrategy
_get_stop_strategy(std::string_view str_stop_strategy)
{
	constexpr static auto _ = phf::make_phf({
		hhl("stop_none"),
		hhl("none"),
		hhl("stop_all"),
		hhl("all"),
		hhl("stop_stemmed"),
		hhl("stemmed"),
	});

	switch (_.fhhl(str_stop_strategy)) {
		case _.fhhl("stop_none"):
			return StopStrategy::stop_none;
		case _.fhhl("none"):
			return StopStrategy::stop_none;
		case _.fhhl("stop_all"):
			return StopStrategy::stop_all;
		case _.fhhl("all"):
			return StopStrategy::stop_all;
		case _.fhhl("stop_stemmed"):
			return StopStrategy::stop_stemmed;
		case _.fhhl("stemmed"):
			return StopStrategy::stop_stemmed;
		default:
			return StopStrategy::INVALID;
	}
}


static const std::string str_set_stem_strategy(strings::join<std::string>({
	"stem_none",
	"none",
	"stem_some",
	"some",
	"stem_all",
	"all",
	"stem_all_z",
	"all_z",
}, ", ", " or "));


static const std::string str_set_index_uuid_field(strings::join<std::string>({
	"uuid",
	"uuid_field",
	"both",
}, ", ", " or "));

static inline UUIDFieldIndex
_get_index_uuid_field(std::string_view str_index_uuid_field)
{
	constexpr static auto _ = phf::make_phf({
		hhl("uuid"),
		hhl("uuid_field"),
		hhl("both"),
	});

	switch (_.fhhl(str_index_uuid_field)) {
		case _.fhhl("uuid"):
			return UUIDFieldIndex::uuid;
		case _.fhhl("uuid_field"):
			return UUIDFieldIndex::uuid_field;
		case _.fhhl("both"):
			return UUIDFieldIndex::both;
		default:
			return UUIDFieldIndex::INVALID;
	}
}



static const std::string str_set_index(strings::join<std::string>({
	"none",
	"field_terms",
	"field_values",
	"field_terms,field_values",
	"field_values,field_terms",
	"field",
	"field_all",
	"global_terms",
	"field_terms,global_terms",
	"global_terms,field_terms",
	"terms",
	"global_terms,field_values",
	"field_values,global_terms",
	"global_terms,field",
	"global_terms,field_all",
	"field,global_terms",
	"field_all,global_terms",
	"global_values",
	"global_values,field_terms",
	"field_terms,global_values",
	"field_values,global_values",
	"global_values,field_values",
	"values",
	"global_values,field",
	"global_values,field_all",
	"field,global_values",
	"field_all,global_values",
	"global",
	"global_all",
	"global_values,global_terms",
	"global_terms,global_values",
	"global,field_terms",
	"global_all,field_terms",
	"field_terms,global",
	"field_terms,global_all",
	"global_all,field_values",
	"global,field_values",
	"field_values,global",
	"field_values,global_all",
	"field_all,global_all",
	"global_all,field_all",
	"all",
}, ", ", " or "));

static inline TypeIndex
_get_index(std::string_view str_index)
{
	constexpr static auto _ = phf::make_phf({
		hhl("none"),
		hhl("field_terms"),
		hhl("field_values"),
		hhl("field_terms,field_values"),
		hhl("field_values,field_terms"),
		hhl("field"),
		hhl("field_all"),
		hhl("global_terms"),
		hhl("field_terms,global_terms"),
		hhl("global_terms,field_terms"),
		hhl("terms"),
		hhl("global_terms,field_values"),
		hhl("field_values,global_terms"),
		hhl("global_terms,field"),
		hhl("global_terms,field_all"),
		hhl("field,global_terms"),
		hhl("field_all,global_terms"),
		hhl("global_values"),
		hhl("global_values,field_terms"),
		hhl("field_terms,global_values"),
		hhl("field_values,global_values"),
		hhl("global_values,field_values"),
		hhl("values"),
		hhl("global_values,field"),
		hhl("global_values,field_all"),
		hhl("field,global_values"),
		hhl("field_all,global_values"),
		hhl("global"),
		hhl("global_all"),
		hhl("global_values,global_terms"),
		hhl("global_terms,global_values"),
		hhl("global,field_terms"),
		hhl("global_all,field_terms"),
		hhl("field_terms,global"),
		hhl("field_terms,global_all"),
		hhl("global_all,field_values"),
		hhl("global,field_values"),
		hhl("field_values,global"),
		hhl("field_values,global_all"),
		hhl("field_all,global_all"),
		hhl("global_all,field_all"),
		hhl("all"),
	});

	switch(_.fhhl(str_index)) {
		case _.fhhl("none"):
			return TypeIndex::NONE;
		case _.fhhl("field_terms"):
			return TypeIndex::FIELD_TERMS;
		case _.fhhl("field_values"):
			return TypeIndex::FIELD_VALUES;
		case _.fhhl("field_terms,field_values"):
			return TypeIndex::FIELD_ALL;
		case _.fhhl("field_values,field_terms"):
			return TypeIndex::FIELD_ALL;
		case _.fhhl("field"):
			return TypeIndex::FIELD_ALL;
		case _.fhhl("field_all"):
			return TypeIndex::FIELD_ALL;
		case _.fhhl("global_terms"):
			return TypeIndex::GLOBAL_TERMS;
		case _.fhhl("field_terms,global_terms"):
			return TypeIndex::TERMS;
		case _.fhhl("global_terms,field_terms"):
			return TypeIndex::TERMS;
		case _.fhhl("terms"):
			return TypeIndex::TERMS;
		case _.fhhl("global_terms,field_values"):
			return TypeIndex::GLOBAL_TERMS_FIELD_VALUES;
		case _.fhhl("field_values,global_terms"):
			return TypeIndex::GLOBAL_TERMS_FIELD_VALUES;
		case _.fhhl("global_terms,field"):
			return TypeIndex::GLOBAL_TERMS_FIELD_ALL;
		case _.fhhl("global_terms,field_all"):
			return TypeIndex::GLOBAL_TERMS_FIELD_ALL;
		case _.fhhl("field,global_terms"):
			return TypeIndex::GLOBAL_TERMS_FIELD_ALL;
		case _.fhhl("field_all,global_terms"):
			return TypeIndex::GLOBAL_TERMS_FIELD_ALL;
		case _.fhhl("global_values"):
			return TypeIndex::GLOBAL_VALUES;
		case _.fhhl("global_values,field_terms"):
			return TypeIndex::GLOBAL_VALUES_FIELD_TERMS;
		case _.fhhl("field_terms,global_values"):
			return TypeIndex::GLOBAL_VALUES_FIELD_TERMS;
		case _.fhhl("field_values,global_values"):
			return TypeIndex::VALUES;
		case _.fhhl("global_values,field_values"):
			return TypeIndex::VALUES;
		case _.fhhl("values"):
			return TypeIndex::VALUES;
		case _.fhhl("global_values,field"):
			return TypeIndex::GLOBAL_VALUES_FIELD_ALL;
		case _.fhhl("global_values,field_all"):
			return TypeIndex::GLOBAL_VALUES_FIELD_ALL;
		case _.fhhl("field,global_values"):
			return TypeIndex::GLOBAL_VALUES_FIELD_ALL;
		case _.fhhl("field_all,global_values"):
			return TypeIndex::GLOBAL_VALUES_FIELD_ALL;
		case _.fhhl("global"):
			return TypeIndex::GLOBAL_ALL;
		case _.fhhl("global_all"):
			return TypeIndex::GLOBAL_ALL;
		case _.fhhl("global_values,global_terms"):
			return TypeIndex::GLOBAL_ALL;
		case _.fhhl("global_terms,global_values"):
			return TypeIndex::GLOBAL_ALL;
		case _.fhhl("global,field_terms"):
			return TypeIndex::GLOBAL_ALL_FIELD_TERMS;
		case _.fhhl("global_all,field_terms"):
			return TypeIndex::GLOBAL_ALL_FIELD_TERMS;
		case _.fhhl("field_terms,global"):
			return TypeIndex::GLOBAL_ALL_FIELD_TERMS;
		case _.fhhl("field_terms,global_all"):
			return TypeIndex::GLOBAL_ALL_FIELD_TERMS;
		case _.fhhl("global_all,field_values"):
			return TypeIndex::GLOBAL_ALL_FIELD_VALUES;
		case _.fhhl("global,field_values"):
			return TypeIndex::GLOBAL_ALL_FIELD_VALUES;
		case _.fhhl("field_values,global"):
			return TypeIndex::GLOBAL_ALL_FIELD_VALUES;
		case _.fhhl("field_values,global_all"):
			return TypeIndex::GLOBAL_ALL_FIELD_VALUES;
		case _.fhhl("field_all,global_all"):
			return TypeIndex::ALL;
		case _.fhhl("global_all,field_all"):
			return TypeIndex::ALL;
		case _.fhhl("all"):
			return TypeIndex::ALL;
		default:
			return TypeIndex::INVALID;
	}
}


static inline const std::array<FieldType, SPC_TOTAL_TYPES>&
_get_type(std::string_view str_type)
{
	constexpr static auto _ = phf::make_phf({
		hhl("boolean"),
		hhl("date"),
		hhl("datetime"),
		hhl("float"),
		hhl("floating"),
		hhl("geospatial"),
		hhl("integer"),
		hhl("positive"),
		hhl("string"),
		hhl("term"),  // FIXME: remove legacy term
		hhl("keyword"),
		hhl("text"),
		hhl("time"),
		hhl("timedelta"),
		hhl("uuid"),
		hhl("script"),
		hhl("array"),
		hhl("array/boolean"),
		hhl("array/date"),
		hhl("array/datetime"),
		hhl("array/float"),
		hhl("array/geospatial"),
		hhl("array/integer"),
		hhl("array/positive"),
		hhl("array/string"),
		hhl("array/term"),  // FIXME: remove legacy term
		hhl("array/keyword"),
		hhl("array/text"),
		hhl("array/time"),
		hhl("array/timedelta"),
		hhl("array/uuid"),
		hhl("boolean/array"),
		hhl("date/array"),
		hhl("datetime/array"),
		hhl("float/array"),
		hhl("geospatial/array"),
		hhl("integer/array"),
		hhl("positive/array"),
		hhl("string/array"),
		hhl("term/array"),  // FIXME: remove legacy term
		hhl("keyword/array"),
		hhl("text/array"),
		hhl("time/array"),
		hhl("timedelta/array"),
		hhl("uuid/array"),
		hhl("object"),
		hhl("object/boolean"),
		hhl("object/date"),
		hhl("object/datetime"),
		hhl("object/float"),
		hhl("object/geospatial"),
		hhl("object/integer"),
		hhl("object/positive"),
		hhl("object/string"),
		hhl("object/term"),  // FIXME: remove legacy term
		hhl("object/keyword"),
		hhl("object/text"),
		hhl("object/time"),
		hhl("object/timedelta"),
		hhl("object/uuid"),
		hhl("boolean/object"),
		hhl("date/object"),
		hhl("datetime/object"),
		hhl("float/object"),
		hhl("geospatial/object"),
		hhl("integer/object"),
		hhl("positive/object"),
		hhl("string/object"),
		hhl("term/object"),  // FIXME: remove legacy term
		hhl("keyword/object"),
		hhl("text/object"),
		hhl("time/object"),
		hhl("timedelta/object"),
		hhl("uuid/object"),
		hhl("array/object"),
		hhl("array/boolean/object"),
		hhl("array/date/object"),
		hhl("array/datetime/object"),
		hhl("array/float/object"),
		hhl("array/geospatial/object"),
		hhl("array/integer/object"),
		hhl("array/positive/object"),
		hhl("array/string/object"),
		hhl("array/term/object"),  // FIXME: remove legacy term
		hhl("array/keyword/object"),
		hhl("array/text/object"),
		hhl("array/time/object"),
		hhl("array/timedelta/object"),
		hhl("array/uuid/object"),
		hhl("array/object/boolean"),
		hhl("array/object/date"),
		hhl("array/object/datetime"),
		hhl("array/object/float"),
		hhl("array/object/geospatial"),
		hhl("array/object/integer"),
		hhl("array/object/positive"),
		hhl("array/object/string"),
		hhl("array/object/term"),  // FIXME: remove legacy term
		hhl("array/object/keyword"),
		hhl("array/object/text"),
		hhl("array/object/time"),
		hhl("array/object/timedelta"),
		hhl("array/object/uuid"),
		hhl("object/array"),
		hhl("object/array/boolean"),
		hhl("object/array/date"),
		hhl("object/array/datetime"),
		hhl("object/array/float"),
		hhl("object/array/geospatial"),
		hhl("object/array/integer"),
		hhl("object/array/positive"),
		hhl("object/array/string"),
		hhl("object/array/term"),  // FIXME: remove legacy term
		hhl("object/array/keyword"),
		hhl("object/array/text"),
		hhl("object/array/time"),
		hhl("object/array/timedelta"),
		hhl("object/array/uuid"),
		hhl("boolean/array/object"),
		hhl("date/array/object"),
		hhl("datetime/array/object"),
		hhl("float/array/object"),
		hhl("geospatial/array/object"),
		hhl("integer/array/object"),
		hhl("positive/array/object"),
		hhl("string/array/object"),
		hhl("term/array/object"),  // FIXME: remove legacy term
		hhl("keyword/array/object"),
		hhl("text/array/object"),
		hhl("time/array/object"),
		hhl("timedelta/array/object"),
		hhl("uuid/array/object"),
		hhl("boolean/object/array"),
		hhl("date/object/array"),
		hhl("datetime/object/array"),
		hhl("float/object/array"),
		hhl("geospatial/object/array"),
		hhl("integer/object/array"),
		hhl("positive/object/array"),
		hhl("string/object/array"),
		hhl("term/object/array"),  // FIXME: remove legacy term
		hhl("keyword/object/array"),
		hhl("text/object/array"),
		hhl("time/object/array"),
		hhl("timedelta/object/array"),
		hhl("uuid/object/array"),
		hhl("object/boolean/array"),
		hhl("object/date/array"),
		hhl("object/datetime/array"),
		hhl("object/float/array"),
		hhl("object/geospatial/array"),
		hhl("object/integer/array"),
		hhl("object/positive/array"),
		hhl("object/string/array"),
		hhl("object/term/array"),  // FIXME: remove legacy term
		hhl("object/keyword/array"),
		hhl("object/text/array"),
		hhl("object/time/array"),
		hhl("object/timedelta/array"),
		hhl("object/uuid/array"),
		hhl("foreign"),
		hhl("foreign/object"),
		hhl("object/foreign"),
		hhl("undefined"),
	});

	switch(_.fhhl(str_type)) {
		case _.fhhl("boolean"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::boolean       }};
			return _;
		}
		case _.fhhl("date"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::date          }};
			return _;
		}
		case _.fhhl("datetime"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::datetime      }};
			return _;
		}
		case _.fhhl("float"):
		case _.fhhl("floating"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::floating      }};
			return _;
		}
		case _.fhhl("geospatial"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::geo           }};
			return _;
		}
		case _.fhhl("integer"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::integer       }};
			return _;
		}
		case _.fhhl("positive"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::positive      }};
			return _;
		}
		case _.fhhl("string"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::string        }};
			return _;
		}
		case _.fhhl("term"):  // FIXME: remove legacy term
		case _.fhhl("keyword"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::keyword       }};
			return _;
		}
		case _.fhhl("text"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::text          }};
			return _;
		}
		case _.fhhl("time"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::time          }};
			return _;
		}
		case _.fhhl("timedelta"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::timedelta     }};
			return _;
		}
		case _.fhhl("uuid"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::uuid          }};
			return _;
		}
		case _.fhhl("array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::empty         }};
			return _;
		}
		case _.fhhl("boolean/array"):
		case _.fhhl("array/boolean"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::boolean       }};
			return _;
		}
		case _.fhhl("date/array"):
		case _.fhhl("array/date"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::date          }};
			return _;
		}
		case _.fhhl("datetime/array"):
		case _.fhhl("array/datetime"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::datetime      }};
			return _;
		}
		case _.fhhl("float/array"):
		case _.fhhl("array/float"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::floating      }};
			return _;
		}
		case _.fhhl("geospatial/array"):
		case _.fhhl("array/geospatial"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::geo           }};
			return _;
		}
		case _.fhhl("integer/array"):
		case _.fhhl("array/integer"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::integer       }};
			return _;
		}
		case _.fhhl("positive/array"):
		case _.fhhl("array/positive"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::positive      }};
			return _;
		}
		case _.fhhl("string/array"):
		case _.fhhl("array/string"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::string        }};
			return _;
		}
		case _.fhhl("term/array"):  // FIXME: remove legacy term
		case _.fhhl("array/term"):  // FIXME: remove legacy term
		case _.fhhl("keyword/array"):
		case _.fhhl("array/keyword"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::keyword       }};
			return _;
		}
		case _.fhhl("text/array"):
		case _.fhhl("array/text"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::text          }};
			return _;
		}
		case _.fhhl("time/array"):
		case _.fhhl("array/time"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::time          }};
			return _;
		}
		case _.fhhl("timedelta/array"):
		case _.fhhl("array/timedelta"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::timedelta     }};
			return _;
		}
		case _.fhhl("uuid/array"):
		case _.fhhl("array/uuid"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::array, FieldType::uuid          }};
			return _;
		}
		case _.fhhl("object"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::empty         }};
			return _;
		}
		case _.fhhl("array/object"):
		case _.fhhl("object/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::empty         }};
			return _;
		}
		case _.fhhl("array/boolean/object"):
		case _.fhhl("array/object/boolean"):
		case _.fhhl("object/array/boolean"):
		case _.fhhl("boolean/array/object"):
		case _.fhhl("boolean/object/array"):
		case _.fhhl("object/boolean/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::boolean       }};
			return _;
		}
		case _.fhhl("array/date/object"):
		case _.fhhl("array/object/date"):
		case _.fhhl("object/array/date"):
		case _.fhhl("date/array/object"):
		case _.fhhl("date/object/array"):
		case _.fhhl("object/date/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::date          }};
			return _;
		}
		case _.fhhl("array/datetime/object"):
		case _.fhhl("array/object/datetime"):
		case _.fhhl("object/array/datetime"):
		case _.fhhl("datetime/array/object"):
		case _.fhhl("datetime/object/array"):
		case _.fhhl("object/datetime/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::datetime      }};
			return _;
		}
		case _.fhhl("array/float/object"):
		case _.fhhl("array/object/float"):
		case _.fhhl("object/array/float"):
		case _.fhhl("float/array/object"):
		case _.fhhl("float/object/array"):
		case _.fhhl("object/float/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::floating      }};
			return _;
		}
		case _.fhhl("array/geospatial/object"):
		case _.fhhl("array/object/geospatial"):
		case _.fhhl("object/array/geospatial"):
		case _.fhhl("geospatial/array/object"):
		case _.fhhl("geospatial/object/array"):
		case _.fhhl("object/geospatial/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::geo           }};
			return _;
		}
		case _.fhhl("array/integer/object"):
		case _.fhhl("array/object/integer"):
		case _.fhhl("object/array/integer"):
		case _.fhhl("integer/array/object"):
		case _.fhhl("integer/object/array"):
		case _.fhhl("object/integer/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::integer       }};
			return _;
		}
		case _.fhhl("array/positive/object"):
		case _.fhhl("array/object/positive"):
		case _.fhhl("object/array/positive"):
		case _.fhhl("positive/array/object"):
		case _.fhhl("positive/object/array"):
		case _.fhhl("object/positive/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::positive      }};
			return _;
		}
		case _.fhhl("array/string/object"):
		case _.fhhl("array/object/string"):
		case _.fhhl("object/array/string"):
		case _.fhhl("string/array/object"):
		case _.fhhl("string/object/array"):
		case _.fhhl("object/string/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::string        }};
			return _;
		}
		case _.fhhl("array/term/object"):  // FIXME: remove legacy term
		case _.fhhl("array/object/term"):  // FIXME: remove legacy term
		case _.fhhl("object/array/term"):  // FIXME: remove legacy term
		case _.fhhl("term/array/object"):  // FIXME: remove legacy term
		case _.fhhl("term/object/array"):  // FIXME: remove legacy term
		case _.fhhl("object/term/array"):  // FIXME: remove legacy term
		case _.fhhl("array/keyword/object"):
		case _.fhhl("array/object/keyword"):
		case _.fhhl("object/array/keyword"):
		case _.fhhl("keyword/array/object"):
		case _.fhhl("keyword/object/array"):
		case _.fhhl("object/keyword/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::keyword       }};
			return _;
		}
		case _.fhhl("array/text/object"):
		case _.fhhl("array/object/text"):
		case _.fhhl("object/array/text"):
		case _.fhhl("text/array/object"):
		case _.fhhl("text/object/array"):
		case _.fhhl("object/text/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::text          }};
			return _;
		}
		case _.fhhl("array/time/object"):
		case _.fhhl("array/object/time"):
		case _.fhhl("object/array/time"):
		case _.fhhl("time/array/object"):
		case _.fhhl("time/object/array"):
		case _.fhhl("object/time/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::time          }};
			return _;
		}
		case _.fhhl("array/timedelta/object"):
		case _.fhhl("array/object/timedelta"):
		case _.fhhl("object/array/timedelta"):
		case _.fhhl("timedelta/array/object"):
		case _.fhhl("timedelta/object/array"):
		case _.fhhl("object/timedelta/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::timedelta     }};
			return _;
		}
		case _.fhhl("array/uuid/object"):
		case _.fhhl("array/object/uuid"):
		case _.fhhl("object/array/uuid"):
		case _.fhhl("uuid/array/object"):
		case _.fhhl("uuid/object/array"):
		case _.fhhl("object/uuid/array"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::array, FieldType::uuid          }};
			return _;
		}
		case _.fhhl("boolean/object"):
		case _.fhhl("object/boolean"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::boolean       }};
			return _;
		}
		case _.fhhl("date/object"):
		case _.fhhl("object/date"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::date          }};
			return _;
		}
		case _.fhhl("datetime/object"):
		case _.fhhl("object/datetime"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::datetime      }};
			return _;
		}
		case _.fhhl("float/object"):
		case _.fhhl("object/float"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::floating      }};
			return _;
		}
		case _.fhhl("geospatial/object"):
		case _.fhhl("object/geospatial"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::geo           }};
			return _;
		}
		case _.fhhl("integer/object"):
		case _.fhhl("object/integer"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::integer       }};
			return _;
		}
		case _.fhhl("positive/object"):
		case _.fhhl("object/positive"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::positive      }};
			return _;
		}
		case _.fhhl("string/object"):
		case _.fhhl("object/string"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::string        }};
			return _;
		}
		case _.fhhl("term/object"):  // FIXME: remove legacy term
		case _.fhhl("object/term"):  // FIXME: remove legacy term
		case _.fhhl("keyword/object"):
		case _.fhhl("object/keyword"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::keyword       }};
			return _;
		}
		case _.fhhl("text/object"):
		case _.fhhl("object/text"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::text          }};
			return _;
		}
		case _.fhhl("time/object"):
		case _.fhhl("object/time"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::time          }};
			return _;
		}
		case _.fhhl("timedelta/object"):
		case _.fhhl("object/timedelta"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::timedelta     }};
			return _;
		}
		case _.fhhl("uuid/object"):
		case _.fhhl("object/uuid"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::object, FieldType::empty, FieldType::uuid          }};
			return _;
		}
		case _.fhhl("foreign"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::foreign, FieldType::empty,  FieldType::empty, FieldType::empty         }};
			return _;
		}
		case _.fhhl("object/foreign"):
		case _.fhhl("foreign/object"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::foreign, FieldType::object, FieldType::empty, FieldType::empty         }};
			return _;
		}
		case _.fhhl("script"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::script        }};
			return _;
		}
		default:
		case _.fhhl("undefined"): {
			static const std::array<FieldType, SPC_TOTAL_TYPES> _{{ FieldType::empty,   FieldType::empty,  FieldType::empty, FieldType::empty         }};
			return _;
		}
	}
}


static inline const std::string&
_get_str_index_uuid_field(UUIDFieldIndex index_uuid_field) noexcept
{
	switch (index_uuid_field) {
		case UUIDFieldIndex::uuid: {
			static const std::string uuid("uuid");
			return uuid;
		}
		case UUIDFieldIndex::uuid_field: {
			static const std::string uuid_field("uuid_field");
			return uuid_field;
		}
		case UUIDFieldIndex::both: {
			static const std::string both("both");
			return both;
		}
		default: {
			static const std::string unknown("unknown");
			return unknown;
		}
	}
}


static inline const std::string&
_get_str_type(const std::array<FieldType, SPC_TOTAL_TYPES>& sep_types)
{
	constexpr static auto _ = phf::make_phf({
		hh(EMPTY   + EMPTY   + EMPTY  + EMPTY),
		hh(EMPTY   + EMPTY   + ARRAY  + EMPTY),
		hh(EMPTY   + EMPTY   + ARRAY  + BOOLEAN),
		hh(EMPTY   + EMPTY   + ARRAY  + DATE),
		hh(EMPTY   + EMPTY   + ARRAY  + DATETIME),
		hh(EMPTY   + EMPTY   + ARRAY  + FLOATING),
		hh(EMPTY   + EMPTY   + ARRAY  + GEO),
		hh(EMPTY   + EMPTY   + ARRAY  + INTEGER),
		hh(EMPTY   + EMPTY   + ARRAY  + POSITIVE),
		hh(EMPTY   + EMPTY   + ARRAY  + STRING),
		hh(EMPTY   + EMPTY   + ARRAY  + KEYWORD),
		hh(EMPTY   + EMPTY   + ARRAY  + TEXT),
		hh(EMPTY   + EMPTY   + ARRAY  + TIME),
		hh(EMPTY   + EMPTY   + ARRAY  + TIMEDELTA),
		hh(EMPTY   + EMPTY   + ARRAY  + UUID),
		hh(EMPTY   + EMPTY   + EMPTY  + BOOLEAN),
		hh(EMPTY   + EMPTY   + EMPTY  + DATE),
		hh(EMPTY   + EMPTY   + EMPTY  + DATETIME),
		hh(EMPTY   + EMPTY   + EMPTY  + FLOATING),
		hh(FOREIGN + EMPTY   + EMPTY  + EMPTY),
		hh(FOREIGN + OBJECT  + EMPTY  + EMPTY),
		hh(FOREIGN + EMPTY   + EMPTY  + SCRIPT),
		hh(EMPTY   + EMPTY   + EMPTY  + GEO),
		hh(EMPTY   + EMPTY   + EMPTY  + INTEGER),
		hh(EMPTY   + OBJECT  + EMPTY  + EMPTY),
		hh(EMPTY   + OBJECT  + ARRAY  + EMPTY),
		hh(EMPTY   + OBJECT  + ARRAY  + BOOLEAN),
		hh(EMPTY   + OBJECT  + ARRAY  + DATE),
		hh(EMPTY   + OBJECT  + ARRAY  + DATETIME),
		hh(EMPTY   + OBJECT  + ARRAY  + FLOATING),
		hh(EMPTY   + OBJECT  + ARRAY  + GEO),
		hh(EMPTY   + OBJECT  + ARRAY  + INTEGER),
		hh(EMPTY   + OBJECT  + ARRAY  + POSITIVE),
		hh(EMPTY   + OBJECT  + ARRAY  + STRING),
		hh(EMPTY   + OBJECT  + ARRAY  + KEYWORD),
		hh(EMPTY   + OBJECT  + ARRAY  + TEXT),
		hh(EMPTY   + OBJECT  + ARRAY  + TIME),
		hh(EMPTY   + OBJECT  + ARRAY  + TIMEDELTA),
		hh(EMPTY   + OBJECT  + ARRAY  + UUID),
		hh(EMPTY   + OBJECT  + EMPTY  + BOOLEAN),
		hh(EMPTY   + OBJECT  + EMPTY  + DATE),
		hh(EMPTY   + OBJECT  + EMPTY  + DATETIME),
		hh(EMPTY   + OBJECT  + EMPTY  + FLOATING),
		hh(EMPTY   + OBJECT  + EMPTY  + GEO),
		hh(EMPTY   + OBJECT  + EMPTY  + INTEGER),
		hh(EMPTY   + OBJECT  + EMPTY  + POSITIVE),
		hh(EMPTY   + OBJECT  + EMPTY  + STRING),
		hh(EMPTY   + OBJECT  + EMPTY  + KEYWORD),
		hh(EMPTY   + OBJECT  + EMPTY  + TEXT),
		hh(EMPTY   + OBJECT  + EMPTY  + TIME),
		hh(EMPTY   + OBJECT  + EMPTY  + TIMEDELTA),
		hh(EMPTY   + OBJECT  + EMPTY  + UUID),
		hh(EMPTY   + EMPTY   + EMPTY  + POSITIVE),
		hh(EMPTY   + EMPTY   + EMPTY  + SCRIPT),
		hh(EMPTY   + EMPTY   + EMPTY  + STRING),
		hh(EMPTY   + EMPTY   + EMPTY  + KEYWORD),
		hh(EMPTY   + EMPTY   + EMPTY  + TEXT),
		hh(EMPTY   + EMPTY   + EMPTY  + TIME),
		hh(EMPTY   + EMPTY   + EMPTY  + TIMEDELTA),
		hh(EMPTY   + EMPTY   + EMPTY  + UUID),
	});

	switch (_.fhh(std::string_view(reinterpret_cast<const char*>(sep_types.data()), SPC_TOTAL_TYPES))) {
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + EMPTY): {
			static const std::string str_type("undefined");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + EMPTY): {
			static const std::string str_type("array");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + BOOLEAN): {
			static const std::string str_type("array/boolean");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + DATE): {
			static const std::string str_type("array/date");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + DATETIME): {
			static const std::string str_type("array/datetime");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + FLOATING): {
			static const std::string str_type("array/float");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + GEO): {
			static const std::string str_type("array/geospatial");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + INTEGER): {
			static const std::string str_type("array/integer");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + POSITIVE): {
			static const std::string str_type("array/positive");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + KEYWORD): {
			static const std::string str_type("array/keyword");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + STRING): {
			static const std::string str_type("array/string");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + TEXT): {
			static const std::string str_type("array/text");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + TIME): {
			static const std::string str_type("array/time");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + TIMEDELTA): {
			static const std::string str_type("array/timedelta");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + ARRAY  + UUID): {
			static const std::string str_type("array/uuid");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + BOOLEAN): {
			static const std::string str_type("boolean");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + DATE): {
			static const std::string str_type("date");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + DATETIME): {
			static const std::string str_type("datetime");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + FLOATING): {
			static const std::string str_type("floating");
			return str_type;
		}
		case _.fhh(FOREIGN + EMPTY   + EMPTY  + EMPTY): {
			static const std::string str_type("foreign");
			return str_type;
		}
		case _.fhh(FOREIGN + OBJECT  + EMPTY  + EMPTY): {
			static const std::string str_type("foreign/object");
			return str_type;
		}
		case _.fhh(FOREIGN + EMPTY   + EMPTY  + SCRIPT): {
			static const std::string str_type("foreign/script");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + GEO): {
			static const std::string str_type("geospatial");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + INTEGER): {
			static const std::string str_type("integer");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + EMPTY): {
			static const std::string str_type("object");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + EMPTY): {
			static const std::string str_type("object/array");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + BOOLEAN): {
			static const std::string str_type("object/array/boolean");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + DATE): {
			static const std::string str_type("object/array/date");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + DATETIME): {
			static const std::string str_type("object/array/datetime");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + FLOATING): {
			static const std::string str_type("object/array/float");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + GEO): {
			static const std::string str_type("object/array/geospatial");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + INTEGER): {
			static const std::string str_type("object/array/integer");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + POSITIVE): {
			static const std::string str_type("object/array/positive");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + STRING): {
			static const std::string str_type("object/array/string");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + KEYWORD): {
			static const std::string str_type("object/array/keyword");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + TEXT): {
			static const std::string str_type("object/array/text");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + TIME): {
			static const std::string str_type("object/array/time");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + TIMEDELTA): {
			static const std::string str_type("object/array/timedelta");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + ARRAY  + UUID): {
			static const std::string str_type("object/array/uuid");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + BOOLEAN): {
			static const std::string str_type("object/boolean");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + DATE): {
			static const std::string str_type("object/date");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + DATETIME): {
			static const std::string str_type("object/datetime");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + FLOATING): {
			static const std::string str_type("object/float");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + GEO): {
			static const std::string str_type("object/geospatial");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + INTEGER): {
			static const std::string str_type("object/integer");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + POSITIVE): {
			static const std::string str_type("object/positive");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + STRING): {
			static const std::string str_type("object/string");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + KEYWORD): {
			static const std::string str_type("object/keyword");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + TEXT): {
			static const std::string str_type("object/text");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + TIME): {
			static const std::string str_type("object/time");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + TIMEDELTA): {
			static const std::string str_type("object/timedelta");
			return str_type;
		}
		case _.fhh(EMPTY   + OBJECT  + EMPTY  + UUID): {
			static const std::string str_type("object/uuid");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + POSITIVE): {
			static const std::string str_type("positive");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + SCRIPT): {
			static const std::string str_type("script");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + STRING): {
			static const std::string str_type("string");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + KEYWORD): {
			static const std::string str_type("keyword");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + TEXT): {
			static const std::string str_type("text");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + TIME): {
			static const std::string str_type("time");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + TIMEDELTA): {
			static const std::string str_type("timedelta");
			return str_type;
		}
		case _.fhh(EMPTY   + EMPTY   + EMPTY  + UUID): {
			static const std::string str_type("uuid");
			return str_type;
		}
		default: {
			std::string result;
			if (sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
				result += enum_name(sep_types[SPC_FOREIGN_TYPE]);
			}
			if (sep_types[SPC_OBJECT_TYPE] == FieldType::object) {
				if (!result.empty()) { result += "/"; }
				result += enum_name(sep_types[SPC_OBJECT_TYPE]);
			}
			if (sep_types[SPC_ARRAY_TYPE] == FieldType::array) {
				if (!result.empty()) { result += "/"; }
				result += enum_name(sep_types[SPC_ARRAY_TYPE]);
			}
			if (sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
				if (!result.empty()) { result += "/"; }
				result += enum_name(sep_types[SPC_CONCRETE_TYPE]);
			}
			THROW(ClientError, "{} not supported.", repr(result), RESERVED_TYPE);
		}
	}
}


/*
 *  Function to generate a prefix given an field accuracy.
 */

static inline std::pair<std::string, FieldType>
_get_acc_data(std::string_view field_acc)
{
	auto accuracy_date = _get_accuracy_datetime(field_acc.substr(1));
	if (accuracy_date != UnitTime::INVALID) {
		return std::make_pair(get_prefix(toUType(accuracy_date)), FieldType::datetime);
	}
	try {
		switch (field_acc[1]) {
			case 'g':
				if (field_acc[2] == 'e' && field_acc[3] == 'o') {
					return std::make_pair(get_prefix(strict_stoull(field_acc.substr(4))), FieldType::geo);
				}
				break;
			case 't':
				if (field_acc[2] == 'd') {
					return std::make_pair(get_prefix(toUType(_get_accuracy_time(field_acc.substr(3)))), FieldType::timedelta);
				}
				return std::make_pair(get_prefix(toUType(_get_accuracy_time(field_acc.substr(2)))), FieldType::time);
			default:
				return std::make_pair(get_prefix(strict_stoull(field_acc.substr(1))), FieldType::integer);
		}
	} catch (const std::invalid_argument&) {
	} catch (const std::out_of_range&) { }

	THROW(ClientError, "The field name: {} is not valid", repr(field_acc));
}


/*
 * Default acc_prefixes for global values.
 */

static std::vector<std::string>
get_acc_prefix(const std::vector<uint64_t> accuracy)
{
	std::vector<std::string> res;
	res.reserve(accuracy.size());
	for (const auto& acc : accuracy) {
		res.push_back(get_prefix(acc));
	}
	return res;
}

static const std::vector<std::string> global_acc_prefix_num(get_acc_prefix(def_accuracy_num));
static const std::vector<std::string> global_acc_prefix_date(get_acc_prefix(def_accuracy_datetime));
static const std::vector<std::string> global_acc_prefix_time(get_acc_prefix(def_accuracy_time));
static const std::vector<std::string> global_acc_prefix_geo(get_acc_prefix(def_accuracy_geo));


/*
 * Acceptable values string used when there is a data inconsistency.
 */


specification_t default_spc;


static inline const std::pair<bool, const std::string>&
_get_stem_language(std::string_view str_stem_language)
{
	constexpr static auto _ = phf::make_phf({
		hhl("armenian"),
		hhl("hy"),
		hhl("basque"),
		hhl("eu"),
		hhl("catalan"),
		hhl("ca"),
		hhl("danish"),
		hhl("da"),
		hhl("dutch"),
		hhl("nl"),
		hhl("kraaij_pohlmann"),
		hhl("english"),
		hhl("en"),
		hhl("earlyenglish"),
		hhl("english_lovins"),
		hhl("lovins"),
		hhl("english_porter"),
		hhl("porter"),
		hhl("finnish"),
		hhl("fi"),
		hhl("french"),
		hhl("fr"),
		hhl("german"),
		hhl("de"),
		hhl("german2"),
		hhl("hungarian"),
		hhl("hu"),
		hhl("italian"),
		hhl("it"),
		hhl("norwegian"),
		hhl("nb"),
		hhl("nn"),
		hhl("no"),
		hhl("portuguese"),
		hhl("pt"),
		hhl("romanian"),
		hhl("ro"),
		hhl("russian"),
		hhl("ru"),
		hhl("spanish"),
		hhl("es"),
		hhl("swedish"),
		hhl("sv"),
		hhl("turkish"),
		hhl("tr"),
		hhl("none"),
		hhl(""),
	});

	switch(_.fhhl(str_stem_language)) {
		case _.fhhl("armenian"): {
			static const std::pair<bool, const std::string> hy{ true,  "hy" };
			return hy;
		}
		case _.fhhl("hy"): {
			static const std::pair<bool, const std::string> hy{ true,  "hy" };
			return hy;
		}
		case _.fhhl("basque"): {
			static const std::pair<bool, const std::string> ue{ true,  "ue" };
			return ue;
		}
		case _.fhhl("eu"): {
			static const std::pair<bool, const std::string> eu{ true,  "eu" };
			return eu;
		}
		case _.fhhl("catalan"): {
			static const std::pair<bool, const std::string> ca{ true,  "ca" };
			return ca;
		}
		case _.fhhl("ca"): {
			static const std::pair<bool, const std::string> ca{ true,  "ca" };
			return ca;
		}
		case _.fhhl("danish"): {
			static const std::pair<bool, const std::string> da{ true,  "da" };
			return da;
		}
		case _.fhhl("da"): {
			static const std::pair<bool, const std::string> da{ true,  "da" };
			return da;
		}
		case _.fhhl("dutch"): {
			static const std::pair<bool, const std::string> nl{ true,  "nl" };
			return nl;
		}
		case _.fhhl("nl"): {
			static const std::pair<bool, const std::string> nl{ true,  "nl" };
			return nl;
		}
		case _.fhhl("kraaij_pohlmann"): {
			static const std::pair<bool, const std::string> nl{ false, "nl" };
			return nl;
		}
		case _.fhhl("english"): {
			static const std::pair<bool, const std::string> en{ true,  "en" };
			return en;
		}
		case _.fhhl("en"): {
			static const std::pair<bool, const std::string> en{ true,  "en" };
			return en;
		}
		case _.fhhl("earlyenglish"): {
			static const std::pair<bool, const std::string> en{ false, "en" };
			return en;
		}
		case _.fhhl("english_lovins"): {
			static const std::pair<bool, const std::string> en{ false, "en" };
			return en;
		}
		case _.fhhl("lovins"): {
			static const std::pair<bool, const std::string> en{ false, "en" };
			return en;
		}
		case _.fhhl("english_porter"): {
			static const std::pair<bool, const std::string> en{ false, "en" };
			return en;
		}
		case _.fhhl("porter"): {
			static const std::pair<bool, const std::string> en{ false, "en" };
			return en;
		}
		case _.fhhl("finnish"): {
			static const std::pair<bool, const std::string> fi{ true,  "fi" };
			return fi;
		}
		case _.fhhl("fi"): {
			static const std::pair<bool, const std::string> fi{ true,  "fi" };
			return fi;
		}
		case _.fhhl("french"): {
			static const std::pair<bool, const std::string> fr{ true,  "fr" };
			return fr;
		}
		case _.fhhl("fr"): {
			static const std::pair<bool, const std::string> fr{ true,  "fr" };
			return fr;
		}
		case _.fhhl("german"): {
			static const std::pair<bool, const std::string> de{ true,  "de" };
			return de;
		}
		case _.fhhl("de"): {
			static const std::pair<bool, const std::string> de{ true,  "de" };
			return de;
		}
		case _.fhhl("german2"): {
			static const std::pair<bool, const std::string> de{ false, "de" };
			return de;
		}
		case _.fhhl("hungarian"): {
			static const std::pair<bool, const std::string> hu{ true,  "hu" };
			return hu;
		}
		case _.fhhl("hu"): {
			static const std::pair<bool, const std::string> hu{ true,  "hu" };
			return hu;
		}
		case _.fhhl("italian"): {
			static const std::pair<bool, const std::string> it{ true,  "it" };
			return it;
		}
		case _.fhhl("it"): {
			static const std::pair<bool, const std::string> it{ true,  "it" };
			return it;
		}
		case _.fhhl("norwegian"): {
			static const std::pair<bool, const std::string> no{ true,  "no" };
			return no;
		}
		case _.fhhl("nb"): {
			static const std::pair<bool, const std::string> no{ false, "no" };
			return no;
		}
		case _.fhhl("nn"): {
			static const std::pair<bool, const std::string> no{ false, "no" };
			return no;
		}
		case _.fhhl("no"): {
			static const std::pair<bool, const std::string> no{ true,  "no" };
			return no;
		}
		case _.fhhl("portuguese"): {
			static const std::pair<bool, const std::string> pt{ true,  "pt" };
			return pt;
		}
		case _.fhhl("pt"): {
			static const std::pair<bool, const std::string> pt{ true,  "pt" };
			return pt;
		}
		case _.fhhl("romanian"): {
			static const std::pair<bool, const std::string> ro{ true,  "ro" };
			return ro;
		}
		case _.fhhl("ro"): {
			static const std::pair<bool, const std::string> ro{ true,  "ro" };
			return ro;
		}
		case _.fhhl("russian"): {
			static const std::pair<bool, const std::string> ru{ true,  "ru" };
			return ru;
		}
		case _.fhhl("ru"): {
			static const std::pair<bool, const std::string> ru{ true,  "ru" };
			return ru;
		}
		case _.fhhl("spanish"): {
			static const std::pair<bool, const std::string> es{ true,  "es" };
			return es;
		}
		case _.fhhl("es"): {
			static const std::pair<bool, const std::string> es{ true,  "es" };
			return es;
		}
		case _.fhhl("swedish"): {
			static const std::pair<bool, const std::string> sv{ true,  "sv" };
			return sv;
		}
		case _.fhhl("sv"): {
			static const std::pair<bool, const std::string> sv{ true,  "sv" };
			return sv;
		}
		case _.fhhl("turkish"): {
			static const std::pair<bool, const std::string> tr{ true,  "tr" };
			return tr;
		}
		case _.fhhl("tr"): {
			static const std::pair<bool, const std::string> tr{ true,  "tr" };
			return tr;
		}
		case _.fhhl("none"):
		case _.fhhl(""): {
			static const std::pair<bool, const std::string> _{ true, "" };
			return _;
		}
		default: {
			static const std::pair<bool, const std::string> _{ false, "unknown" };
			return _;
		}
	}
}


std::string
repr_field(std::string_view name, std::string_view field_name)
{
	return name == field_name ? repr(name) : strings::format("{} ({})", repr(name), repr(field_name));
}


bool has_dispatch_set_default_spc(uint32_t key);
bool has_dispatch_process_properties(uint32_t key);
bool has_dispatch_process_concrete_properties(uint32_t key);


required_spc_t::flags_t::flags_t()
	: bool_term(DEFAULT_BOOL_TERM),
	  partials(DEFAULT_GEO_PARTIALS),
	  store(true),
	  parent_store(true),
	  recurse(true),
	  dynamic(true),
	  strict(false),
	  date_detection(true),
	  datetime_detection(true),
	  time_detection(true),
	  timedelta_detection(true),
	  numeric_detection(true),
	  geo_detection(true),
	  bool_detection(true),
	  text_detection(true),
	  uuid_detection(true),
	  partial_paths(false),
	  is_namespace(false),
	  ngram(false),
	  cjk_ngram(false),
	  cjk_words(false),
	  field_found(true),
	  concrete(false),
	  complete(false),
	  uuid_field(false),
	  uuid_path(false),
	  inside_namespace(false),
#ifdef XAPIAND_CHAISCRIPT
	  normalized_script(false),
#endif
	  has_uuid_prefix(false),
	  has_bool_term(false),
	  has_index(false),
	  has_namespace(false),
	  has_partial_paths(false),
	  static_endpoint(false) { }


std::string
required_spc_t::prefix_t::to_string() const
{
	auto res = repr(field);
	if (uuid.empty()) {
		return res;
	}
	res.insert(0, 1, '(').append(", ").append(repr(uuid)).push_back(')');
	return res;
}


std::string
required_spc_t::prefix_t::operator()() const noexcept
{
	return field;
}


required_spc_t::required_spc_t()
	: sep_types({{ FieldType::empty, FieldType::empty, FieldType::empty, FieldType::empty }}),
	  slot(Xapian::BAD_VALUENO),
	  stop_strategy(DEFAULT_STOP_STRATEGY),
	  stem_strategy(DEFAULT_STEM_STRATEGY),
	  error(DEFAULT_GEO_ERROR) { }


required_spc_t::required_spc_t(Xapian::valueno slot, FieldType type, std::vector<uint64_t> accuracy, std::vector<std::string> acc_prefix)
	: sep_types({{ FieldType::empty, FieldType::empty, FieldType::empty, type }}),
	  slot(slot),
	  accuracy(std::move(accuracy)),
	  acc_prefix(std::move(acc_prefix)),
	  stop_strategy(DEFAULT_STOP_STRATEGY),
	  stem_strategy(DEFAULT_STEM_STRATEGY),
	  error(DEFAULT_GEO_ERROR) { }


required_spc_t::required_spc_t(const required_spc_t& o) = default;


required_spc_t::required_spc_t(required_spc_t&& o) noexcept
	: sep_types(std::move(o.sep_types)),
	  prefix(std::move(o.prefix)),
	  slot(std::move(o.slot)),
	  flags(std::move(o.flags)),
	  accuracy(std::move(o.accuracy)),
	  acc_prefix(std::move(o.acc_prefix)),
	  ignored(std::move(o.ignored)),
	  language(std::move(o.language)),
	  stop_strategy(std::move(o.stop_strategy)),
	  stem_strategy(std::move(o.stem_strategy)),
	  stem_language(std::move(o.stem_language)),
	  error(std::move(o.error)) { }


required_spc_t&
required_spc_t::operator=(const required_spc_t& o) = default;


required_spc_t&
required_spc_t::operator=(required_spc_t&& o) noexcept
{
	sep_types = std::move(o.sep_types);
	prefix = std::move(o.prefix);
	slot = std::move(o.slot);
	flags = std::move(o.flags);
	accuracy = std::move(o.accuracy);
	acc_prefix = std::move(o.acc_prefix);
	ignored = std::move(o.ignored);
	language = std::move(o.language);
	stop_strategy = std::move(o.stop_strategy);
	stem_strategy = std::move(o.stem_strategy);
	stem_language = std::move(o.stem_language);
	error = std::move(o.error);
	return *this;
}


const std::array<FieldType, SPC_TOTAL_TYPES>&
required_spc_t::get_types(std::string_view str_type)
{
	L_CALL("required_spc_t::get_types({})", repr(str_type));

	const auto& type = _get_type(str_type);
	if (std::string_view(reinterpret_cast<const char*>(type.data()), SPC_TOTAL_TYPES) == (EMPTY + EMPTY + EMPTY + EMPTY)) {
		THROW(ClientError, "{} not supported, '{}' must be one of {{ 'date', 'datetime', 'float', 'geospatial', 'integer', 'positive', 'script', 'keyword', 'string', 'text', 'time', 'timedelta', 'uuid' }} or any of their {{ 'object/<type>', 'array/<type>', 'object/array/<type>', 'foreign/<type>', 'foreign/object/<type>,', 'foreign/array/<type>', 'foreign/object/array/<type>' }} variations.", repr(str_type), RESERVED_TYPE);
	}
	return type;
}


const std::string&
required_spc_t::get_str_type(const std::array<FieldType, SPC_TOTAL_TYPES>& sep_types)
{
	L_CALL("required_spc_t::get_str_type({{ {}, {}, {}, {} }})", toUType(sep_types[SPC_FOREIGN_TYPE]), toUType(sep_types[SPC_OBJECT_TYPE]), toUType(sep_types[SPC_ARRAY_TYPE]), toUType(sep_types[SPC_CONCRETE_TYPE]));

	return _get_str_type(sep_types);
}


void
required_spc_t::set_types(std::string_view str_type)
{
	L_CALL("required_spc_t::set_types({})", repr(str_type));

	sep_types = get_types(str_type);
}


MsgPack
required_spc_t::to_obj() const
{
	MsgPack obj;

	// required_spc_t

	obj["type"] = _get_str_type(sep_types);
	obj["prefix"] = prefix.to_string();
	obj["slot"] = slot;

	auto& obj_flags = obj["flags"] = MsgPack::MAP();
	obj_flags["bool_term"] = flags.bool_term;
	obj_flags["partials"] = flags.partials;

	obj_flags["store"] = flags.store;
	obj_flags["parent_store"] = flags.parent_store;
	obj_flags["recurse"] = flags.recurse;
	obj_flags["dynamic"] = flags.dynamic;
	obj_flags["strict"] = flags.strict;
	obj_flags["date_detection"] = flags.date_detection;
	obj_flags["datetime_detection"] = flags.datetime_detection;
	obj_flags["time_detection"] = flags.time_detection;
	obj_flags["timedelta_detection"] = flags.timedelta_detection;
	obj_flags["numeric_detection"] = flags.numeric_detection;
	obj_flags["geo_detection"] = flags.geo_detection;
	obj_flags["bool_detection"] = flags.bool_detection;
	obj_flags["text_detection"] = flags.text_detection;
	obj_flags["uuid_detection"] = flags.uuid_detection;

	obj_flags["partial_paths"] = flags.partial_paths;
	obj_flags["is_namespace"] = flags.is_namespace;

	obj_flags["field_found"] = flags.field_found;
	obj_flags["concrete"] = flags.concrete;
	obj_flags["complete"] = flags.complete;
	obj_flags["uuid_field"] = flags.uuid_field;
	obj_flags["uuid_path"] = flags.uuid_path;
	obj_flags["inside_namespace"] = flags.inside_namespace;
#ifdef XAPIAND_CHAISCRIPT
	obj_flags["normalized_script"] = flags.normalized_script;
#endif
	obj_flags["has_uuid_prefix"] = flags.has_uuid_prefix;
	obj_flags["has_bool_term"] = flags.has_bool_term;
	obj_flags["has_index"] = flags.has_index;
	obj_flags["has_namespace"] = flags.has_namespace;
	obj_flags["has_partial_paths"] = flags.has_partial_paths;
	obj_flags["static_endpoint"] = flags.static_endpoint;
	obj_flags["ngram"] = flags.ngram;
	obj_flags["cjk_ngram"] = flags.cjk_ngram;
	obj_flags["cjk_words"] = flags.cjk_words;

	auto& obj_accuracy = obj["accuracy"] = MsgPack::ARRAY();
	for (const auto& a : accuracy) {
		obj_accuracy.append(a);
	}

	auto& obj_acc_prefix = obj["acc_prefix"] = MsgPack::ARRAY();
	for (const auto& a : acc_prefix) {
		obj_acc_prefix.append(a);
	}

	auto& obj_ignore = obj["ignored"] = MsgPack::ARRAY();
	for (const auto& a : ignored) {
		obj_ignore.append(a);
	}

	obj["language"] = language;
	obj["stop_strategy"] = enum_name(stop_strategy);
	obj["stem_strategy"] = enum_name(stem_strategy);
	obj["stem_language"] = stem_language;

	obj["error"] = error;

	return obj;
}


std::string
required_spc_t::to_string(int indent) const
{
	return to_obj().to_string(indent);
}


index_spc_t::index_spc_t(required_spc_t&& spc)
	: type(std::move(spc.sep_types[SPC_CONCRETE_TYPE])),
	  prefix(std::move(spc.prefix.field)),
	  slot(std::move(spc.slot)),
	  accuracy(std::move(spc.accuracy)),
	  acc_prefix(std::move(spc.acc_prefix)) { }


index_spc_t::index_spc_t(const required_spc_t& spc)
	: type(spc.sep_types[SPC_CONCRETE_TYPE]),
	  prefix(spc.prefix.field),
	  slot(spc.slot),
	  accuracy(spc.accuracy),
	  acc_prefix(spc.acc_prefix) { }


specification_t::specification_t()
	: position({ 0 }),
	  weight({ 1 }),
	  spelling({ DEFAULT_SPELLING }),
	  positions({ DEFAULT_POSITIONS }),
	  index(DEFAULT_INDEX),
	  index_uuid_field(DEFAULT_INDEX_UUID_FIELD) { }


specification_t::specification_t(Xapian::valueno slot, FieldType type, const std::vector<uint64_t>& accuracy, const std::vector<std::string>& acc_prefix)
	: required_spc_t(slot, type, accuracy, acc_prefix),
	  position({ 0 }),
	  weight({ 1 }),
	  spelling({ DEFAULT_SPELLING }),
	  positions({ DEFAULT_POSITIONS }),
	  index(DEFAULT_INDEX),
	  index_uuid_field(DEFAULT_INDEX_UUID_FIELD) { }


specification_t::specification_t(const specification_t& o)
	: required_spc_t(o),
	  local_prefix(o.local_prefix),
	  position(o.position),
	  weight(o.weight),
	  spelling(o.spelling),
	  positions(o.positions),
	  index(o.index),
	  index_uuid_field(o.index_uuid_field),
	  meta_name(o.meta_name),
	  full_meta_name(o.full_meta_name),
	  aux_stem_language(o.aux_stem_language),
	  aux_language(o.aux_language),
	  partial_prefixes(o.partial_prefixes),
	  partial_index_spcs(o.partial_index_spcs) { }


specification_t::specification_t(specification_t&& o) noexcept
	: required_spc_t(std::move(o)),
	  local_prefix(std::move(o.local_prefix)),
	  position(std::move(o.position)),
	  weight(std::move(o.weight)),
	  spelling(std::move(o.spelling)),
	  positions(std::move(o.positions)),
	  index(std::move(o.index)),
	  index_uuid_field(std::move(o.index_uuid_field)),
	  meta_name(std::move(o.meta_name)),
	  full_meta_name(std::move(o.full_meta_name)),
	  aux_stem_language(std::move(o.aux_stem_language)),
	  aux_language(std::move(o.aux_language)),
	  partial_prefixes(std::move(o.partial_prefixes)),
	  partial_index_spcs(std::move(o.partial_index_spcs)) { }


specification_t&
specification_t::operator=(const specification_t& o)
{
	local_prefix = o.local_prefix;
	position = o.position;
	weight = o.weight;
	spelling = o.spelling;
	positions = o.positions;
	index = o.index;
	index_uuid_field = o.index_uuid_field;
	value_rec.reset();
	value.reset();
	doc_acc.reset();
#ifdef XAPIAND_CHAISCRIPT
	script.reset();
#endif
	meta_name = o.meta_name;
	full_meta_name = o.full_meta_name;
	aux_stem_language = o.aux_stem_language;
	aux_language = o.aux_language;
	partial_prefixes = o.partial_prefixes;
	partial_index_spcs = o.partial_index_spcs;
	required_spc_t::operator=(o);
	return *this;
}


specification_t&
specification_t::operator=(specification_t&& o) noexcept
{
	local_prefix = std::move(o.local_prefix);
	position = std::move(o.position);
	weight = std::move(o.weight);
	spelling = std::move(o.spelling);
	positions = std::move(o.positions);
	index = std::move(o.index);
	index_uuid_field = std::move(o.index_uuid_field);
	value_rec.reset();
	value.reset();
	doc_acc.reset();
#ifdef XAPIAND_CHAISCRIPT
	script.reset();
#endif
	meta_name = std::move(o.meta_name);
	full_meta_name = std::move(o.full_meta_name);
	aux_stem_language = std::move(o.aux_stem_language);
	aux_language = std::move(o.aux_language);
	partial_prefixes = std::move(o.partial_prefixes);
	partial_index_spcs = std::move(o.partial_index_spcs);
	required_spc_t::operator=(std::move(o));
	return *this;
}


FieldType
specification_t::global_type(FieldType field_type)
{
	switch (field_type) {
		case FieldType::floating:
		case FieldType::integer:
		case FieldType::positive:
		case FieldType::boolean:
		case FieldType::date:
		case FieldType::datetime:
		case FieldType::time:
		case FieldType::timedelta:
		case FieldType::geo:
		case FieldType::uuid:
		case FieldType::keyword:
			return field_type;

		case FieldType::string:
		case FieldType::text:
			return FieldType::text;

		default:
			THROW(ClientError, "Type: {:#04x} is an unknown type", toUType(field_type));
	}
}


const specification_t&
specification_t::get_global(FieldType field_type)
{
	switch (field_type) {
		case FieldType::floating: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::floating, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::integer: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::integer, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::positive: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::positive, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::boolean: {
			static const specification_t spc(DB_SLOT_BOOLEAN, FieldType::boolean, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::date: {
			static const specification_t spc(DB_SLOT_DATE, FieldType::date, def_accuracy_datetime, global_acc_prefix_date);
			return spc;
		}
		case FieldType::datetime: {
			static const specification_t spc(DB_SLOT_DATE, FieldType::datetime, def_accuracy_datetime, global_acc_prefix_date);
			return spc;
		}
		case FieldType::time: {
			static const specification_t spc(DB_SLOT_TIME, FieldType::time, def_accuracy_time, global_acc_prefix_time);
			return spc;
		}
		case FieldType::timedelta: {
			static const specification_t spc(DB_SLOT_TIMEDELTA, FieldType::timedelta, def_accuracy_time, global_acc_prefix_time);
			return spc;
		}
		case FieldType::geo: {
			static const specification_t spc(DB_SLOT_GEO, FieldType::geo, def_accuracy_geo, global_acc_prefix_geo);
			return spc;
		}
		case FieldType::uuid: {
			static const specification_t spc(DB_SLOT_UUID, FieldType::uuid, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::keyword: {
			static const specification_t spc(DB_SLOT_STRING, FieldType::keyword, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::string:
		case FieldType::text: {
			static const specification_t spc(DB_SLOT_STRING, FieldType::text, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		default:
			THROW(ClientError, "Type: {:#04x} is an unknown type", toUType(field_type));
	}
}


void
specification_t::update(index_spc_t&& spc)
{
	sep_types[SPC_CONCRETE_TYPE] = std::move(spc.type);
	prefix.field = std::move(spc.prefix);
	slot = std::move(spc.slot);
	accuracy = std::move(spc.accuracy);
	acc_prefix = std::move(spc.acc_prefix);
}


void
specification_t::update(const index_spc_t& spc)
{
	sep_types[SPC_CONCRETE_TYPE] = spc.type;
	prefix.field = spc.prefix;
	slot = spc.slot;
	accuracy = spc.accuracy;
	acc_prefix = spc.acc_prefix;
}


MsgPack
specification_t::to_obj() const
{
	MsgPack obj = required_spc_t::to_obj();

	// specification_t

	obj["local_prefix"] = local_prefix.to_string();

	auto& obj_position = obj["position"] = MsgPack::ARRAY();
	for (const auto& p : position) {
		obj_position.append(p);
	}

	auto& obj_weight = obj["weight"] = MsgPack::ARRAY();
	for (const auto& w : weight) {
		obj_weight.append(w);
	}

	auto& obj_spelling = obj["spelling"] = MsgPack::ARRAY();
	for (const auto& s : spelling) {
		obj_spelling.append(static_cast<bool>(s));
	}

	auto& obj_positions = obj["positions"] = MsgPack::ARRAY();
	for (const auto& p : positions) {
		obj_positions.append(static_cast<bool>(p));
	}

	obj["index"] = _get_str_index(index);

	obj["index_uuid_field"] = _get_str_index_uuid_field(index_uuid_field);

	obj["value_rec"] = value_rec ? value_rec->to_string() : MsgPack::NIL();
	obj["value"] = value ? value->to_string() : MsgPack::NIL();
	obj["doc_acc"] = doc_acc ? doc_acc->to_string() : MsgPack::NIL();
#ifdef XAPIAND_CHAISCRIPT
	obj["script"] = script ? script->to_string() : MsgPack::NIL();
#endif

	obj["endpoint"] = endpoint;

	obj["meta_name"] = meta_name;
	obj["full_meta_name"] = full_meta_name;

	obj["aux_stem_language"] = aux_stem_language;
	obj["aux_language"] = aux_language;

	auto& obj_partial_prefixes = obj["partial_prefixes"] = MsgPack::ARRAY();
	for (const auto& p : partial_prefixes) {
		obj_partial_prefixes.append(p.to_string());
	}

	auto& obj_partial_index_spcs = obj["partial_index_spcs"] = MsgPack::ARRAY();
	for (const auto& s : partial_index_spcs) {
		obj_partial_index_spcs.append(MsgPack({
			{ "prefix", repr(s.prefix) },
			{ "slot", s.slot },
		}));
	}

	return obj;
}


std::string
specification_t::to_string(int indent) const
{
	return to_obj().to_string(indent);
}


template <typename ErrorType>
std::pair<const MsgPack*, const MsgPack*>
Schema::check(const MsgPack& object, const char* prefix, bool allow_foreign, bool allow_root)
{
	L_CALL("Schema::check({}, <prefix>, allow_foreign:{}, allow_root:{})", repr(object.to_string()), allow_foreign, allow_root);

	if (object.empty()) {
		THROW(ErrorType, "{}Schema object is empty", prefix);
	}

	// Check foreign:
	if (allow_foreign) {
		if (object.is_string()) {
			return std::make_pair(&object, nullptr);
		}
		if (!object.is_map()) {
			THROW(ErrorType, "{}Schema must be a map", prefix);
		}
		auto it_end = object.end();
		auto type_it = object.find(RESERVED_TYPE);
		if (type_it != it_end) {
			auto& type = type_it.value();
			if (!type.is_string()) {
				THROW(ErrorType, "{}Schema field '{}' must be a string", prefix, RESERVED_TYPE);
			}
			auto type_name = type.str_view();
			const auto& sep_types = required_spc_t::get_types(type_name);
			if (sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
				auto endpoint_it = object.find(RESERVED_ENDPOINT);
				if (endpoint_it == it_end) {
					THROW(ErrorType, "{}Schema field '{}' does not exist", prefix, RESERVED_ENDPOINT);
				}
				auto& endpoint = endpoint_it.value();
				if (!endpoint.is_string()) {
					THROW(ErrorType, "{}Schema field '{}' must be a string", prefix, RESERVED_ENDPOINT);
				}
				return std::make_pair(&endpoint, &object);
			}
			if (sep_types[SPC_OBJECT_TYPE] != FieldType::object) {
				THROW(ErrorType, "{}Schema object has an unsupported type: {}", prefix, type_name);
			}
		}
	} else {
		if (!object.is_map()) {
			THROW(ErrorType, "{}Schema must be a map", prefix);
		}
	}

	auto it_end = object.end();

	// Check schema object:
	auto schema_it = object.find(SCHEMA_FIELD_NAME);
	if (schema_it == it_end) {
		if (!allow_root) {
			THROW(ErrorType, "{}Schema field '{}' does not exist", prefix, SCHEMA_FIELD_NAME);
		}
		return std::make_pair(nullptr, nullptr);
	}

	auto& schema = schema_it.value();
	if (!schema.is_map()) {
		THROW(ErrorType, "{}Schema field '{}' is not an object", prefix, SCHEMA_FIELD_NAME);
	}
	auto schema_it_end = schema.end();
	auto type_it = schema.find(RESERVED_TYPE);
	if (type_it != schema_it_end) {
		auto& type = type_it.value();
		if (!type.is_string()) {
			THROW(ErrorType, "{}Schema field '{}.{}' must be a string", prefix, SCHEMA_FIELD_NAME, RESERVED_TYPE);
		}
		auto type_name = type.str_view();
		const auto& sep_types = required_spc_t::get_types(type_name);
		if (sep_types[SPC_OBJECT_TYPE] != FieldType::object) {
			THROW(ErrorType, "{}Schema field '{}' has an unsupported type: {}", prefix, SCHEMA_FIELD_NAME, type_name);
		}
	}

	// Prevent schemas from having a '_schemas' field inside:
	auto reserved_schema_it = object.find(RESERVED_SCHEMA);
	if (reserved_schema_it != it_end) {
		THROW(ErrorType, "{}Schema field '{}' is not valid", prefix, RESERVED_SCHEMA);
	}

	return std::make_pair(nullptr, &schema);
}


Schema::Schema(std::shared_ptr<const MsgPack> s, std::unique_ptr<MsgPack> m, std::string o)
	: schema(std::move(s)),
	  mut_schema(std::move(m)),
	  origin(std::move(o))
{
	auto checked = check<Error>(*schema, "Schema is corrupt: ", true, false);
	if (checked.first != nullptr) {
		schema = get_initial_schema();
	}
}


std::shared_ptr<const MsgPack>
Schema::get_initial_schema()
{
	L_CALL("Schema::get_initial_schema()");

	static const MsgPack initial_schema_tpl({
		{ RESERVED_IGNORE, SCHEMA_FIELD_NAME },
		{ SCHEMA_FIELD_NAME, MsgPack::MAP() },
	});
	auto initial_schema = std::make_shared<const MsgPack>(initial_schema_tpl);
	initial_schema->lock();
	return initial_schema;
}


const MsgPack&
Schema::get_properties(std::string_view full_meta_name)
{
	L_CALL("Schema::get_properties({})", repr(full_meta_name));

	const MsgPack* prop = &get_properties();
	Split<std::string_view> field_names(full_meta_name, DB_OFFSPRING_UNION);
	for (const auto& field_name : field_names) {
		prop = &prop->at(field_name);
	}
	return *prop;
}


MsgPack&
Schema::get_mutable_properties(std::string_view full_meta_name)
{
	L_CALL("Schema::get_mutable_properties({})", repr(full_meta_name));

	MsgPack* prop = &get_mutable_properties();
	Split<std::string_view> field_names(full_meta_name, DB_OFFSPRING_UNION);
	for (const auto& field_name : field_names) {
		prop = &prop->get(field_name);
	}
	return *prop;
}


const MsgPack&
Schema::get_newest_properties(std::string_view full_meta_name)
{
	L_CALL("Schema::get_newest_properties({})", repr(full_meta_name));

	const MsgPack* prop = &get_newest_properties();
	Split<std::string_view> field_names(full_meta_name, DB_OFFSPRING_UNION);
	for (const auto& field_name : field_names) {
		prop = &prop->at(field_name);
	}
	return *prop;
}


MsgPack&
Schema::clear()
{
	L_CALL("Schema::clear()");

	auto& prop = get_mutable_properties();
	prop.clear();
	return prop;
}


inline void
Schema::restart_specification()
{
	L_CALL("Schema::restart_specification()");

	specification.flags.partials             = default_spc.flags.partials;
	specification.error                      = default_spc.error;

	specification.flags.ngram                = default_spc.flags.ngram;
	specification.flags.cjk_ngram            = default_spc.flags.cjk_ngram;
	specification.flags.cjk_words            = default_spc.flags.cjk_words;
	specification.language                   = default_spc.language;
	specification.stop_strategy              = default_spc.stop_strategy;
	specification.stem_strategy              = default_spc.stem_strategy;
	specification.stem_language              = default_spc.stem_language;

	specification.flags.bool_term            = default_spc.flags.bool_term;
	specification.flags.has_bool_term        = default_spc.flags.has_bool_term;
	specification.flags.has_index            = default_spc.flags.has_index;
	specification.flags.has_namespace        = default_spc.flags.has_namespace;
	specification.flags.static_endpoint      = default_spc.flags.static_endpoint;

	specification.flags.concrete             = default_spc.flags.concrete;
	specification.flags.complete             = default_spc.flags.complete;
	specification.flags.uuid_field           = default_spc.flags.uuid_field;

	specification.sep_types                  = default_spc.sep_types;
	specification.endpoint                   = default_spc.endpoint;
	specification.local_prefix               = default_spc.local_prefix;
	specification.slot                       = default_spc.slot;
	specification.accuracy                   = default_spc.accuracy;
	specification.acc_prefix                 = default_spc.acc_prefix;
	specification.aux_stem_language          = default_spc.aux_stem_language;
	specification.aux_language               = default_spc.aux_language;

	specification.ignored                    = default_spc.ignored;

	specification.partial_index_spcs         = default_spc.partial_index_spcs;
}


inline void
Schema::restart_namespace_specification()
{
	L_CALL("Schema::restart_namespace_specification()");

	specification.flags.bool_term            = default_spc.flags.bool_term;
	specification.flags.has_bool_term        = default_spc.flags.has_bool_term;
	specification.flags.static_endpoint      = default_spc.flags.static_endpoint;

	specification.flags.concrete             = default_spc.flags.concrete;
	specification.flags.complete             = default_spc.flags.complete;
	specification.flags.uuid_field           = default_spc.flags.uuid_field;

	specification.sep_types                  = default_spc.sep_types;
	specification.endpoint                   = default_spc.endpoint;
	specification.aux_stem_language          = default_spc.aux_stem_language;
	specification.aux_language               = default_spc.aux_language;

	specification.partial_index_spcs         = default_spc.partial_index_spcs;
}


struct FedSpecification : MsgPack::Data {
	specification_t specification;

	FedSpecification(specification_t  specification) : specification(std::move(specification)) { }
};


template <typename T>
inline bool
Schema::feed_subproperties(T& properties, std::string_view meta_name)
{
	L_CALL("Schema::feed_subproperties({}, {})", repr(properties->to_string()), repr(meta_name));

	auto it = properties->find(meta_name);
	if (it == properties->end()) {
		return false;
	}

	properties = &it.value();
	const auto data = std::static_pointer_cast<const FedSpecification>(properties->get_data());
	if (data) {
		// This is the feed cache
		auto local_prefix_uuid = specification.local_prefix.uuid;
		auto prefix = specification.prefix;
		specification = data->specification;
		specification.prefix = prefix;
		specification.local_prefix.uuid = local_prefix_uuid;
		return true;
	}

	specification.flags.field_found = true;

	const auto& stem = _get_stem_language(meta_name);
	if (stem.first && stem.second != "unknown") {
		specification.language = stem.second;
		specification.aux_language = stem.second;
	}

	if (specification.full_meta_name.empty()) {
		specification.full_meta_name.assign(meta_name);
	} else {
		specification.full_meta_name.append(1, DB_OFFSPRING_UNION).append(meta_name);
	}

	dispatch_feed_properties(*properties);

	properties->set_data(std::make_shared<const FedSpecification>(specification));

	return true;
}


/*  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 *      ___           _
 *     |_ _|_ __   __| | _____  __
 *      | || '_ \ / _` |/ _ \ \/ /
 *      | || | | | (_| |  __/>  <
 *     |___|_| |_|\__,_|\___/_/\_\
 *  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 */

std::tuple<std::string, Xapian::Document, MsgPack>
Schema::index(const MsgPack& object, MsgPack document_id, DatabaseHandler& db_handler, const Data& data)
{
	L_CALL("Schema::index({}, {}, <db_handler>)", repr(object.to_string()), repr(document_id.to_string()));

	static UUIDGenerator generator;

	L_INDEX("Schema index({}): " + DIM_GREY + "{}", document_id.to_string(), object.to_string());

	try {
		map_values.clear();
		specification = default_spc;
		specification.slot = DB_SLOT_ROOT;  // Set default RESERVED_SLOT for root

		FieldVector fields;
		fields.reserve(object.size());
		Field* id_field = nullptr;
		auto properties = &get_newest_properties();

		if (object.empty()) {
			dispatch_feed_properties(*properties);
		} else if (properties->empty()) {  // new schemas have empty properties
			specification.flags.field_found = false;
			auto mut_properties = &get_mutable_properties();
			dispatch_write_properties(*mut_properties, object, fields, &id_field);
			properties = &*mut_properties;
		} else {
			dispatch_feed_properties(*properties);
			dispatch_process_properties(object, fields, &id_field);
		}

		auto spc_id = get_data_id();
		if (id_field != nullptr && id_field->second != nullptr && id_field->second->is_map()) {
			_get_data_id(spc_id, *id_field->second);
		}
		auto id_type = spc_id.get_type();

		std::string unprefixed_term_id;
		std::string term_id;

		if (!document_id) {
			switch (id_type) {
				case FieldType::empty:
					id_type = FieldType::uuid;
					spc_id.set_type(id_type);
					set_data_id(spc_id);
					properties = &get_mutable_properties();
				[[fallthrough]];
				case FieldType::uuid: {
					size_t n_shards = db_handler.endpoints.size();
					size_t shard_num = 0;
					// Try getting a new ID which can currently be indexed (active node)
					// Get the least used shard:
					auto min_doccount = std::numeric_limits<Xapian::doccount>::max();
					for (size_t n = 0; n < n_shards; ++n) {
						auto& endpoint = db_handler.endpoints[n];
						auto node = endpoint.node();
						if (node && node->is_active()) {
							try {
								lock_shard lk_shard(endpoint, db_handler.flags);
								auto doccount = lk_shard->db()->get_doccount();
								if (min_doccount > doccount) {
									min_doccount = doccount;
									shard_num = n;
								}
							} catch (...) {}
						}
					}
					// Figure out a term which goes into the least used shard:
					for (int t = 10; t >= 0; --t) {
						auto tmp_unprefixed_term_id = generator(opts.uuid_compact).serialise();
						auto tmp_term_id = prefixed(tmp_unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
						auto tmp_shard_num = fnv1ah64::hash(tmp_term_id) % n_shards;
						auto& endpoint = db_handler.endpoints[tmp_shard_num];
						auto node = endpoint.node();
						if (node && node->is_active()) {
							unprefixed_term_id = std::move(tmp_unprefixed_term_id);
							term_id = std::move(tmp_term_id);
							if (shard_num == tmp_shard_num) {
								break;
							}
						}
					}
					document_id = Unserialise::uuid(unprefixed_term_id, static_cast<UUIDRepr>(opts.uuid_repr));
					break;
				}
				case FieldType::integer:
					document_id = MsgPack(0).as_i64();
					unprefixed_term_id = Serialise::serialise(spc_id, document_id);
					term_id = prefixed(unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
					break;
				case FieldType::positive:
					document_id = MsgPack(0).as_u64();
					unprefixed_term_id = Serialise::serialise(spc_id, document_id);
					term_id = prefixed(unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
					break;
				case FieldType::floating:
					document_id = MsgPack(0).as_f64();
					unprefixed_term_id = Serialise::serialise(spc_id, document_id);
					term_id = prefixed(unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
					break;
				case FieldType::text:
				case FieldType::string:
				case FieldType::keyword: {
					size_t n_shards = db_handler.endpoints.size();
					size_t shard_num = 0;
					// Try getting a new ID which can currently be indexed (active node)
					// Get the least used shard:
					auto min_doccount = std::numeric_limits<Xapian::doccount>::max();
					for (size_t n = 0; n < n_shards; ++n) {
						auto& endpoint = db_handler.endpoints[n];
						auto node = endpoint.node();
						if (node && node->is_active()) {
							try {
								lock_shard lk_shard(endpoint, db_handler.flags);
								auto doccount = lk_shard->db()->get_doccount();
								if (min_doccount > doccount) {
									min_doccount = doccount;
									shard_num = n;
								}
							} catch (...) {}
						}
					}
					// Figure out a term which goes into the least used shard:
					for (int t = 10; t >= 0; --t) {
						auto tmp_document_id = Base64::rfc4648url_unpadded().encode(generator(true).serialise());
						auto tmp_unprefixed_term_id = Serialise::serialise(spc_id, tmp_document_id);
						auto tmp_term_id = prefixed(tmp_unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
						auto tmp_shard_num = fnv1ah64::hash(tmp_term_id) % n_shards;
						auto& endpoint = db_handler.endpoints[tmp_shard_num];
						auto node = endpoint.node();
						if (node && node->is_active()) {
							document_id = std::move(tmp_document_id);
							unprefixed_term_id = std::move(tmp_unprefixed_term_id);
							term_id = std::move(tmp_term_id);
							if (shard_num == tmp_shard_num) {
								break;
							}
						}
					}
					break;
				}
				default:
					THROW(ClientError, "Invalid datatype for '{}'", ID_FIELD_NAME);
			}
		} else {
			// Get early term ID when possible
			switch (id_type) {
				case FieldType::empty: {
					const auto type_ser = Serialise::guess_serialise(document_id);
					id_type = type_ser.first;
					if (id_type == FieldType::text || id_type == FieldType::string) {
						id_type = FieldType::keyword;
					}
					spc_id.set_type(id_type);
					spc_id.flags.bool_term = true;
					set_data_id(spc_id);
					properties = &get_mutable_properties();
					unprefixed_term_id = type_ser.second;
					document_id = Cast::cast(id_type, document_id);
					break;
				}
				case FieldType::uuid:
				case FieldType::integer:
				case FieldType::positive:
				case FieldType::floating:
				case FieldType::text:
				case FieldType::string:
				case FieldType::keyword:
					document_id = Cast::cast(id_type, document_id);
					unprefixed_term_id = Serialise::serialise(spc_id, document_id);
					break;
				default:
					THROW(ClientError, "Invalid datatype for '{}'", ID_FIELD_NAME);
			}
			term_id = prefixed(unprefixed_term_id, spc_id.prefix(), spc_id.get_ctype());
		}

#ifdef XAPIAND_CHAISCRIPT
		std::unique_ptr<MsgPack> mut_object;
		if (specification.script) {
			mut_object = db_handler.call_script(object, term_id, *specification.script, data);
			if (mut_object != nullptr) {
				if (!mut_object->is_map()) {
					THROW(ClientError, "Script must return an object, it returned {}", enum_name(mut_object->get_type()));
				}
				// Rebuild fields with new values.
				fields.clear();
				fields.reserve(mut_object->size());
				id_field = nullptr;
				const auto it_e = mut_object->end();
				for (auto it = mut_object->begin(); it != it_e; ++it) {
					auto str_key = it->str_view();
					if (is_reserved(str_key)) {
						auto key = hh(str_key);
						if (!has_dispatch_process_properties(key)) {
							if (!has_dispatch_process_concrete_properties(key)) {
								fields.emplace_back(str_key, &it.value());
								if (key == hh(ID_FIELD_NAME)) {
									id_field = &fields.back();
								}
							}
						}
					} else {
						fields.emplace_back(str_key, &it.value());
					}
				}
			}
		}
#endif

		// Add ID field.
		MsgPack id_field_obj;
		if (id_field != nullptr && id_field->second != nullptr) {
			if (id_field->second->is_map()) {
				id_field_obj = *id_field->second;
				id_field_obj[RESERVED_VALUE] = document_id;
				id_field->second = &id_field_obj;
			} else {
				id_field->second = &document_id;
			}
		} else {
			fields.emplace_back(ID_FIELD_NAME, &document_id);
			id_field = &fields.back();
		}

		Xapian::Document doc;
		MsgPack data_obj;

		auto data_ptr = &data_obj;
		index_item_value(properties, doc, data_ptr, fields);

		for (const auto& elem : map_values) {
			const auto val_ser = StringList::serialise(elem.second.begin(), elem.second.end());
			doc.add_value(elem.first, val_ser);
			L_INDEX("Slot: {}  Values: {}", elem.first, repr(val_ser));
		}

		if (term_id != "QN\x80") {
			doc.add_boolean_term(term_id);  // make sure the ID term is ALWAYS added!
		}

		return std::make_tuple(std::move(term_id), std::move(doc), std::move(data_obj));
	} catch (...) {
		L_DEBUG("ERROR IN {}: {}", document_id.to_string(), object.to_string());
		mut_schema.reset();
		throw;
	}
}


const MsgPack&
Schema::index_subproperties(const MsgPack*& properties, MsgPack*& data, std::string_view name, const MsgPack& object, FieldVector& fields, size_t pos)
{
	L_CALL("Schema::index_subproperties({}, {}, {}, {}, <fields>, {})", repr(properties->to_string()), repr(data->to_string()), repr(name), repr(object.to_string()), pos);

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			detect_dynamic(field_name);
			update_prefixes();
			if (specification.flags.store) {
				auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
				data = &inserted.first.value();
			}
		}
		const auto& field_name = *it;
		dispatch_process_properties(object, fields);
		detect_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
		if (specification.flags.store) {
			auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
			if (!inserted.second && pos == 0) {
				THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			data = &inserted.first.value();
		}
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(properties, field_name)) {
				update_prefixes();
				if (specification.flags.store) {
					auto inserted = data->insert(field_name);
					data = &inserted.first.value();
				}
			} else {
				detect_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(properties, specification.meta_name)) {
						update_prefixes();
						if (specification.flags.store) {
							auto inserted = data->insert(normalize_uuid(field_name));
							data = &inserted.first.value();
						}
						continue;
					}
				}

				auto mut_properties = &get_mutable_properties(specification.full_meta_name);
				add_field(mut_properties);
				if (specification.flags.store) {
					auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
					data = &inserted.first.value();
				}

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						detect_dynamic(n_field_name);
						add_field(mut_properties);
						if (specification.flags.store) {
							auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(n_field_name) : n_field_name);
							data = &inserted.first.value();
						}
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					detect_dynamic(n_field_name);
					add_field(mut_properties, object, fields);
					if (specification.flags.store) {
						auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(n_field_name) : n_field_name);
						if (!inserted.second && pos == 0) {
							THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
						}
						data = &inserted.first.value();
					}
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(properties, field_name)) {
			dispatch_process_properties(object, fields);
			update_prefixes();
			if (specification.flags.store) {
				auto inserted = data->insert(field_name);
				if (!inserted.second && pos == 0) {
					THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				data = &inserted.first.value();
			}
		} else {
			detect_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(properties, specification.meta_name)) {
					dispatch_process_properties(object, fields);
					update_prefixes();
					if (specification.flags.store) {
						auto inserted = data->insert(normalize_uuid(field_name));
						if (!inserted.second && pos == 0) {
							THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
						}
						data = &inserted.first.value();
					}
					return *properties;
				}
			}

			auto mut_properties = &get_mutable_properties(specification.full_meta_name);
			add_field(mut_properties, object, fields);
			if (specification.flags.store) {
				auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
				if (!inserted.second && pos == 0) {
					THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				data = &inserted.first.value();
			}
			return *mut_properties;
		}
	}

	return *properties;
}


const MsgPack&
Schema::index_subproperties(const MsgPack*& properties, MsgPack*& data, std::string_view name, size_t pos)
{
	L_CALL("Schema::index_subproperties({}, {}, {}, {})", repr(properties->to_string()), repr(data->to_string()), repr(name), pos);

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			detect_dynamic(field_name);
			update_prefixes();
			if (specification.flags.store) {
				auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
				data = &inserted.first.value();
			}
		}
		const auto& field_name = *it;
		detect_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
		if (specification.flags.store) {
			auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
			if (!inserted.second && pos == 0) {
				THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			data = &inserted.first.value();
		}
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(properties, field_name)) {
				update_prefixes();
				if (specification.flags.store) {
					auto inserted = data->insert(field_name);
					data = &inserted.first.value();
				}
			} else {
				detect_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(properties, specification.meta_name)) {
						update_prefixes();
						if (specification.flags.store) {
							auto inserted = data->insert(normalize_uuid(field_name));
							data = &inserted.first.value();
						}
						continue;
					}
				}

				auto mut_properties = &get_mutable_properties(specification.full_meta_name);
				add_field(mut_properties);
				if (specification.flags.store) {
					auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
					data = &inserted.first.value();
				}

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						detect_dynamic(n_field_name);
						add_field(mut_properties);
						if (specification.flags.store) {
							auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(n_field_name) : n_field_name);
							data = &inserted.first.value();
						}
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					detect_dynamic(n_field_name);
					add_field(mut_properties);
					if (specification.flags.store) {
						auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(n_field_name) : n_field_name);
						if (!inserted.second && pos == 0) {
							THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
						}
						data = &inserted.first.value();
					}
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(properties, field_name)) {
			update_prefixes();
			if (specification.flags.store) {
				auto inserted = data->insert(field_name);
				if (!inserted.second && pos == 0) {
					THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				data = &inserted.first.value();
			}
		} else {
			detect_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(properties, specification.meta_name)) {
					update_prefixes();
					if (specification.flags.store) {
						auto inserted = data->insert(normalize_uuid(field_name));
						if (!inserted.second && pos == 0) {
							THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
						}
						data = &inserted.first.value();
					}
					return *properties;
				}
			}

			auto mut_properties = &get_mutable_properties(specification.full_meta_name);
			add_field(mut_properties);
			if (specification.flags.store) {
				auto inserted = data->insert(specification.flags.uuid_field ? normalize_uuid(field_name) : field_name);
				if (!inserted.second && pos == 0) {
					THROW(ClientError, "Field {} in {} is duplicated", repr_field(name, inserted.first->as_str()), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				data = &inserted.first.value();
			}
			return *mut_properties;
		}

	}

	return *properties;
}


void
Schema::index_object(const MsgPack*& parent_properties, const MsgPack& object, MsgPack*& parent_data, Xapian::Document& doc, const std::string& name)
{
	L_CALL("Schema::index_object({}, {}, {}, <Xapian::Document>, {})", repr(parent_properties->to_string()), repr(object.to_string()), repr(parent_data->to_string()), repr(name));

	if (is_comment(name)) {
		return;  // skip comments (empty fields or fields starting with '#')
	}

	if (is_valid(name) && (!specification.flags.recurse || specification.ignored.find(name) != specification.ignored.end())) {
		if (specification.flags.store) {
			parent_data->get(name) = object;
		}
		return;
	}

	switch (object.get_type()) {
		case MsgPack::Type::MAP: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			auto data = parent_data;
			FieldVector fields;
			properties = &index_subproperties(properties, data, name, object, fields, 0);
			index_item_value(properties, doc, data, fields);
			if (specification.flags.store) {
				if (data->is_map() && data->size() == 1) {
					auto it = data->find(RESERVED_VALUE);
					if (it != data->end()) {
						*data = it.value();
					}
				}
				if (data->is_undefined() || (data->is_map() && data->empty())) {
					parent_data->erase(name);
				}
			}
			specification = std::move(spc_start);
			break;
		}

		case MsgPack::Type::ARRAY: {
			index_array(parent_properties, object, parent_data, doc, name);
			break;
		}

		case MsgPack::Type::NIL:
		case MsgPack::Type::UNDEFINED: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			auto data = parent_data;
			index_subproperties(properties, data, name, 0);
			index_partial_paths(doc);
			if (specification.flags.store) {
				if (data->is_map() && data->size() == 1) {
					auto it = data->find(RESERVED_VALUE);
					if (it != data->end()) {
						*data = it.value();
					}
				}
				if (data->is_undefined() || (data->is_map() && data->empty())) {
					parent_data->erase(name);
				}
			}
			specification = std::move(spc_start);
			break;
		}

		default: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			auto data = parent_data;
			index_subproperties(properties, data, name, 0);
			index_item_value(doc, *data, object, 0);
			if (specification.flags.store) {
				if (data->is_map() && data->size() == 1) {
					auto it = data->find(RESERVED_VALUE);
					if (it != data->end()) {
						*data = it.value();
					}
				}
				if (data->is_undefined() || (data->is_map() && data->empty())) {
					parent_data->erase(name);
				}
			}
			specification = std::move(spc_start);
			break;
		}
	}
}


void
Schema::index_array(const MsgPack*& parent_properties, const MsgPack& array, MsgPack*& parent_data, Xapian::Document& doc, const std::string& name)
{
	L_CALL("Schema::index_array({}, {}, <MsgPack*>, <Xapian::Document>, {})", repr(parent_properties->to_string()), repr(array.to_string()), repr(name));

	if (array.empty()) {
		set_type_to_array();
		if (specification.flags.store) {
			parent_data->get(name) = MsgPack::ARRAY();
		}
		return;
	}

	auto spc_start = specification;
	size_t pos = 0;
	for (const auto& item : array) {
		switch (item.get_type()) {
			case MsgPack::Type::MAP: {
				auto properties = &*parent_properties;
				auto data = parent_data;
				FieldVector fields;
				properties = &index_subproperties(properties, data, name, item, fields, pos);
				auto data_pos = specification.flags.store ? &data->get(pos) : data;
				set_type_to_array();
				index_item_value(properties, doc, data_pos, fields);
				specification = spc_start;
				break;
			}

			case MsgPack::Type::ARRAY: {
				auto properties = &*parent_properties;
				auto data = parent_data;
				index_subproperties(properties, data, name, pos);
				auto data_pos = specification.flags.store ? &data->get(pos) : data;
				set_type_to_array();
				index_item_value(doc, *data_pos, item);
				if (specification.flags.store) {
					if (data_pos->is_map() && data_pos->size() == 1) {
						auto it = data_pos->find(RESERVED_VALUE);
						if (it != data_pos->end()) {
							*data_pos = it.value();
						}
					}
				}
				specification = spc_start;
				break;
			}

			case MsgPack::Type::NIL:
			case MsgPack::Type::UNDEFINED: {
				auto properties = &*parent_properties;
				auto data = parent_data;
				index_subproperties(properties, data, name, pos);
				auto data_pos = specification.flags.store ? &data->get(pos) : data;
				set_type_to_array();
				index_partial_paths(doc);
				if (specification.flags.store) {
					*data_pos = item;
					if (data_pos->is_map() && data_pos->size() == 1) {
						auto it = data_pos->find(RESERVED_VALUE);
						if (it != data_pos->end()) {
							*data_pos = it.value();
						}
					}
				}
				specification = spc_start;
				break;
			}

			default: {
				auto properties = &*parent_properties;
				auto data = parent_data;
				index_subproperties(properties, data, name, pos);
				auto data_pos = specification.flags.store ? &data->get(pos) : data;
				set_type_to_array();
				index_item_value(doc, *data_pos, item, pos);
				if (specification.flags.store) {
					if (data_pos->is_map() && data_pos->size() == 1) {
						auto it = data_pos->find(RESERVED_VALUE);
						if (it != data_pos->end()) {
							*data_pos = it.value();
						}
					}
				}
				specification = spc_start;
				break;
			}
		}
		++pos;
	}
}


void
Schema::index_item_value(Xapian::Document& doc, MsgPack& data, const MsgPack& item_value, size_t pos)
{
	L_CALL("Schema::index_item_value(<doc>, {}, {}, {})", repr(data.to_string()), repr(item_value.to_string()), pos);

	if (!specification.flags.complete) {
		if (specification.flags.inside_namespace) {
			complete_namespace_specification(item_value);
		} else {
			complete_specification(item_value);
		}
	}

	if (specification.partial_index_spcs.empty()) {
		index_item(doc, item_value, data, pos);
	} else {
		bool add_value = true;
		index_spc_t start_index_spc(specification.sep_types[SPC_CONCRETE_TYPE], std::move(specification.prefix.field), specification.slot, std::move(specification.accuracy), std::move(specification.acc_prefix));
		for (const auto& index_spc : specification.partial_index_spcs) {
			specification.update(index_spc);
			index_item(doc, item_value, data, pos, add_value);
			add_value = false;
		}
		specification.update(std::move(start_index_spc));
	}

	if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
		set_type_to_object();
	}
}


inline void
Schema::index_item_value(Xapian::Document& doc, MsgPack& data, const MsgPack& item_value)
{
	L_CALL("Schema::index_item_value(<doc>, {}, {})", repr(data.to_string()), repr(item_value.to_string()));

	switch (item_value.get_type()) {
		case MsgPack::Type::ARRAY: {
			bool valid = false;
			for (const auto& item : item_value) {
				if (!(item.is_null() || item.is_undefined())) {
					if (!specification.flags.complete) {
						if (specification.flags.inside_namespace) {
							complete_namespace_specification(item);
						} else {
							complete_specification(item);
						}
					}
					valid = true;
					break;
				}
			}
			if (valid) {
				break;
			}
		}
		[[fallthrough]];
		case MsgPack::Type::NIL:
		case MsgPack::Type::UNDEFINED:
			if (!specification.flags.concrete) {
				if (specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
					if (specification.flags.inside_namespace) {
						validate_required_namespace_data();
					} else {
						validate_required_data(get_mutable_properties(specification.full_meta_name));
					}
				}
			}
			index_partial_paths(doc);
			if (specification.flags.store) {
				data = item_value;
			}
			return;
		default:
			if (!specification.flags.complete) {
				if (specification.flags.inside_namespace) {
					complete_namespace_specification(item_value);
				} else {
					complete_specification(item_value);
				}
			}
			break;
	}

	if (specification.partial_index_spcs.empty()) {
		index_item(doc, item_value, data);
	} else {
		bool add_value = true;
		index_spc_t start_index_spc(specification.sep_types[SPC_CONCRETE_TYPE], std::move(specification.prefix.field), specification.slot,
			std::move(specification.accuracy), std::move(specification.acc_prefix));
		for (const auto& index_spc : specification.partial_index_spcs) {
			specification.update(index_spc);
			index_item(doc, item_value, data, add_value);
			add_value = false;
		}
		specification.update(std::move(start_index_spc));
	}

	if (specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
		if (!specification.flags.static_endpoint) {
			data[RESERVED_ENDPOINT] = specification.endpoint;
		}
	}
}


inline void
Schema::index_item_value(const MsgPack*& properties, Xapian::Document& doc, MsgPack*& data, const FieldVector& fields)
{
	L_CALL("Schema::index_item_value({}, <doc>, {}, <FieldVector>)", repr(properties->to_string()), repr(data->to_string()));

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
	}

	auto val = specification.value ? specification.value.get() : specification.value_rec.get();

	if (val != nullptr) {
		if (specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
			THROW(ClientError, "{} is a foreign type and as such it cannot have a value", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		index_item_value(doc, *data, *val);
	} else {
		if (!specification.flags.concrete) {
			if (specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
				if (specification.flags.inside_namespace) {
					validate_required_namespace_data();
				} else {
					validate_required_data(get_mutable_properties(specification.full_meta_name));
				}
			}
		}
		if (fields.empty()) {
			index_partial_paths(doc);
			if (specification.flags.store && specification.sep_types[SPC_OBJECT_TYPE] == FieldType::object) {
				*data = MsgPack::MAP();
			}
		}
	}

	if (fields.empty()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
			set_type_to_object();
		}
	} else {
		if (specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
			THROW(ClientError, "{} is a foreign type and as such it cannot have extra fields", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		set_type_to_object();
		const auto spc_object = std::move(specification);
		for (const auto& field : fields) {
			specification = spc_object;
			index_object(properties, *field.second, data, doc, field.first);
		}
	}
}


/*  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 *      _   _           _       _
 *     | | | |_ __   __| | __ _| |_ ___
 *     | | | | '_ \ / _` |/ _` | __/ _ \
 *     | |_| | |_) | (_| | (_| | ||  __/
 *      \___/| .__/ \__,_|\__,_|\__\___|
 *           |_|
 *  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 */

bool
Schema::update(const MsgPack& object)
{
	L_CALL("Schema::update({})", repr(object.to_string()));

	L_INDEX("Schema update: " + DIM_GREY + "{}", object.to_string());

	try {
		map_values.clear();
		specification = default_spc;
		specification.slot = DB_SLOT_ROOT;  // Set default RESERVED_SLOT for root

		std::pair<const MsgPack*, const MsgPack*> checked;
		checked = check<ClientError>(object, "Invalid schema: ", true, true);

		if (checked.first != nullptr) {
			mut_schema = std::make_unique<MsgPack>(MsgPack({
				{ RESERVED_TYPE, "foreign/object" },
				{ RESERVED_ENDPOINT, *checked.first },
			}));

			return checked.second != nullptr ? checked.second->size() != 2 : false;
		}

		if (checked.second != nullptr) {
			const auto& schema_obj = *checked.second;

			auto properties = &get_newest_properties();

			FieldVector fields;

			if (properties->empty()) {  // new schemas have empty properties
				specification.flags.field_found = false;
				auto mut_properties = &get_mutable_properties();
				dispatch_write_properties(*mut_properties, schema_obj, fields);
				properties = &*mut_properties;
			} else {
				dispatch_feed_properties(*properties);
				dispatch_process_properties(schema_obj, fields);
			}

			update_item_value(properties, fields);
		}

		// Inject remaining items from received object into the new schema
		const auto it_e = object.end();
		for (auto it = object.begin(); it != it_e; ++it) {
			auto str_key = it->str();
			if (str_key != SCHEMA_FIELD_NAME) {
				if (!mut_schema) {
					mut_schema = std::make_unique<MsgPack>(*schema);
				}
				mut_schema->get(str_key) = it.value();
			}
		}

		return false;
	} catch (...) {
		L_DEBUG("ERROR: {}", object.to_string());
		mut_schema.reset();
		throw;
	}
}


const MsgPack&
Schema::update_subproperties(const MsgPack*& properties, std::string_view name, const MsgPack& object, FieldVector& fields)
{
	L_CALL("Schema::update_subproperties({}, {}, {}, <fields>)", repr(properties->to_string()), repr(name), repr(object.to_string()));

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			detect_dynamic(field_name);
			update_prefixes();
		}
		const auto& field_name = *it;
		dispatch_process_properties(object, fields);
		detect_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(properties, field_name)) {
				update_prefixes();
			} else {
				detect_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(properties, specification.meta_name)) {
						update_prefixes();
						continue;
					}
				}

				auto mut_properties = &get_mutable_properties(specification.full_meta_name);
				add_field(mut_properties);

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						detect_dynamic(n_field_name);
						add_field(mut_properties);
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					detect_dynamic(n_field_name);
					add_field(mut_properties, object, fields);
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(properties, field_name)) {
			dispatch_process_properties(object, fields);
			update_prefixes();
		} else {
			detect_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(properties, specification.meta_name)) {
					dispatch_process_properties(object, fields);
					update_prefixes();
					return *properties;
				}
			}

			auto mut_properties = &get_mutable_properties(specification.full_meta_name);
			add_field(mut_properties, object, fields);
			return *mut_properties;
		}
	}

	return *properties;
}


const MsgPack&
Schema::update_subproperties(const MsgPack*& properties, const std::string& name)
{
	L_CALL("Schema::update_subproperties({}, {})", repr(properties->to_string()), repr(name));

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			detect_dynamic(field_name);
			update_prefixes();
		}
		const auto& field_name = *it;
		detect_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(properties, field_name)) {
				update_prefixes();
			} else {
				detect_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(properties, specification.meta_name)) {
						update_prefixes();
						continue;
					}
				}

				auto mut_properties = &get_mutable_properties(specification.full_meta_name);
				add_field(mut_properties);

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						detect_dynamic(n_field_name);
						add_field(mut_properties);
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					detect_dynamic(n_field_name);
					add_field(mut_properties);
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(properties, field_name)) {
			update_prefixes();
		} else {
			detect_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(properties, specification.meta_name)) {
					update_prefixes();
					return *properties;
				}
			}

			auto mut_properties = &get_mutable_properties(specification.full_meta_name);
			add_field(mut_properties);
			return *mut_properties;
		}

	}

	return *properties;
}


void
Schema::update_object(const MsgPack*& parent_properties, const MsgPack& object, const std::string& name)
{
	L_CALL("Schema::update_object({}, {}, {})", repr(parent_properties->to_string()), repr(object.to_string()), repr(name));

	if (is_comment(name)) {
		return;  // skip comments (empty fields or fields starting with '#')
	}

	if (is_valid(name) && (!specification.flags.recurse || specification.ignored.find(name) != specification.ignored.end())) {
		return;
	}

	switch (object.get_type()) {
		case MsgPack::Type::MAP: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			FieldVector fields;
			properties = &update_subproperties(properties, name, object, fields);
			update_item_value(properties, fields);
			specification = std::move(spc_start);
			return;
		}

		case MsgPack::Type::ARRAY: {
			return update_array(parent_properties, object, name);
		}

		case MsgPack::Type::NIL:
		case MsgPack::Type::UNDEFINED: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			update_subproperties(properties, name);
			specification = std::move(spc_start);
			return;
		}

		default: {
			auto spc_start = specification;
			auto properties = &*parent_properties;
			update_subproperties(properties, name);
			update_item_value();
			specification = std::move(spc_start);
			return;
		}
	}
}

void
Schema::update_array(const MsgPack*& parent_properties, const MsgPack& array, const std::string& name)
{
	L_CALL("Schema::update_array({}, {}, {})", repr(parent_properties->to_string()), repr(array.to_string()), repr(name));

	if (array.empty()) {
		set_type_to_array();
		return;
	}

	auto spc_start = specification;
	size_t pos = 0;
	for (const auto& item : array) {
		switch (item.get_type()) {
			case MsgPack::Type::MAP: {
				auto properties = &*parent_properties;
				FieldVector fields;
				properties = &update_subproperties(properties, name, item, fields);
				update_item_value(properties, fields);
				specification = spc_start;
				break;
			}

			case MsgPack::Type::NIL:
			case MsgPack::Type::UNDEFINED: {
				auto properties = &*parent_properties;
				update_subproperties(properties, name);
				specification = spc_start;
				break;
			}

			default: {
				auto properties = &*parent_properties;
				update_subproperties(properties, name);
				update_item_value();
				specification = spc_start;
				break;
			}
		}
		++pos;
	}
}


void
Schema::update_item_value()
{
	L_CALL("Schema::update_item_value()");

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		bool concrete_type = specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty;
		if (!concrete_type && !foreign_type) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
		if (specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
			if (specification.flags.inside_namespace) {
				validate_required_namespace_data();
			} else {
				validate_required_data(get_mutable_properties(specification.full_meta_name));
			}
		}
	}

	if (!specification.partial_index_spcs.empty()) {
		index_spc_t start_index_spc(specification.sep_types[SPC_CONCRETE_TYPE], std::move(specification.prefix.field), specification.slot, std::move(specification.accuracy), std::move(specification.acc_prefix));
		for (const auto& index_spc : specification.partial_index_spcs) {
			specification.update(index_spc);
		}
		specification.update(std::move(start_index_spc));
	}

	if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
		set_type_to_object();
	}
}


inline void
Schema::update_item_value(const MsgPack*& properties, const FieldVector& fields)
{
	L_CALL("Schema::update_item_value(<const MsgPack*>, <FieldVector>)");

	const auto spc_start = specification;

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		if (specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
			if (specification.flags.inside_namespace) {
				validate_required_namespace_data();
			} else {
				validate_required_data(get_mutable_properties(specification.full_meta_name));
			}
		}
	}

	if (specification.flags.is_namespace && !fields.empty()) {
		specification = std::move(spc_start);
		return;
	}

	if (fields.empty()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
			set_type_to_object();
		}
	} else {
		if (specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
			THROW(ClientError, "{} is a foreign type and as such it cannot have extra fields", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		set_type_to_object();
		const auto spc_object = std::move(specification);
		for (const auto& field : fields) {
			specification = spc_object;
			update_object(properties, *field.second, field.first);
		}
	}
}


/*  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 *     __        __    _ _
 *     \ \      / / __(_) |_ ___
 *      \ \ /\ / / '__| | __/ _ \
 *       \ V  V /| |  | | ||  __/
 *        \_/\_/ |_|  |_|\__\___|
 *  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 */

bool
Schema::write(const MsgPack& object, bool replace)
{
	L_CALL("Schema::write({}, {})", repr(object.to_string()), replace);

	L_INDEX("Schema write: " + DIM_GREY + "{}", object.to_string());

	try {
		map_values.clear();
		specification = default_spc;
		specification.slot = DB_SLOT_ROOT;  // Set default RESERVED_SLOT for root

		std::pair<const MsgPack*, const MsgPack*> checked;
		checked = check<ClientError>(object, "Invalid schema: ", true, true);

		if (checked.first != nullptr) {
			mut_schema = std::make_unique<MsgPack>(MsgPack({
				{ RESERVED_TYPE, "foreign/object" },
				{ RESERVED_ENDPOINT, *checked.first },
			}));

			return checked.second != nullptr ? checked.second->size() != 2 : false;
		}

		if (checked.second != nullptr) {
			const auto& schema_obj = *checked.second;

			auto mut_properties = &get_mutable_properties();
			if (replace) {
				mut_properties->clear();
			}

			FieldVector fields;

			if (mut_properties->empty()) {  // new schemas have empty properties
				specification.flags.field_found = false;
			} else {
				dispatch_feed_properties(*mut_properties);
			}

			dispatch_write_properties(*mut_properties, schema_obj, fields);

			write_item_value(mut_properties, fields);
		}

		// Inject remaining items from received object into the new schema
		const auto it_e = object.end();
		for (auto it = object.begin(); it != it_e; ++it) {
			auto str_key = it->str();
			if (str_key != SCHEMA_FIELD_NAME) {
				if (!mut_schema) {
					mut_schema = std::make_unique<MsgPack>(*schema);
				}
				mut_schema->get(str_key) = it.value();
			}
		}

		return false;
	} catch (...) {
		L_DEBUG("ERROR: {}", object.to_string());
		mut_schema.reset();
		throw;
	}
}


MsgPack&
Schema::write_subproperties(MsgPack*& mut_properties, std::string_view name, const MsgPack& object, FieldVector& fields)
{
	L_CALL("Schema::write_subproperties({}, {}, {}, <fields>)", repr(mut_properties->to_string()), repr(name), repr(object.to_string()));

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			verify_dynamic(field_name);
			update_prefixes();
		}
		const auto& field_name = *it;
		dispatch_write_properties(*mut_properties, object, fields);
		verify_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(mut_properties, field_name)) {
				update_prefixes();
			} else {
				verify_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(mut_properties, specification.meta_name)) {
						update_prefixes();
						continue;
					}
				}

				add_field(mut_properties);

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						verify_dynamic(n_field_name);
						add_field(mut_properties);
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					verify_dynamic(n_field_name);
					add_field(mut_properties, object, fields);
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(mut_properties, field_name)) {
			dispatch_write_properties(*mut_properties, object, fields);
			update_prefixes();
		} else {
			verify_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(mut_properties, specification.meta_name)) {
					dispatch_write_properties(*mut_properties, object, fields);
					update_prefixes();
					return *mut_properties;
				}
			}

			add_field(mut_properties, object, fields);
			return *mut_properties;
		}
	}

	return *mut_properties;
}


MsgPack&
Schema::write_subproperties(MsgPack*& mut_properties, const std::string& name)
{
	L_CALL("Schema::write_subproperties({}, {})", repr(mut_properties->to_string()), repr(name));

	Split<std::string_view> field_names(name, DB_OFFSPRING_UNION);

	auto it = field_names.begin();
	assert(it != field_names.end());

	if (specification.flags.is_namespace) {
		restart_namespace_specification();
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			verify_dynamic(field_name);
			update_prefixes();
		}
		const auto& field_name = *it;
		verify_dynamic(field_name);
		update_prefixes();
		specification.flags.inside_namespace = true;
	} else {
		for (; !it.last(); ++it) {
			const auto& field_name = *it;
			if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			restart_specification();
			if (feed_subproperties(mut_properties, field_name)) {
				update_prefixes();
			} else {
				verify_dynamic(field_name);
				if (specification.flags.uuid_field) {
					if (feed_subproperties(mut_properties, specification.meta_name)) {
						update_prefixes();
						continue;
					}
				}

				add_field(mut_properties);

				for (++it; !it.last(); ++it) {
					const auto& n_field_name = *it;
					if (!is_valid(n_field_name)) {
						THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
					} else {
						verify_dynamic(n_field_name);
						add_field(mut_properties);
					}
				}
				const auto& n_field_name = *it;
				if (!is_valid(n_field_name)) {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(name, n_field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				} else {
					verify_dynamic(n_field_name);
					add_field(mut_properties);
				}
				return *mut_properties;
			}
		}

		const auto& field_name = *it;
		if (!is_valid(field_name) && !(specification.full_meta_name.empty() && has_dispatch_set_default_spc(hh(field_name)))) {
			THROW(ClientError, "Field {} in {} is not valid", repr_field(name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		restart_specification();
		if (feed_subproperties(mut_properties, field_name)) {
			update_prefixes();
		} else {
			verify_dynamic(field_name);
			if (specification.flags.uuid_field) {
				if (feed_subproperties(mut_properties, specification.meta_name)) {
					update_prefixes();
					return *mut_properties;
				}
			}

			add_field(mut_properties);
			return *mut_properties;
		}

	}

	return *mut_properties;
}


void
Schema::write_object(MsgPack*& mut_parent_properties, const MsgPack& object, const std::string& name)
{
	L_CALL("Schema::write_object({}, {}, {})", repr(mut_parent_properties->to_string()), repr(object.to_string()), repr(name));

	if (is_comment(name)) {
		return;  // skip comments (empty fields or fields starting with '#')
	}

	if (is_valid(name) && (!specification.flags.recurse || specification.ignored.find(name) != specification.ignored.end())) {
		return;
	}

	switch (object.get_type()) {
		case MsgPack::Type::MAP: {
			auto spc_start = specification;
			auto properties = &*mut_parent_properties;
			FieldVector fields;
			properties = &write_subproperties(properties, name, object, fields);
			write_item_value(properties, fields);
			specification = std::move(spc_start);
			return;
		}

		case MsgPack::Type::ARRAY: {
			return write_array(mut_parent_properties, object, name);
		}

		case MsgPack::Type::NIL:
		case MsgPack::Type::UNDEFINED: {
			auto spc_start = specification;
			auto properties = &*mut_parent_properties;
			write_subproperties(properties, name);
			specification = std::move(spc_start);
			return;
		}

		default: {
			auto spc_start = specification;
			auto properties = &*mut_parent_properties;
			write_subproperties(properties, name);
			write_item_value(properties);
			specification = std::move(spc_start);
			return;
		}
	}
}


void
Schema::write_array(MsgPack*& mut_parent_properties, const MsgPack& array, const std::string& name)
{
	L_CALL("Schema::write_array({}, {}, {})", repr(mut_parent_properties->to_string()), repr(array.to_string()), repr(name));

	if (array.empty()) {
		set_type_to_array();
		return;
	}

	auto spc_start = specification;
	size_t pos = 0;
	for (const auto& item : array) {
		switch (item.get_type()) {
			case MsgPack::Type::MAP: {
				auto properties = &*mut_parent_properties;
				FieldVector fields;
				properties = &write_subproperties(properties, name, item, fields);
				write_item_value(properties, fields);
				specification = spc_start;
				break;
			}

			case MsgPack::Type::NIL:
			case MsgPack::Type::UNDEFINED: {
				auto properties = &*mut_parent_properties;
				write_subproperties(properties, name);
				specification = spc_start;
				break;
			}

			default: {
				auto properties = &*mut_parent_properties;
				write_subproperties(properties, name);
				write_item_value(properties);
				specification = spc_start;
				break;
			}
		}
		++pos;
	}
}


void
Schema::write_item_value(MsgPack*& mut_properties)
{
	L_CALL("Schema::write_item_value()");

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		bool concrete_type = specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty;
		if (!concrete_type && !foreign_type) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
		if (specification.flags.inside_namespace) {
			validate_required_namespace_data();
		} else {
			validate_required_data(*mut_properties);
		}
	}

	if (!specification.partial_index_spcs.empty()) {
		index_spc_t start_index_spc(specification.sep_types[SPC_CONCRETE_TYPE], std::move(specification.prefix.field), specification.slot, std::move(specification.accuracy), std::move(specification.acc_prefix));
		for (const auto& index_spc : specification.partial_index_spcs) {
			specification.update(index_spc);
		}
		specification.update(std::move(start_index_spc));
	}

	if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
		specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
		set_type_to_object();
	}
}


inline void
Schema::write_item_value(MsgPack*& mut_properties, const FieldVector& fields)
{
	L_CALL("Schema::write_item_value(<const MsgPack*>, <FieldVector>)");

	const auto spc_start = specification;

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		if (specification.flags.inside_namespace) {
			validate_required_namespace_data();
		} else {
			validate_required_data(*mut_properties);
		}
	}

	if (specification.flags.is_namespace && !fields.empty()) {
		specification = std::move(spc_start);
		return;
	}

	if (fields.empty()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty &&
			specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty) {
			set_type_to_object();
		}
	} else {
		if (specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign) {
			THROW(ClientError, "{} is a foreign type and as such it cannot have extra fields", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
		set_type_to_object();
		const auto spc_object = std::move(specification);
		for (const auto& field : fields) {
			specification = spc_object;
			write_object(mut_properties, *field.second, field.first);
		}
	}
}

/*  _____ _____ _____ _____ _____ _____ _____ _____
 * |_____|_____|_____|_____|_____|_____|_____|_____|
 */

std::unordered_set<std::string>
Schema::get_partial_paths(const std::vector<required_spc_t::prefix_t>& partial_prefixes, bool uuid_path)
{
	L_CALL("Schema::get_partial_paths({}, {})", partial_prefixes.size(), uuid_path);

	if (partial_prefixes.size() > LIMIT_PARTIAL_PATHS_DEPTH) {
		THROW(ClientError, "Partial paths limit depth is {}, and partial paths provided has a depth of {}", LIMIT_PARTIAL_PATHS_DEPTH, partial_prefixes.size());
	}

	std::vector<std::string> paths;
	paths.reserve(std::pow(2, partial_prefixes.size() - 2));
	auto it = partial_prefixes.begin();
	paths.push_back(it->field);

	if (uuid_path) {
		if (!it->uuid.empty() && it->field != it->uuid) {
			paths.push_back(it->uuid);
		}
		const auto it_last = partial_prefixes.end() - 1;
		for (++it; it != it_last; ++it) {
			const auto size = paths.size();
			for (size_t i = 0; i < size; ++i) {
				std::string path;
				path.reserve(paths[i].length() + it->field.length());
				path.assign(paths[i]).append(it->field);
				paths.push_back(std::move(path));
				if (!it->uuid.empty() && it->field != it->uuid) {
					path.reserve(paths[i].length() + it->uuid.length());
					path.assign(paths[i]).append(it->uuid);
					paths.push_back(std::move(path));
				}
			}
		}

		if (!it_last->uuid.empty() && it_last->field != it_last->uuid) {
			const auto size = paths.size();
			for (size_t i = 0; i < size; ++i) {
				std::string path;
				path.reserve(paths[i].length() + it_last->uuid.length());
				path.assign(paths[i]).append(it_last->uuid);
				paths.push_back(std::move(path));
				paths[i].append(it_last->field);
			}
		} else {
			for (auto& path : paths) {
				path.append(it_last->field);
			}
		}
	} else {
		const auto it_last = partial_prefixes.end() - 1;
		for (++it; it != it_last; ++it) {
			const auto size = paths.size();
			for (size_t i = 0; i < size; ++i) {
				std::string path;
				path.reserve(paths[i].length() + it->field.length());
				path.assign(paths[i]).append(it->field);
				paths.push_back(std::move(path));
			}
		}

		for (auto& path : paths) {
			path.append(it_last->field);
		}
	}

	return std::unordered_set<std::string>(std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
}


void
Schema::complete_namespace_specification(const MsgPack& item_value)
{
	L_CALL("Schema::complete_namespace_specification({})", repr(item_value.to_string()));

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		bool concrete_type = specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty;
		if (!concrete_type && !foreign_type) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			guess_field_type(item_value);
		}

		validate_required_namespace_data();
	}

	if (specification.partial_prefixes.size() > 2) {
		auto paths = get_partial_paths(specification.partial_prefixes, specification.flags.uuid_path);
		specification.partial_index_spcs.reserve(paths.size());

		if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
			for (auto& path : paths) {
				specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], std::move(path)));
			}
		} else {
			auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
			for (auto& path : paths) {
				specification.partial_index_spcs.emplace_back(global_type, std::move(path));
			}
		}
	} else {
		if (specification.flags.uuid_path) {
			switch (specification.index_uuid_field) {
				case UUIDFieldIndex::uuid: {
					if (specification.prefix.uuid.empty()) {
						auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
						if (specification.sep_types[SPC_CONCRETE_TYPE] == global_type) {
							// Use specification directly because path has never been indexed as UIDFieldIndex::BOTH and type is the same as global_type.
							if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
								specification.slot = get_slot(specification.prefix.field, specification.get_ctype());
								for (auto& acc_prefix : specification.acc_prefix) {
									acc_prefix.insert(0, specification.prefix.field);
								}
							}
						} else if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
							specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field));
						} else {
							specification.partial_index_spcs.emplace_back(global_type, specification.prefix.field);
						}
					} else if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
						specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid));
					} else {
						specification.partial_index_spcs.emplace_back(specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]), specification.prefix.uuid);
					}
					break;
				}
				case UUIDFieldIndex::uuid_field: {
					auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
					if (specification.sep_types[SPC_CONCRETE_TYPE] == global_type) {
						// Use specification directly because type is the same as global_type.
						if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
							if (specification.flags.has_uuid_prefix) {
								specification.slot = get_slot(specification.prefix.field, specification.get_ctype());
							}
							for (auto& acc_prefix : specification.acc_prefix) {
								acc_prefix.insert(0, specification.prefix.field);
							}
						}
					} else if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
						specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field));
					} else {
						specification.partial_index_spcs.emplace_back(global_type, specification.prefix.field);
					}
					break;
				}
				case UUIDFieldIndex::both: {
					if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
						specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field));
						specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid));
					} else {
						auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
						specification.partial_index_spcs.emplace_back(global_type, std::move(specification.prefix.field));
						specification.partial_index_spcs.emplace_back(global_type, specification.prefix.uuid);
					}
					break;
				}
				case UUIDFieldIndex::INVALID:
					break;
			}
		} else {
			auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
			if (specification.sep_types[SPC_CONCRETE_TYPE] == global_type) {
				// Use specification directly because path is not uuid and type is the same as global_type.
				if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
					for (auto& acc_prefix : specification.acc_prefix) {
						acc_prefix.insert(0, specification.prefix.field);
					}
				}
			} else if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
				specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field));
			} else {
				specification.partial_index_spcs.emplace_back(global_type, specification.prefix.field);
			}
		}
	}

	specification.flags.complete = true;
}


void
Schema::complete_specification(const MsgPack& item_value)
{
	L_CALL("Schema::complete_specification({})", repr(item_value.to_string()));

	if (!specification.flags.concrete) {
		bool foreign_type = specification.sep_types[SPC_FOREIGN_TYPE] == FieldType::foreign;
		if (!foreign_type && !specification.endpoint.empty()) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.sep_types[SPC_FOREIGN_TYPE] = FieldType::foreign;
		}
		bool concrete_type = specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty;
		if (!concrete_type && !foreign_type) {
			if (specification.flags.strict) {
				THROW(MissingTypeError, "Type of field {} is missing", specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			guess_field_type(item_value);
		}
		if (specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty) {
			validate_required_data(get_mutable_properties(specification.full_meta_name));
		}
	}

	if (specification.partial_prefixes.size() > 2) {
		auto paths = get_partial_paths(specification.partial_prefixes, specification.flags.uuid_path);
		specification.partial_index_spcs.reserve(paths.size());
		paths.erase(specification.prefix.field);
		if (!specification.local_prefix.uuid.empty()) {
			// local_prefix.uuid tell us if the last field is indexed as UIDFieldIndex::BOTH.
			paths.erase(specification.prefix.uuid);
		}

		if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
			for (auto& path : paths) {
				specification.partial_index_spcs.emplace_back(get_namespace_specification(specification.sep_types[SPC_CONCRETE_TYPE], std::move(path)));
			}
		} else {
			auto global_type = specification_t::global_type(specification.sep_types[SPC_CONCRETE_TYPE]);
			for (auto& path : paths) {
				specification.partial_index_spcs.emplace_back(global_type, std::move(path));
			}
		}
	}

	if (specification.flags.uuid_path) {
		switch (specification.index_uuid_field) {
			case UUIDFieldIndex::uuid: {
				if (specification.prefix.uuid.empty()) {
					// Use specification directly because path has never been indexed as UIDFieldIndex::BOTH.
					if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
						specification.slot = get_slot(specification.prefix.field, specification.get_ctype());
						for (auto& acc_prefix : specification.acc_prefix) {
							acc_prefix.insert(0, specification.prefix.field);
						}
					}
				} else if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
					index_spc_t spc_uuid(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid, get_slot(specification.prefix.uuid, specification.get_ctype()),
						specification.accuracy, specification.acc_prefix);
					for (auto& acc_prefix : spc_uuid.acc_prefix) {
						acc_prefix.insert(0, spc_uuid.prefix);
					}
					specification.partial_index_spcs.push_back(std::move(spc_uuid));
				} else {
					specification.partial_index_spcs.emplace_back(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid);
				}
				break;
			}
			case UUIDFieldIndex::uuid_field: {
				// Use specification directly.
				if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
					if (specification.flags.has_uuid_prefix) {
						specification.slot = get_slot(specification.prefix.field, specification.get_ctype());
					}
					for (auto& acc_prefix : specification.acc_prefix) {
						acc_prefix.insert(0, specification.prefix.field);
					}
				}
				break;
			}
			case UUIDFieldIndex::both: {
				if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
					index_spc_t spc_field(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field,
						specification.flags.has_uuid_prefix ? get_slot(specification.prefix.field, specification.get_ctype()) : specification.slot,
						specification.accuracy, specification.acc_prefix);
					for (auto& acc_prefix : spc_field.acc_prefix) {
						acc_prefix.insert(0, spc_field.prefix);
					}
					index_spc_t spc_uuid(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid, get_slot(specification.prefix.uuid, specification.get_ctype()),
						specification.accuracy, specification.acc_prefix);
					for (auto& acc_prefix : spc_uuid.acc_prefix) {
						acc_prefix.insert(0, spc_uuid.prefix);
					}
					specification.partial_index_spcs.push_back(std::move(spc_field));
					specification.partial_index_spcs.push_back(std::move(spc_uuid));
				} else {
					specification.partial_index_spcs.emplace_back(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.field);
					specification.partial_index_spcs.emplace_back(specification.sep_types[SPC_CONCRETE_TYPE], specification.prefix.uuid);
				}
				break;
			}
			case UUIDFieldIndex::INVALID:
				break;
		}
	} else {
		if (toUType(specification.index & TypeIndex::FIELD_VALUES) != 0u) {
			for (auto& acc_prefix : specification.acc_prefix) {
				acc_prefix.insert(0, specification.prefix.field);
			}
		}
	}

	specification.flags.complete = true;
}


inline void
Schema::set_type_to_object()
{
	L_CALL("Schema::set_type_to_object()");

	if unlikely(specification.sep_types[SPC_OBJECT_TYPE] == FieldType::empty && !specification.flags.inside_namespace) {
		specification.sep_types[SPC_OBJECT_TYPE] = FieldType::object;
		auto& mut_properties = get_mutable_properties(specification.full_meta_name);
		mut_properties[RESERVED_TYPE] = _get_str_type(specification.sep_types);
	}
}


inline void
Schema::set_type_to_array()
{
	L_CALL("Schema::set_type_to_array()");

	if unlikely(specification.sep_types[SPC_ARRAY_TYPE] == FieldType::empty && !specification.flags.inside_namespace) {
		specification.sep_types[SPC_ARRAY_TYPE] = FieldType::array;
		auto& mut_properties = get_mutable_properties(specification.full_meta_name);
		mut_properties[RESERVED_TYPE] = _get_str_type(specification.sep_types);
	}
}


void
Schema::validate_required_namespace_data()
{
	L_CALL("Schema::validate_required_namespace_data()");

	switch (specification.sep_types[SPC_CONCRETE_TYPE]) {
		case FieldType::geo:
			// Set partials and error.
			specification.flags.partials = default_spc.flags.partials;
			specification.error = default_spc.error;
			specification.flags.concrete = true;
			break;

		case FieldType::string:
		case FieldType::text:
			specification.flags.ngram = default_spc.flags.ngram;
			specification.flags.cjk_ngram = default_spc.flags.cjk_ngram;
			specification.flags.cjk_words = default_spc.flags.cjk_words;
			specification.language = default_spc.language;
			if (!specification.language.empty()) {
				specification.stop_strategy = default_spc.stop_strategy;
			}
			specification.stem_language = default_spc.stem_language;
			if (!specification.stem_language.empty()) {
				specification.stem_strategy = default_spc.stem_strategy;
			}
			specification.flags.concrete = true;
			break;

		case FieldType::keyword:
			if (!specification.flags.has_bool_term) {
				specification.flags.bool_term = strings::hasupper(specification.meta_name);
				specification.flags.has_bool_term = specification.flags.bool_term;
			}
			specification.flags.concrete = true;
			break;

		case FieldType::script:
			if (!specification.flags.has_index) {
				specification.index = TypeIndex::NONE; // Fallback to index anything.
				specification.flags.has_index = true;
			}
			specification.flags.concrete = true;
			break;

		case FieldType::date:
		case FieldType::datetime:
		case FieldType::time:
		case FieldType::timedelta:
		case FieldType::integer:
		case FieldType::positive:
		case FieldType::floating:
		case FieldType::boolean:
		case FieldType::uuid:
			specification.flags.concrete = true;
			break;

		case FieldType::empty:
			specification.flags.concrete = false;
			break;

		default:
			THROW(ClientError, "{}: '{}' is not supported", RESERVED_TYPE, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]));
	}
}


void
Schema::validate_required_data(MsgPack& mut_properties)
{
	L_CALL("Schema::validate_required_data({})", repr(mut_properties.to_string()));

	dispatch_set_default_spc(mut_properties);

	std::set<uint64_t> set_acc;

	auto type = specification.sep_types[SPC_CONCRETE_TYPE];
	switch (type) {
		case FieldType::geo: {
			// Set partials and error.
			mut_properties[RESERVED_PARTIALS] = static_cast<bool>(specification.flags.partials);
			mut_properties[RESERVED_ERROR] = specification.error;
			if (toUType(specification.index & TypeIndex::TERMS) != 0u) {
				if (specification.doc_acc) {
					if (specification.doc_acc->is_array()) {
						for (const auto& _accuracy : *specification.doc_acc) {
							if (_accuracy.is_number()) {
								const auto val_acc = _accuracy.u64();
								if (val_acc <= HTM_MAX_LEVEL) {
									set_acc.insert(val_acc);
								} else {
									THROW(ClientError, "Data inconsistency, level value in '{}': '{}' must be a positive number between 0 and {} ({} not supported)", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL, val_acc);
								}
							} else {
								THROW(ClientError, "Data inconsistency, level value in '{}': '{}' must be a positive number between 0 and {}", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL);
							}
						}
					} else {
						THROW(ClientError, "Data inconsistency, level value in '{}': '{}' must be a positive number between 0 and {}", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL);
					}
				} else {
					set_acc.insert(def_accuracy_geo.begin(), def_accuracy_geo.end());
				}
			}
			specification.flags.concrete = true;
			break;
		}
		case FieldType::date:
		case FieldType::datetime: {
			if (toUType(specification.index & TypeIndex::TERMS) != 0u) {
				if (specification.doc_acc) {
					if (specification.doc_acc->is_array()) {
						for (const auto& _accuracy : *specification.doc_acc) {
							uint64_t accuracy;
							if (_accuracy.is_string()) {
								auto accuracy_date = _get_accuracy_datetime(_accuracy.str_view());
								if (accuracy_date != UnitTime::INVALID) {
									accuracy = toUType(accuracy_date);
								} else {
									THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, type == FieldType::datetime ? DATETIME_STR : DATE_STR, repr(str_set_acc_date), repr(_accuracy.str_view()));
								}
							} else if (_accuracy.is_number()) {
								accuracy = _accuracy.u64();
								if (!validate_acc_date(static_cast<UnitTime>(accuracy))) {
									THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, type == FieldType::datetime ? DATETIME_STR : DATE_STR, repr(str_set_acc_date));
								}
							} else {
								THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, type == FieldType::datetime ? DATETIME_STR : DATE_STR, repr(str_set_acc_date), repr(_accuracy.str_view()));
							}
							set_acc.insert(accuracy);
						}
					} else {
						THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, type == FieldType::datetime ? DATETIME_STR : DATE_STR, repr(str_set_acc_date));
					}
				} else {
					set_acc.insert(def_accuracy_datetime.begin(), def_accuracy_datetime.end());
				}
			}
			specification.flags.concrete = true;
			break;
		}
		case FieldType::time:
		case FieldType::timedelta: {
			if (toUType(specification.index & TypeIndex::TERMS) != 0u) {
				if (specification.doc_acc) {
					if (specification.doc_acc->is_array()) {
						for (const auto& _accuracy : *specification.doc_acc) {
							if (_accuracy.is_string()) {
								auto accuracy_time = _get_accuracy_time(_accuracy.str_view());
								if (accuracy_time != UnitTime::INVALID) {
									set_acc.insert(toUType(accuracy_time));
								} else {
									THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), repr(str_set_acc_time), repr(_accuracy.str_view()));
								}
							} else {
								THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), repr(str_set_acc_time), repr(_accuracy.str_view()));
							}
						}
					} else {
						THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), repr(str_set_acc_time));
					}
				} else {
					set_acc.insert(def_accuracy_time.begin(), def_accuracy_time.end());
				}
			}
			specification.flags.concrete = true;
			break;
		}
		case FieldType::integer:
		case FieldType::positive:
		case FieldType::floating: {
			if (toUType(specification.index & TypeIndex::TERMS) != 0u) {
				if (specification.doc_acc) {
					if (specification.doc_acc->is_array()) {
						for (const auto& _accuracy : *specification.doc_acc) {
							if (_accuracy.is_number()) {
								set_acc.insert(_accuracy.u64());
							} else {
								THROW(ClientError, "Data inconsistency, '{}' in '{}' must be an array of positive numbers", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]));
							}
						}
					} else {
						THROW(ClientError, "Data inconsistency, '{}' in '{}' must be an array of positive numbers", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]));
					}
				} else {
					set_acc.insert(def_accuracy_num.begin(), def_accuracy_num.end());
				}
			}
			specification.flags.concrete = true;
			break;
		}
		case FieldType::string:
		case FieldType::text: {
			mut_properties[RESERVED_NGRAM] = static_cast<bool>(specification.flags.ngram);
			mut_properties[RESERVED_CJK_NGRAM] = static_cast<bool>(specification.flags.cjk_ngram);
			mut_properties[RESERVED_CJK_WORDS] = static_cast<bool>(specification.flags.cjk_words);

			// Language could be needed, for soundex.
			if (specification.aux_language.empty() && !specification.aux_stem_language.empty()) {
				specification.language = specification.aux_stem_language;
			}
			if (!specification.language.empty()) {
				mut_properties[RESERVED_LANGUAGE] = specification.language;
				mut_properties[RESERVED_STOP_STRATEGY] = enum_name(specification.stop_strategy);
			}
			if (specification.aux_stem_language.empty() && !specification.aux_language.empty()) {
				specification.stem_language = specification.aux_language;
			}
			if (!specification.stem_language.empty()) {
				mut_properties[RESERVED_STEM_LANGUAGE] = specification.stem_language;
				mut_properties[RESERVED_STEM_STRATEGY] = enum_name(specification.stem_strategy);
			}

			specification.flags.concrete = true;
			break;
		}
		case FieldType::keyword: {
			// Process RESERVED_BOOL_TERM
			if (!specification.flags.has_bool_term) {
				// By default, if normalized name has upper characters then it is consider bool term.
				const auto bool_term = strings::hasupper(specification.meta_name);
				if (specification.flags.bool_term != bool_term) {
					specification.flags.bool_term = bool_term;
					mut_properties[RESERVED_BOOL_TERM] = static_cast<bool>(specification.flags.bool_term);
				}
				specification.flags.has_bool_term = true;
			}

			specification.flags.concrete = true;
			break;
		}
		case FieldType::script: {
			if (!specification.flags.has_index) {
				const auto index = TypeIndex::NONE; // Fallback to index anything.
				if (specification.index != index) {
					specification.index = index;
					mut_properties[RESERVED_INDEX] = _get_str_index(index);
				}
				specification.flags.has_index = true;
			}
			specification.flags.concrete = true;
			break;
		}
		case FieldType::boolean:
		case FieldType::uuid:
			specification.flags.concrete = true;
			break;

		case FieldType::empty:
			specification.flags.concrete = false;
			break;

		default:
			THROW(ClientError, "{}: '{}' is not supported", RESERVED_TYPE, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]));
	}

	// If field is namespace fallback to index anything but values.
	if (!specification.flags.has_index && !specification.partial_prefixes.empty()) {
		const auto index = specification.index & ~TypeIndex::VALUES;
		if (specification.index != index) {
			specification.index = index;
			mut_properties[RESERVED_INDEX] = _get_str_index(index);
		}
		specification.flags.has_index = true;
	}

	if (specification.index != TypeIndex::NONE) {
		if (specification.flags.concrete) {
			if (toUType(specification.index & TypeIndex::VALUES) != 0u) {
				// Write RESERVED_SLOT in properties (if it has values).
				if (specification.slot == Xapian::BAD_VALUENO) {
					specification.slot = get_slot(specification.prefix.field, specification.get_ctype());
				}
				mut_properties[RESERVED_SLOT] = specification.slot;

				// Write RESERVED_ACCURACY and RESERVED_ACC_PREFIX in properties.
				if (!set_acc.empty()) {
					specification.acc_prefix.clear();
					for (const auto& acc : set_acc) {
						specification.acc_prefix.push_back(get_prefix(acc));
					}
					specification.accuracy.assign(set_acc.begin(), set_acc.end());
					switch (specification.sep_types[SPC_CONCRETE_TYPE]) {
						case FieldType::date:
						case FieldType::datetime:
						case FieldType::time:
						case FieldType::timedelta:
							mut_properties[RESERVED_ACCURACY] = MsgPack::ARRAY();
							for (auto& acc : specification.accuracy) {
								mut_properties[RESERVED_ACCURACY].push_back(_get_str_acc_date(static_cast<UnitTime>(acc)));
							}
							break;
						default:
							mut_properties[RESERVED_ACCURACY] = specification.accuracy;
							break;
					}
					mut_properties[RESERVED_ACC_PREFIX] = specification.acc_prefix;
				}
			}
		}
	}

	// Process RESERVED_TYPE
	mut_properties[RESERVED_TYPE] = _get_str_type(specification.sep_types);

	// L_DEBUG("\nspecification = {}\nmut_properties = {}", specification.to_string(4), mut_properties.to_string(true));
}


void
Schema::guess_field_type(const MsgPack& item_doc)
{
	L_CALL("Schema::guess_field_type({})", repr(item_doc.to_string()));

	switch (item_doc.get_type()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			if (specification.flags.numeric_detection) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::positive;
				return;
			}
			break;
		case MsgPack::Type::NEGATIVE_INTEGER:
			if (specification.flags.numeric_detection) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::integer;
				return;
			}
			break;
		case MsgPack::Type::FLOAT:
			if (specification.flags.numeric_detection) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::floating;
				return;
			}
			break;
		case MsgPack::Type::BOOLEAN:
			if (specification.flags.bool_detection) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::boolean;
				return;
			}
			break;
		case MsgPack::Type::STR: {
			const auto str_value = item_doc.str_view();
			if (specification.flags.uuid_detection && Serialise::isUUID(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::uuid;
				return;
			}
			if (specification.flags.date_detection && Datetime::isDate(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::date;
				return;
			}
			if (specification.flags.datetime_detection && Datetime::isDatetime(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::datetime;
				return;
			}
			if (specification.flags.time_detection && Datetime::isTime(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::time;
				return;
			}
			if (specification.flags.timedelta_detection && Datetime::isTimedelta(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::timedelta;
				return;
			}
			if (specification.flags.geo_detection && EWKT::isEWKT(str_value)) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::geo;
				return;
			}
			if (specification.flags.bool_detection) {
				if (str_value == "true" || str_value == "false") {
					specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::boolean;
					return;
				}
			}
			if (specification.flags.text_detection && !specification.flags.bool_term) {
				specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::text;
				return;
			}
			specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::keyword;
			return;
		}
		case MsgPack::Type::MAP:
			if (item_doc.size() == 1) {
				auto item = item_doc.begin();
				if (item->is_string()) {
					specification.sep_types[SPC_CONCRETE_TYPE] = Cast::get_field_type(item->str());
					return;
				}
			}
			THROW(ClientError, "'{}' cannot be a nested object", RESERVED_VALUE);
		case MsgPack::Type::ARRAY:
			THROW(ClientError, "'{}' cannot be a nested array", RESERVED_VALUE);
		default:
			break;
	}

	THROW(ClientError, "'{}': {} is ambiguous", RESERVED_VALUE, repr(item_doc.to_string()));
}


void
Schema::index_item(Xapian::Document& doc, const MsgPack& value, MsgPack& data, size_t pos, bool add_value)
{
	L_CALL("Schema::index_item(<doc>, {}, {}, {}, {})", repr(value.to_string()), repr(data.to_string()), pos, add_value);

	L_SCHEMA("Final Specification: {}", specification.to_string(4));

	_index_item(doc, std::array<std::reference_wrapper<const MsgPack>, 1>({{ value }}), pos);
	if (specification.flags.store && add_value) {
		// Add value to data.
		auto& data_value = data[RESERVED_VALUE];
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::uuid) {
			switch (data_value.get_type()) {
				case MsgPack::Type::UNDEFINED:
					data_value = normalize_uuid(value);
					break;
				case MsgPack::Type::ARRAY:
					data_value.push_back(normalize_uuid(value));
					break;
				default:
					data_value = MsgPack({ data_value, normalize_uuid(value) });
					break;
			}
		} else if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::date || specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::datetime) {
			switch (data_value.get_type()) {
				case MsgPack::Type::UNDEFINED:
					data_value = Datetime::iso8601(Datetime::DatetimeParser(value));
					break;
				case MsgPack::Type::ARRAY:
					data_value.push_back(Datetime::iso8601(Datetime::DatetimeParser(value)));
					break;
				default:
					data_value = MsgPack({ data_value, Datetime::iso8601(Datetime::DatetimeParser(value)) });
					break;
			}
		} else {
			switch (data_value.get_type()) {
				case MsgPack::Type::UNDEFINED:
					data_value = value;
					break;
				case MsgPack::Type::ARRAY:
					data_value.push_back(value);
					break;
				default:
					data_value = MsgPack({ data_value, value });
					break;
			}
		}
	}
}


void
Schema::index_item(Xapian::Document& doc, const MsgPack& values, MsgPack& data, bool add_values)
{
	L_CALL("Schema::index_item(<doc>, {}, {}, {})", repr(values.to_string()), repr(data.to_string()), add_values);

	if (values.is_array()) {
		set_type_to_array();

		_index_item(doc, values, 0);

		if (specification.flags.store && add_values) {
			// Add value to data.
			auto& data_value = data[RESERVED_VALUE];
			if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::uuid) {
				switch (data_value.get_type()) {
					case MsgPack::Type::UNDEFINED:
						data_value = MsgPack::ARRAY();
						for (const auto& value : values) {
							data_value.push_back(normalize_uuid(value));
						}
						break;
					case MsgPack::Type::ARRAY:
						for (const auto& value : values) {
							data_value.push_back(normalize_uuid(value));
						}
						break;
					default:
						data_value = MsgPack({ data_value });
						for (const auto& value : values) {
							data_value.push_back(normalize_uuid(value));
						}
						break;
				}
			}  else if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::date || specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::datetime) {
				switch (data_value.get_type()) {
					case MsgPack::Type::UNDEFINED:
						for (const auto& value : values) {
							data_value.push_back(Datetime::iso8601(Datetime::DatetimeParser(value)));
						}
						break;
					case MsgPack::Type::ARRAY:
						for (const auto& value : values) {
							data_value.push_back(Datetime::iso8601(Datetime::DatetimeParser(value)));
						}
						break;
					default:
						for (const auto& value : values) {
							data_value.push_back(MsgPack({ data_value, Datetime::iso8601(Datetime::DatetimeParser(value)) }));
						}
						break;
				}
			} else {
				switch (data_value.get_type()) {
					case MsgPack::Type::UNDEFINED:
						data_value = values;
						break;
					case MsgPack::Type::ARRAY:
						for (const auto& value : values) {
							data_value.push_back(value);
						}
						break;
					default:
						data_value = MsgPack({ data_value });
						for (const auto& value : values) {
							data_value.push_back(value);
						}
						break;
				}
			}
		}
	} else {
		index_item(doc, values, data, 0, add_values);
	}
}


void
Schema::index_partial_paths(Xapian::Document& doc)
{
	L_CALL("Schema::index_partial_paths(<Xapian::Document>)");

	if (specification.flags.partial_paths) {
		if (toUType(specification.index & TypeIndex::FIELD_TERMS) != 0u) {
			if (specification.partial_prefixes.size() > 2) {
				const auto paths = get_partial_paths(specification.partial_prefixes, specification.flags.uuid_path);
				for (const auto& path : paths) {
					doc.add_boolean_term(path);
				}
			} else {
				doc.add_boolean_term(specification.prefix.field);
			}
		}
	}
}


inline void
Schema::index_simple_term(Xapian::Document& doc, std::string_view term, const specification_t& field_spc, size_t pos)
{
	L_CALL("Schema::index_simple_term(<doc>, {}, <field_spc>, {})", repr(term), pos);

	if (term.size() > 245) {
		if (field_spc.sep_types[SPC_CONCRETE_TYPE] == FieldType::keyword) {
			THROW(ClientError, "Keyword too long");
		}
		return;
	}

	if (term == "QN\x80") {
		// Term reserved for numeric (autoincremented) IDs
		return;
	}

	const auto weight = field_spc.flags.bool_term ? 0 : field_spc.weight[getPos(pos, field_spc.weight.size())];
	const auto position = field_spc.position[getPos(pos, field_spc.position.size())];
	if (position != 0u) {
		doc.add_posting(std::string(term), position, weight);
	} else {
		doc.add_term(std::string(term), weight);
	}
	L_INDEX("Field Term [{}] -> {}  Bool: {}  Posting: {}", pos, repr(term), field_spc.flags.bool_term, position);
}


template <typename T>
inline void
Schema::_index_item(Xapian::Document& doc, T&& values, size_t pos)
{
	L_CALL("Schema::_index_item(<doc>, <values>, {})", pos);

	switch (specification.index) {
		case TypeIndex::INVALID:
		case TypeIndex::NONE:
			return;

		case TypeIndex::FIELD_TERMS: {
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_term(doc, Serialise::MsgPack(specification, value), specification, pos++);
				}
			}
			break;
		}
		case TypeIndex::FIELD_VALUES: {
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, specification, pos++);
				}
			}
			break;
		}
		case TypeIndex::FIELD_ALL: {
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, specification, pos++, &specification);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_TERMS: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_term(doc, Serialise::MsgPack(global_spc, value), global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::TERMS: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_all_term(doc, value, specification, global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_TERMS_FIELD_VALUES: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, specification, pos++, nullptr, &global_spc);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_TERMS_FIELD_ALL: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, specification, pos++, &specification, &global_spc);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_VALUES: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_g, global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_VALUES_FIELD_TERMS: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_g, global_spc, pos++, &specification);
				}
			}
			break;
		}
		case TypeIndex::VALUES: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_all_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, s_g, specification, global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_VALUES_FIELD_ALL: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_all_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, s_g, specification, global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_ALL: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_g, global_spc, pos++, nullptr, &global_spc);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_ALL_FIELD_TERMS: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_value(doc, value.is_map() ? Cast::cast(value) : value, s_g, global_spc, pos++, &specification, &global_spc);
				}
			}
			break;
		}
		case TypeIndex::GLOBAL_ALL_FIELD_VALUES: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_g = map_values[global_spc.slot];
			auto& s_f = map_values[specification.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					pos++; /* do nothing else (don't index any terms) */
				} else {
					index_all_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, s_g, specification, global_spc, pos++);
				}
			}
			break;
		}
		case TypeIndex::ALL: {
			const auto& global_spc = specification_t::get_global(specification.sep_types[SPC_CONCRETE_TYPE]);
			auto& s_f = map_values[specification.slot];
			auto& s_g = map_values[global_spc.slot];
			for (const MsgPack& value : values) {
				if (value.is_null() || value.is_undefined()) {
					index_simple_term(doc, specification.prefix.field, specification, pos++);
				} else {
					index_all_value(doc, value.is_map() ? Cast::cast(value) : value, s_f, s_g, specification, global_spc, pos++);
				}
			}
			break;
		}
	}
}


void
Schema::index_term(Xapian::Document& doc, std::string serialise_val, const specification_t& field_spc, size_t pos)
{
	L_CALL("Schema::index_term(<Xapian::Document>, {}, <specification_t>, {})", repr(serialise_val), pos);

	switch (field_spc.sep_types[SPC_CONCRETE_TYPE]) {
		case FieldType::string:
		case FieldType::text: {
			Xapian::TermGenerator term_generator;
			Xapian::TermGenerator::flags flags = 0;
			if (field_spc.flags.cjk_ngram) {
				flags |= Xapian::TermGenerator::FLAG_CJK_NGRAM;
			}
#ifdef USE_ICU
			if (field_spc.flags.cjk_words) {
				flags |= Xapian::TermGenerator::FLAG_CJK_WORDS;
			}
#endif
			term_generator.set_flags(flags);
			term_generator.set_document(doc);
			if (!field_spc.language.empty()) {
				term_generator.set_stopper(getStopper(field_spc.language).get());
				term_generator.set_stopper_strategy(getGeneratorStopStrategy(field_spc.stop_strategy));
			}
			if (!field_spc.stem_language.empty()) {
				term_generator.set_stemmer(Xapian::Stem(field_spc.stem_language));
				term_generator.set_stemming_strategy(getGeneratorStemStrategy(field_spc.stem_strategy));
			}
			const bool positions = field_spc.positions[getPos(pos, field_spc.positions.size())];
			if (positions) {
				term_generator.index_text(serialise_val, field_spc.weight[getPos(pos, field_spc.weight.size())], field_spc.prefix.field + field_spc.get_ctype());
			} else {
				term_generator.index_text_without_positions(serialise_val, field_spc.weight[getPos(pos, field_spc.weight.size())], field_spc.prefix.field + field_spc.get_ctype());
			}
			L_INDEX("Field Text to Index [{}] => {}:{} [Positions: {}]", pos, repr(field_spc.prefix.field), serialise_val, positions);
			break;
		}

		case FieldType::keyword:
			if (!field_spc.flags.bool_term) {
				strings::inplace_lower(serialise_val);
			}
			[[fallthrough]];

		default: {
			serialise_val = prefixed(serialise_val, field_spc.prefix.field, field_spc.get_ctype());
			index_simple_term(doc, serialise_val, field_spc, pos);
			break;
		}
	}
}


void
Schema::index_all_term(Xapian::Document& doc, const MsgPack& value, const specification_t& field_spc, const specification_t& global_spc, size_t pos)
{
	L_CALL("Schema::index_all_term(<Xapian::Document>, {}, <specification_t>, <specification_t>, {})", repr(value.to_string()), pos);

	auto serialise_val = Serialise::MsgPack(field_spc, value);
	index_term(doc, serialise_val, field_spc, pos);
	index_term(doc, serialise_val, global_spc, pos);
}


void
Schema::merge_geospatial_values(std::set<std::string>& s, std::vector<range_t> ranges, std::vector<Cartesian> centroids)
{
	L_CALL("Schema::merge_geospatial_values(...)");

	if (s.empty()) {
		s.insert(Serialise::ranges_centroids(ranges, centroids));
	} else {
		auto prev_value = Unserialise::ranges_centroids(*s.begin());
		s.clear();
		ranges = HTM::range_union(std::move(ranges), std::vector<range_t>(std::make_move_iterator(prev_value.first.begin()), std::make_move_iterator(prev_value.first.end())));
		const auto& prev_centroids = prev_value.second;
		if (!prev_centroids.empty()) {
			std::vector<Cartesian> missing;
			auto centroids_begin = centroids.begin();
			auto centroids_end = centroids.end();
			for (const auto& _centroid : prev_centroids) {
				if (std::find(centroids_begin, centroids_end, _centroid) == centroids_end) {
					missing.push_back(_centroid);
				}
			}
			centroids.insert(centroids.end(), missing.begin(), missing.end());
		}
		s.insert(Serialise::ranges_centroids(ranges, centroids));
	}
}


void
Schema::index_value(Xapian::Document& doc, const MsgPack& value, std::set<std::string>& s, const specification_t& spc, size_t pos, const specification_t* field_spc, const specification_t* global_spc)
{
	L_CALL("Schema::index_value(<Xapian::Document>, {}, <std::set<std::string>>, <specification_t>, {}, <specification_t*>, <specification_t*>)", repr(value.to_string()), pos);

	switch (spc.sep_types[SPC_CONCRETE_TYPE]) {
		case FieldType::floating: {
			if (value.is_number()) {
				const auto f_val = value.f64();
				auto ser_value = Serialise::floating(f_val);
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				s.insert(std::move(ser_value));
				GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, static_cast<int64_t>(f_val));
				return;
			} else {
				THROW(ClientError, "Format invalid for float type: {}", repr(value.to_string()));
			}
		}
		case FieldType::integer: {
			if (value.is_number()) {
				const auto i_val = value.i64();
				auto ser_value = Serialise::integer(i_val);
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				s.insert(std::move(ser_value));
				GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, i_val);
				return;
			} else {
				THROW(ClientError, "Format invalid for integer type: {}", value.to_string());
			}
		}
		case FieldType::positive: {
			if (value.is_number()) {
				const auto u_val = value.u64();
				auto ser_value = Serialise::positive(u_val);
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				s.insert(std::move(ser_value));
				GenerateTerms::positive(doc, spc.accuracy, spc.acc_prefix, u_val);
				return;
			} else {
				THROW(ClientError, "Format invalid for positive type: {}", value.to_string());
			}
		}
		case FieldType::date:
		case FieldType::datetime: {
			Datetime::tm_t tm;
			auto ser_value = Serialise::datetime(value, tm);
			if (field_spc != nullptr) {
				index_term(doc, ser_value, *field_spc, pos);
			}
			if (global_spc != nullptr) {
				index_term(doc, ser_value, *global_spc, pos);
			}
			s.insert(std::move(ser_value));
			GenerateTerms::datetime(doc, spc.accuracy, spc.acc_prefix, tm);
			return;
		}
		case FieldType::time: {
			double t_val = 0.0;
			auto ser_value = Serialise::time(value, t_val);
			if (field_spc != nullptr) {
				index_term(doc, ser_value, *field_spc, pos);
			}
			if (global_spc != nullptr) {
				index_term(doc, ser_value, *global_spc, pos);
			}
			s.insert(std::move(ser_value));
			GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, t_val);
			return;
		}
		case FieldType::timedelta: {
			double t_val = 0.0;
			auto ser_value = Serialise::timedelta(value, t_val);
			if (field_spc != nullptr) {
				index_term(doc, ser_value, *field_spc, pos);
			}
			if (global_spc != nullptr) {
				index_term(doc, ser_value, *global_spc, pos);
			}
			s.insert(std::move(ser_value));
			GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, t_val);
			return;
		}
		case FieldType::geo: {
			GeoSpatial geo(value);
			const auto& geometry = geo.getGeometry();
			auto ranges = geometry->getRanges(spc.flags.partials, spc.error);
			if (ranges.empty()) {
				return;
			}
			std::string term;
			if (field_spc != nullptr) {
				if (spc.flags.partials == DEFAULT_GEO_PARTIALS && spc.error == DEFAULT_GEO_ERROR) {
					term = Serialise::ranges_hash(ranges);
					index_term(doc, term, *field_spc, pos);
				} else {
					const auto f_ranges = geometry->getRanges(DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR);
					term = Serialise::ranges_hash(f_ranges);
					index_term(doc, term, *field_spc, pos);
				}
			}
			if (global_spc != nullptr) {
				if (field_spc != nullptr) {
					index_term(doc, std::move(term), *global_spc, pos);
				} else {
					if (spc.flags.partials == DEFAULT_GEO_PARTIALS && spc.error == DEFAULT_GEO_ERROR) {
						index_term(doc, Serialise::ranges_hash(ranges), *global_spc, pos);
					} else {
						const auto g_ranges = geometry->getRanges(DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR);
						index_term(doc, Serialise::ranges_hash(g_ranges), *global_spc, pos);
					}
				}
			}
			GenerateTerms::geo(doc, spc.accuracy, spc.acc_prefix, ranges);
			merge_geospatial_values(s, std::move(ranges), geometry->getCentroids());
			return;
		}
		case FieldType::keyword: {
			if (value.is_string()) {
				auto ser_value = value.str();
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				s.insert(std::move(ser_value));
				return;
			} else {
				THROW(ClientError, "Format invalid for {} type: {}", enum_name(spc.sep_types[SPC_CONCRETE_TYPE]), repr(value.to_string()));
			}
		}
		case FieldType::string:
		case FieldType::text: {
			if (value.is_string()) {
				auto ser_value = value.str();
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				if (ser_value.size() <= 100) {
					// For text and string, only add relatively short values
					s.insert(std::move(ser_value));
				}
				return;
			} else {
				THROW(ClientError, "Format invalid for {} type: {}", enum_name(spc.sep_types[SPC_CONCRETE_TYPE]), repr(value.to_string()));
			}
		}
		case FieldType::boolean: {
			auto ser_value = Serialise::MsgPack(spc, value);
			if (field_spc != nullptr) {
				index_term(doc, ser_value, *field_spc, pos);
			}
			if (global_spc != nullptr) {
				index_term(doc, ser_value, *global_spc, pos);
			}
			s.insert(std::move(ser_value));
			return;
		}
		case FieldType::uuid: {
			if (value.is_string()) {
				auto ser_value = Serialise::uuid(value.str_view());
				if (field_spc != nullptr) {
					index_term(doc, ser_value, *field_spc, pos);
				}
				if (global_spc != nullptr) {
					index_term(doc, ser_value, *global_spc, pos);
				}
				s.insert(std::move(ser_value));
				return;
			} else {
				THROW(ClientError, "Format invalid for uuid type: {}", repr(value.to_string()));
			}
		}
		default:
			THROW(ClientError, "Type: {:#04x} is an unknown type", toUType(spc.sep_types[SPC_CONCRETE_TYPE]));
	}
}


void
Schema::index_all_value(Xapian::Document& doc, const MsgPack& value, std::set<std::string>& s_f, std::set<std::string>& s_g, const specification_t& field_spc, const specification_t& global_spc, size_t pos)
{
	L_CALL("Schema::index_all_value(<Xapian::Document>, {}, <std::set<std::string>>, <std::set<std::string>>, <specification_t>, <specification_t>, {})", repr(value.to_string()), pos);

	switch (field_spc.sep_types[SPC_CONCRETE_TYPE]) {
		case FieldType::floating: {
			if (value.is_number()) {
				const auto f_val = value.f64();
				auto ser_value = Serialise::floating(f_val);
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				s_f.insert(ser_value);
				s_g.insert(std::move(ser_value));
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, static_cast<int64_t>(f_val));
				} else {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, static_cast<int64_t>(f_val));
					GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, static_cast<int64_t>(f_val));
				}
				return;
			} else {
				THROW(ClientError, "Format invalid for float type: {}", repr(value.to_string()));
			}
		}
		case FieldType::integer: {
			if (value.is_number()) {
				const auto i_val = value.i64();
				auto ser_value = Serialise::integer(i_val);
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				s_f.insert(ser_value);
				s_g.insert(std::move(ser_value));
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, i_val);
				} else {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, i_val);
					GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, i_val);
				}
				return;
			} else {
				THROW(ClientError, "Format invalid for integer type: {}", value.to_string());
			}
		}
		case FieldType::positive: {
			if (value.is_number()) {
				const auto u_val = value.u64();
				auto ser_value = Serialise::positive(u_val);
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				s_f.insert(ser_value);
				s_g.insert(std::move(ser_value));
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::positive(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, u_val);
				} else {
					GenerateTerms::positive(doc, field_spc.accuracy, field_spc.acc_prefix, u_val);
					GenerateTerms::positive(doc, global_spc.accuracy, global_spc.acc_prefix, u_val);
				}
				return;
			} else {
				THROW(ClientError, "Format invalid for positive type: {}", repr(value.to_string()));
			}
		}
		case FieldType::date:
		case FieldType::datetime: {
			Datetime::tm_t tm;
			auto ser_value = Serialise::datetime(value, tm);
			if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
				index_term(doc, ser_value, field_spc, pos);
			}
			if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
				index_term(doc, ser_value, global_spc, pos);
			}
			s_f.insert(ser_value);
			s_g.insert(std::move(ser_value));
			if (field_spc.accuracy == global_spc.accuracy) {
				GenerateTerms::datetime(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, tm);
			} else {
				GenerateTerms::datetime(doc, field_spc.accuracy, field_spc.acc_prefix, tm);
				GenerateTerms::datetime(doc, global_spc.accuracy, global_spc.acc_prefix, tm);
			}
			return;
		}
		case FieldType::time: {
			double t_val = 0.0;
			auto ser_value = Serialise::time(value, t_val);
			if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
				index_term(doc, ser_value, field_spc, pos);
			}
			if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
				index_term(doc, ser_value, global_spc, pos);
			}
			s_f.insert(ser_value);
			s_g.insert(std::move(ser_value));
			if (field_spc.accuracy == global_spc.accuracy) {
				GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, t_val);
			} else {
				GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, t_val);
				GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, t_val);
			}
			return;
		}
		case FieldType::timedelta: {
			double t_val;
			auto ser_value = Serialise::timedelta(value, t_val);
			if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
				index_term(doc, ser_value, field_spc, pos);
			}
			if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
				index_term(doc, ser_value, global_spc, pos);
			}
			s_f.insert(ser_value);
			s_g.insert(std::move(ser_value));
			if (field_spc.accuracy == global_spc.accuracy) {
				GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, t_val);
			} else {
				GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, t_val);
				GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, t_val);
			}
			return;
		}
		case FieldType::geo: {
			GeoSpatial geo(value);
			const auto& geometry = geo.getGeometry();
			auto ranges = geometry->getRanges(field_spc.flags.partials, field_spc.error);
			if (ranges.empty()) {
				return;
			}
			if (field_spc.flags.partials == global_spc.flags.partials && field_spc.error == global_spc.error) {
				if (toUType(field_spc.index & TypeIndex::TERMS) != 0u) {
					auto ser_value = Serialise::ranges_hash(ranges);
					if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
						index_term(doc, ser_value, field_spc, pos);
					}
					if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
						index_term(doc, std::move(ser_value), global_spc, pos);
					}
				}
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::geo(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, ranges);
				} else {
					GenerateTerms::geo(doc, field_spc.accuracy, field_spc.acc_prefix, ranges);
					GenerateTerms::geo(doc, global_spc.accuracy, global_spc.acc_prefix, ranges);
				}
				merge_geospatial_values(s_f, ranges, geometry->getCentroids());
				merge_geospatial_values(s_g, std::move(ranges), geometry->getCentroids());
			} else {
				auto g_ranges = geometry->getRanges(global_spc.flags.partials, global_spc.error);
				if (toUType(field_spc.index & TypeIndex::TERMS) != 0u) {
					const auto ser_value = Serialise::ranges_hash(g_ranges);
					if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
						index_term(doc, ser_value, field_spc, pos);
					}
					if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
						index_term(doc, std::move(ser_value), global_spc, pos);
					}
				}
				GenerateTerms::geo(doc, field_spc.accuracy, field_spc.acc_prefix, ranges);
				GenerateTerms::geo(doc, global_spc.accuracy, global_spc.acc_prefix, g_ranges);
				merge_geospatial_values(s_f, std::move(ranges), geometry->getCentroids());
				merge_geospatial_values(s_g, std::move(g_ranges), geometry->getCentroids());
			}
			return;
		}
		case FieldType::keyword: {
			if (value.is_string()) {
				auto ser_value = value.str();
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				s_f.insert(ser_value);
				s_g.insert(std::move(ser_value));
				return;
			} else {
				THROW(ClientError, "Format invalid for {} type: {}", enum_name(field_spc.sep_types[SPC_CONCRETE_TYPE]), repr(value.to_string()));
			}
		}
		case FieldType::string:
		case FieldType::text: {
			if (value.is_string()) {
				auto ser_value = value.str();
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				if (ser_value.size() <= 100) {
					// For text and string, only add relatively short values
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				return;
			} else {
				THROW(ClientError, "Format invalid for {} type: {}", enum_name(field_spc.sep_types[SPC_CONCRETE_TYPE]), repr(value.to_string()));
			}
		}
		case FieldType::boolean: {
			auto ser_value = Serialise::MsgPack(field_spc, value);
			if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
				index_term(doc, ser_value, field_spc, pos);
			}
			if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
				index_term(doc, ser_value, global_spc, pos);
			}
			s_f.insert(ser_value);
			s_g.insert(std::move(ser_value));
			return;
		}
		case FieldType::uuid: {
			if (value.is_string()) {
				auto ser_value = Serialise::uuid(value.str_view());
				if (toUType(field_spc.index & TypeIndex::FIELD_TERMS) != 0u) {
					index_term(doc, ser_value, field_spc, pos);
				}
				if (toUType(field_spc.index & TypeIndex::GLOBAL_TERMS) != 0u) {
					index_term(doc, ser_value, global_spc, pos);
				}
				s_f.insert(ser_value);
				s_g.insert(std::move(ser_value));
				return;
			} else {
				THROW(ClientError, "Format invalid for uuid type: {}", repr(value.to_string()));
			}
		}
		default:
			THROW(ClientError, "Type: {:#04x} is an unknown type", toUType(field_spc.sep_types[SPC_CONCRETE_TYPE]));
	}
}


inline void
Schema::update_prefixes()
{
	L_CALL("Schema::update_prefixes()");

	if (specification.flags.uuid_path) {
		if (specification.flags.uuid_field) {
			switch (specification.index_uuid_field) {
				case UUIDFieldIndex::uuid: {
					specification.flags.has_uuid_prefix = true;
					specification.prefix.field.append(specification.local_prefix.uuid);
					if (!specification.prefix.uuid.empty()) {
						specification.prefix.uuid.append(specification.local_prefix.uuid);
					}
					specification.local_prefix.field = std::move(specification.local_prefix.uuid);
					specification.local_prefix.uuid.clear();
					break;
				}
				case UUIDFieldIndex::uuid_field: {
					specification.prefix.field.append(specification.local_prefix.field);
					if (!specification.prefix.uuid.empty()) {
						specification.prefix.uuid.append(specification.local_prefix.field);
					}
					specification.local_prefix.uuid.clear();
					break;
				}
				case UUIDFieldIndex::both: {
					if (specification.prefix.uuid.empty()) {
						specification.prefix.uuid = specification.prefix.field;
					}
					specification.prefix.field.append(specification.local_prefix.field);
					specification.prefix.uuid.append(specification.local_prefix.uuid);
					break;
				}
				case UUIDFieldIndex::INVALID:
					break;
			}
		} else {
			specification.prefix.field.append(specification.local_prefix.field);
			if (!specification.prefix.uuid.empty()) {
				specification.prefix.uuid.append(specification.local_prefix.field);
			}
		}
	} else {
		specification.prefix.field.append(specification.local_prefix.field);
	}

	if (specification.flags.partial_paths) {
		if (specification.partial_prefixes.empty()) {
			specification.partial_prefixes.push_back(specification.prefix);
		} else {
			specification.partial_prefixes.push_back(specification.local_prefix);
		}
	} else {
		specification.partial_prefixes.clear();
	}
}


inline void
Schema::verify_dynamic(std::string_view field_name)
{
	L_CALL("Schema::verify_dynamic({})", repr(field_name));

	if (field_name == UUID_FIELD_NAME) {
		specification.meta_name.assign(UUID_FIELD_NAME);
		specification.flags.uuid_field = true;
		specification.flags.uuid_path = true;
	} else {
		specification.local_prefix.field.assign(get_prefix(field_name));
		specification.meta_name.assign(field_name);
		specification.flags.uuid_field = false;
	}
}


inline void
Schema::detect_dynamic(std::string_view field_name)
{
	L_CALL("Schema::detect_dynamic({})", repr(field_name));

	if (Serialise::possiblyUUID(field_name)) {
		try {
			auto ser_uuid = Serialise::uuid(field_name);
			specification.local_prefix.uuid.assign(ser_uuid);
			static const auto uuid_field_prefix = get_prefix(UUID_FIELD_NAME);
			specification.local_prefix.field.assign(uuid_field_prefix);
			specification.meta_name.assign(UUID_FIELD_NAME);
			specification.flags.uuid_field = true;
			specification.flags.uuid_path = true;
		} catch (const SerialisationError&) {
			specification.local_prefix.field.assign(get_prefix(field_name));
			specification.meta_name.assign(field_name);
			specification.flags.uuid_field = false;
		}
	} else {
		specification.local_prefix.field.assign(get_prefix(field_name));
		specification.meta_name.assign(field_name);
		specification.flags.uuid_field = false;
	}
}


inline void
Schema::dispatch_process_concrete_properties(const MsgPack& object, FieldVector& fields, Field** id_field)
{
	L_CALL("Schema::dispatch_process_concrete_properties({}, <fields>)", repr(object.to_string()));

	const auto it_e = object.end();
	for (auto it = object.begin(); it != it_e; ++it) {
		auto str_key = it->str_view();
		auto& value = it.value();
		if (is_reserved(str_key)) {
			auto key = hh(str_key);
			if (!_dispatch_process_concrete_properties(key, str_key, value)) {
				fields.emplace_back(str_key, &value);
				if (id_field != nullptr && key == hh(ID_FIELD_NAME)) {
					*id_field = &fields.back();
				}
			}
		} else {
			fields.emplace_back(str_key, &value);
		}
	}

#ifdef XAPIAND_CHAISCRIPT
	normalize_script();
#endif
}


inline void
Schema::dispatch_process_all_properties(const MsgPack& object, FieldVector& fields, Field** id_field)
{
	L_CALL("Schema::dispatch_process_all_properties({}, <fields>)", repr(object.to_string()));

	const auto it_e = object.end();
	for (auto it = object.begin(); it != it_e; ++it) {
		auto str_key = it->str_view();
		auto& value = it.value();
		if (is_reserved(str_key)) {
			auto key = hh(str_key);
			if (!_dispatch_process_properties(key, str_key, value)) {
				if (!_dispatch_process_concrete_properties(key, str_key, value)) {
					fields.emplace_back(str_key, &value);
					if (id_field != nullptr && key == hh(ID_FIELD_NAME)) {
						*id_field = &fields.back();
					}
				}
			}
		} else {
			fields.emplace_back(str_key, &value);
		}
	}

#ifdef XAPIAND_CHAISCRIPT
	normalize_script();
#endif
}


inline void
Schema::dispatch_process_properties(const MsgPack& object, FieldVector& fields, Field** id_field)
{
	if (specification.flags.concrete) {
		dispatch_process_concrete_properties(object, fields, id_field);
	} else {
		dispatch_process_all_properties(object, fields, id_field);
	}
}


inline void
Schema::dispatch_write_concrete_properties(MsgPack& mut_properties, const MsgPack& object, FieldVector& fields, Field** id_field)
{
	L_CALL("Schema::dispatch_write_concrete_properties({}, {}, <fields>)", repr(mut_properties.to_string()), repr(object.to_string()));

	const auto it_e = object.end();
	for (auto it = object.begin(); it != it_e; ++it) {
		auto str_key = it->str_view();
		auto& value = it.value();
		if (is_reserved(str_key)) {
			auto key = hh(str_key);
			if (!_dispatch_write_properties(key, mut_properties, str_key, value)) {
				if (!_dispatch_process_concrete_properties(key, str_key, value)) {
					fields.emplace_back(str_key, &value);
					if (id_field != nullptr && key == hh(ID_FIELD_NAME)) {
						*id_field = &fields.back();
					}
				}
			}
		} else {
			fields.emplace_back(str_key, &value);
		}
	}

#ifdef XAPIAND_CHAISCRIPT
	write_script(mut_properties);
#endif
}


inline bool
Schema::_dispatch_write_properties(uint32_t key, MsgPack& mut_properties, std::string_view prop_name, const MsgPack& value)
{
	L_CALL("Schema::_dispatch_write_properties({})", repr(mut_properties.to_string()));

	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_WEIGHT),
		hh(RESERVED_POSITION),
		hh(RESERVED_SPELLING),
		hh(RESERVED_POSITIONS),
		hh(RESERVED_INDEX),
		hh(RESERVED_STORE),
		hh(RESERVED_RECURSE),
		hh(RESERVED_IGNORE),
		hh(RESERVED_DYNAMIC),
		hh(RESERVED_STRICT),
		hh(RESERVED_DATE_DETECTION),
		hh(RESERVED_DATETIME_DETECTION),
		hh(RESERVED_TIME_DETECTION),
		hh(RESERVED_TIMEDELTA_DETECTION),
		hh(RESERVED_NUMERIC_DETECTION),
		hh(RESERVED_GEO_DETECTION),
		hh(RESERVED_BOOL_DETECTION),
		hh(RESERVED_TEXT_DETECTION),
		hh(RESERVED_UUID_DETECTION),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_NAMESPACE),
		hh(RESERVED_PARTIAL_PATHS),
		hh(RESERVED_INDEX_UUID_FIELD),
		hh(RESERVED_SCHEMA),
		hh(RESERVED_SETTINGS),
	});

	switch (_.find(key)) {
		case _.fhh(RESERVED_WEIGHT):
			write_weight(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_POSITION):
			write_position(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_SPELLING):
			write_spelling(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_POSITIONS):
			write_positions(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_INDEX):
			write_index(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_STORE):
			write_store(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_RECURSE):
			write_recurse(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_IGNORE):
			write_ignore(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_DYNAMIC):
			write_dynamic(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_STRICT):
			write_strict(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_DATE_DETECTION):
			write_date_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_DATETIME_DETECTION):
			write_datetime_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_TIME_DETECTION):
			write_time_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_TIMEDELTA_DETECTION):
			write_timedelta_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_NUMERIC_DETECTION):
			write_numeric_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_GEO_DETECTION):
			write_geo_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOL_DETECTION):
			write_bool_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_TEXT_DETECTION):
			write_text_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_UUID_DETECTION):
			write_uuid_detection(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOL_TERM):
			write_bool_term(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_NAMESPACE):
			write_namespace(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_PARTIAL_PATHS):
			write_partial_paths(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_INDEX_UUID_FIELD):
			write_index_uuid_field(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_SCHEMA):
			write_schema(mut_properties, prop_name, value);
			return true;
		case _.fhh(RESERVED_SETTINGS):
			write_settings(mut_properties, prop_name, value);
			return true;
		default:
			return false;
	}
}


inline bool
Schema::_dispatch_feed_properties(uint32_t key, const MsgPack& value)
{
	L_CALL("Schema::_dispatch_feed_properties({})", repr(value.to_string()));

	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_WEIGHT),
		hh(RESERVED_POSITION),
		hh(RESERVED_SPELLING),
		hh(RESERVED_POSITIONS),
		hh(RESERVED_TYPE),
		hh(RESERVED_PREFIX),
		hh(RESERVED_SLOT),
		hh(RESERVED_INDEX),
		hh(RESERVED_STORE),
		hh(RESERVED_RECURSE),
		hh(RESERVED_IGNORE),
		hh(RESERVED_DYNAMIC),
		hh(RESERVED_STRICT),
		hh(RESERVED_DATE_DETECTION),
		hh(RESERVED_DATETIME_DETECTION),
		hh(RESERVED_TIME_DETECTION),
		hh(RESERVED_TIMEDELTA_DETECTION),
		hh(RESERVED_NUMERIC_DETECTION),
		hh(RESERVED_GEO_DETECTION),
		hh(RESERVED_BOOL_DETECTION),
		hh(RESERVED_TEXT_DETECTION),
		hh(RESERVED_UUID_DETECTION),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_ACCURACY),
		hh(RESERVED_ACC_PREFIX),
		hh(RESERVED_NGRAM),
		hh(RESERVED_CJK_NGRAM),
		hh(RESERVED_CJK_WORDS),
		hh(RESERVED_LANGUAGE),
		hh(RESERVED_STOP_STRATEGY),
		hh(RESERVED_STEM_STRATEGY),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_PARTIALS),
		hh(RESERVED_ERROR),
		hh(RESERVED_NAMESPACE),
		hh(RESERVED_PARTIAL_PATHS),
		hh(RESERVED_INDEX_UUID_FIELD),
		hh(RESERVED_SCRIPT),
		hh(RESERVED_ENDPOINT),
	});

	switch (_.find(key)) {
		case _.fhh(RESERVED_WEIGHT):
			Schema::feed_weight(value);
			return true;
		case _.fhh(RESERVED_POSITION):
			Schema::feed_position(value);
			return true;
		case _.fhh(RESERVED_SPELLING):
			Schema::feed_spelling(value);
			return true;
		case _.fhh(RESERVED_POSITIONS):
			Schema::feed_positions(value);
			return true;
		case _.fhh(RESERVED_TYPE):
			Schema::feed_type(value);
			return true;
		case _.fhh(RESERVED_PREFIX):
			Schema::feed_prefix(value);
			return true;
		case _.fhh(RESERVED_SLOT):
			Schema::feed_slot(value);
			return true;
		case _.fhh(RESERVED_INDEX):
			Schema::feed_index(value);
			return true;
		case _.fhh(RESERVED_STORE):
			Schema::feed_store(value);
			return true;
		case _.fhh(RESERVED_RECURSE):
			Schema::feed_recurse(value);
			return true;
		case _.fhh(RESERVED_IGNORE):
			Schema::feed_ignore(value);
			return true;
		case _.fhh(RESERVED_DYNAMIC):
			Schema::feed_dynamic(value);
			return true;
		case _.fhh(RESERVED_STRICT):
			Schema::feed_strict(value);
			return true;
		case _.fhh(RESERVED_DATE_DETECTION):
			Schema::feed_date_detection(value);
			return true;
		case _.fhh(RESERVED_DATETIME_DETECTION):
			Schema::feed_datetime_detection(value);
			return true;
		case _.fhh(RESERVED_TIME_DETECTION):
			Schema::feed_time_detection(value);
			return true;
		case _.fhh(RESERVED_TIMEDELTA_DETECTION):
			Schema::feed_timedelta_detection(value);
			return true;
		case _.fhh(RESERVED_NUMERIC_DETECTION):
			Schema::feed_numeric_detection(value);
			return true;
		case _.fhh(RESERVED_GEO_DETECTION):
			Schema::feed_geo_detection(value);
			return true;
		case _.fhh(RESERVED_BOOL_DETECTION):
			Schema::feed_bool_detection(value);
			return true;
		case _.fhh(RESERVED_TEXT_DETECTION):
			Schema::feed_text_detection(value);
			return true;
		case _.fhh(RESERVED_UUID_DETECTION):
			Schema::feed_uuid_detection(value);
			return true;
		case _.fhh(RESERVED_BOOL_TERM):
			Schema::feed_bool_term(value);
			return true;
		case _.fhh(RESERVED_ACCURACY):
			Schema::feed_accuracy(value);
			return true;
		case _.fhh(RESERVED_ACC_PREFIX):
			Schema::feed_acc_prefix(value);
			return true;
		case _.fhh(RESERVED_NGRAM):
			Schema::feed_ngram(value);
			return true;
		case _.fhh(RESERVED_CJK_NGRAM):
			Schema::feed_cjk_ngram(value);
			return true;
		case _.fhh(RESERVED_CJK_WORDS):
			Schema::feed_cjk_words(value);
			return true;
		case _.fhh(RESERVED_LANGUAGE):
			Schema::feed_language(value);
			return true;
		case _.fhh(RESERVED_STOP_STRATEGY):
			Schema::feed_stop_strategy(value);
			return true;
		case _.fhh(RESERVED_STEM_STRATEGY):
			Schema::feed_stem_strategy(value);
			return true;
		case _.fhh(RESERVED_STEM_LANGUAGE):
			Schema::feed_stem_language(value);
			return true;
		case _.fhh(RESERVED_PARTIALS):
			Schema::feed_partials(value);
			return true;
		case _.fhh(RESERVED_ERROR):
			Schema::feed_error(value);
			return true;
		case _.fhh(RESERVED_NAMESPACE):
			Schema::feed_namespace(value);
			return true;
		case _.fhh(RESERVED_PARTIAL_PATHS):
			Schema::feed_partial_paths(value);
			return true;
		case _.fhh(RESERVED_INDEX_UUID_FIELD):
			Schema::feed_index_uuid_field(value);
			return true;
		case _.fhh(RESERVED_SCRIPT):
			Schema::feed_script(value);
			return true;
		case _.fhh(RESERVED_ENDPOINT):
			Schema::feed_endpoint(value);
			return true;
		default:
			return false;
	}
}


inline bool
has_dispatch_process_properties(uint32_t key)
{
	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_NGRAM),
		hh(RESERVED_CJK_NGRAM),
		hh(RESERVED_CJK_WORDS),
		hh(RESERVED_LANGUAGE),
		hh(RESERVED_PREFIX),
		hh(RESERVED_SLOT),
		hh(RESERVED_STOP_STRATEGY),
		hh(RESERVED_STEM_STRATEGY),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_TYPE),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_ACCURACY),
		hh(RESERVED_ACC_PREFIX),
		hh(RESERVED_PARTIALS),
		hh(RESERVED_ERROR),
	});

	return _.count(key) != 0u;
}

inline bool
Schema::_dispatch_process_properties(uint32_t key, std::string_view prop_name, const MsgPack& value)
{
	L_CALL("Schema::_dispatch_process_properties({})", repr(prop_name));

	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_NGRAM),
		hh(RESERVED_CJK_NGRAM),
		hh(RESERVED_CJK_WORDS),
		hh(RESERVED_LANGUAGE),
		hh(RESERVED_PREFIX),
		hh(RESERVED_SLOT),
		hh(RESERVED_STOP_STRATEGY),
		hh(RESERVED_STEM_STRATEGY),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_TYPE),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_ACCURACY),
		hh(RESERVED_ACC_PREFIX),
		hh(RESERVED_PARTIALS),
		hh(RESERVED_ERROR),
	});

	switch (_.find(key)) {
		case _.fhh(RESERVED_NGRAM):
			Schema::process_ngram(prop_name, value);
			return true;
		case _.fhh(RESERVED_CJK_NGRAM):
			Schema::process_cjk_ngram(prop_name, value);
			return true;
		case _.fhh(RESERVED_CJK_WORDS):
			Schema::process_cjk_words(prop_name, value);
			return true;
		case _.fhh(RESERVED_LANGUAGE):
			Schema::process_language(prop_name, value);
			return true;
		case _.fhh(RESERVED_PREFIX):
			Schema::process_prefix(prop_name, value);
			return true;
		case _.fhh(RESERVED_SLOT):
			Schema::process_slot(prop_name, value);
			return true;
		case _.fhh(RESERVED_STOP_STRATEGY):
			Schema::process_stop_strategy(prop_name, value);
			return true;
		case _.fhh(RESERVED_STEM_STRATEGY):
			Schema::process_stem_strategy(prop_name, value);
			return true;
		case _.fhh(RESERVED_STEM_LANGUAGE):
			Schema::process_stem_language(prop_name, value);
			return true;
		case _.fhh(RESERVED_TYPE):
			Schema::process_type(prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOL_TERM):
			Schema::process_bool_term(prop_name, value);
			return true;
		case _.fhh(RESERVED_ACCURACY):
			Schema::process_accuracy(prop_name, value);
			return true;
		case _.fhh(RESERVED_ACC_PREFIX):
			Schema::process_acc_prefix(prop_name, value);
			return true;
		case _.fhh(RESERVED_PARTIALS):
			Schema::process_partials(prop_name, value);
			return true;
		case _.fhh(RESERVED_ERROR):
			Schema::process_error(prop_name, value);
			return true;
		default:
			return false;
	}
}

inline bool
has_dispatch_process_concrete_properties(uint32_t key)
{
	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_DATA),
		hh(RESERVED_WEIGHT),
		hh(RESERVED_POSITION),
		hh(RESERVED_SPELLING),
		hh(RESERVED_POSITIONS),
		hh(RESERVED_INDEX),
		hh(RESERVED_STORE),
		hh(RESERVED_RECURSE),
		hh(RESERVED_IGNORE),
		hh(RESERVED_PARTIAL_PATHS),
		hh(RESERVED_INDEX_UUID_FIELD),
		hh(RESERVED_VALUE),
		hh(RESERVED_ENDPOINT),
		hh(RESERVED_SCRIPT),
		hh(RESERVED_FLOAT),
		hh(RESERVED_POSITIVE),
		hh(RESERVED_INTEGER),
		hh(RESERVED_BOOLEAN),
		hh(RESERVED_TERM),  // FIXME: remove legacy term
		hh(RESERVED_KEYWORD),
		hh(RESERVED_TEXT),
		hh(RESERVED_STRING),
		hh(RESERVED_DATETIME),
		hh(RESERVED_UUID),
		hh(RESERVED_EWKT),
		hh(RESERVED_POINT),
		hh(RESERVED_CIRCLE),
		hh(RESERVED_CONVEX),
		hh(RESERVED_POLYGON),
		hh(RESERVED_CHULL),
		hh(RESERVED_MULTIPOINT),
		hh(RESERVED_MULTICIRCLE),
		hh(RESERVED_MULTICONVEX),
		hh(RESERVED_MULTIPOLYGON),
		hh(RESERVED_MULTICHULL),
		hh(RESERVED_GEO_COLLECTION),
		hh(RESERVED_GEO_INTERSECTION),
		hh(RESERVED_CHAI),
		// Next functions only check the consistency of user provided data.
		hh(RESERVED_SLOT),
		hh(RESERVED_NGRAM),
		hh(RESERVED_CJK_NGRAM),
		hh(RESERVED_CJK_WORDS),
		hh(RESERVED_LANGUAGE),
		hh(RESERVED_STOP_STRATEGY),
		hh(RESERVED_STEM_STRATEGY),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_TYPE),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_ACCURACY),
		hh(RESERVED_PARTIALS),
		hh(RESERVED_ERROR),
		hh(RESERVED_DYNAMIC),
		hh(RESERVED_STRICT),
		hh(RESERVED_DATE_DETECTION),
		hh(RESERVED_DATETIME_DETECTION),
		hh(RESERVED_TIME_DETECTION),
		hh(RESERVED_TIMEDELTA_DETECTION),
		hh(RESERVED_NUMERIC_DETECTION),
		hh(RESERVED_GEO_DETECTION),
		hh(RESERVED_BOOL_DETECTION),
		hh(RESERVED_TEXT_DETECTION),
		hh(RESERVED_UUID_DETECTION),
		hh(RESERVED_NAMESPACE),
		hh(RESERVED_SCHEMA),
		hh(RESERVED_SETTINGS),
	});
	return _.count(key) != 0u;
}

inline bool
Schema::_dispatch_process_concrete_properties(uint32_t key, std::string_view prop_name, const MsgPack& value)
{
	L_CALL("Schema::_dispatch_process_concrete_properties({})", repr(prop_name));

	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_DATA),
		hh(RESERVED_WEIGHT),
		hh(RESERVED_POSITION),
		hh(RESERVED_SPELLING),
		hh(RESERVED_POSITIONS),
		hh(RESERVED_INDEX),
		hh(RESERVED_STORE),
		hh(RESERVED_RECURSE),
		hh(RESERVED_IGNORE),
		hh(RESERVED_PARTIAL_PATHS),
		hh(RESERVED_INDEX_UUID_FIELD),
		hh(RESERVED_VALUE),
		hh(RESERVED_ENDPOINT),
		hh(RESERVED_SCRIPT),
		hh(RESERVED_FLOAT),
		hh(RESERVED_POSITIVE),
		hh(RESERVED_INTEGER),
		hh(RESERVED_BOOLEAN),
		hh(RESERVED_TERM),  // FIXME: remove legacy term
		hh(RESERVED_KEYWORD),
		hh(RESERVED_TEXT),
		hh(RESERVED_STRING),
		hh(RESERVED_DATETIME),
		hh(RESERVED_UUID),
		hh(RESERVED_EWKT),
		hh(RESERVED_POINT),
		hh(RESERVED_CIRCLE),
		hh(RESERVED_CONVEX),
		hh(RESERVED_POLYGON),
		hh(RESERVED_CHULL),
		hh(RESERVED_MULTIPOINT),
		hh(RESERVED_MULTICIRCLE),
		hh(RESERVED_MULTICONVEX),
		hh(RESERVED_MULTIPOLYGON),
		hh(RESERVED_MULTICHULL),
		hh(RESERVED_GEO_COLLECTION),
		hh(RESERVED_GEO_INTERSECTION),
		hh(RESERVED_CHAI),
		// Next functions only check the consistency of user provided data.
		hh(RESERVED_SLOT),
		hh(RESERVED_NGRAM),
		hh(RESERVED_CJK_NGRAM),
		hh(RESERVED_CJK_WORDS),
		hh(RESERVED_LANGUAGE),
		hh(RESERVED_STOP_STRATEGY),
		hh(RESERVED_STEM_STRATEGY),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_TYPE),
		hh(RESERVED_BOOL_TERM),
		hh(RESERVED_ACCURACY),
		hh(RESERVED_PARTIALS),
		hh(RESERVED_ERROR),
		hh(RESERVED_DYNAMIC),
		hh(RESERVED_STRICT),
		hh(RESERVED_DATE_DETECTION),
		hh(RESERVED_DATETIME_DETECTION),
		hh(RESERVED_TIME_DETECTION),
		hh(RESERVED_TIMEDELTA_DETECTION),
		hh(RESERVED_NUMERIC_DETECTION),
		hh(RESERVED_GEO_DETECTION),
		hh(RESERVED_BOOL_DETECTION),
		hh(RESERVED_TEXT_DETECTION),
		hh(RESERVED_UUID_DETECTION),
		hh(RESERVED_NAMESPACE),
		hh(RESERVED_SCHEMA),
		hh(RESERVED_SETTINGS),
	});

	switch (_.find(key)) {
		case _.fhh(RESERVED_DATA):
			Schema::process_data(prop_name, value);
			return true;
		case _.fhh(RESERVED_WEIGHT):
			Schema::process_weight(prop_name, value);
			return true;
		case _.fhh(RESERVED_POSITION):
			Schema::process_position(prop_name, value);
			return true;
		case _.fhh(RESERVED_SPELLING):
			Schema::process_spelling(prop_name, value);
			return true;
		case _.fhh(RESERVED_POSITIONS):
			Schema::process_positions(prop_name, value);
			return true;
		case _.fhh(RESERVED_INDEX):
			Schema::process_index(prop_name, value);
			return true;
		case _.fhh(RESERVED_STORE):
			Schema::process_store(prop_name, value);
			return true;
		case _.fhh(RESERVED_RECURSE):
			Schema::process_recurse(prop_name, value);
			return true;
		case _.fhh(RESERVED_IGNORE):
			Schema::process_ignore(prop_name, value);
			return true;
		case _.fhh(RESERVED_PARTIAL_PATHS):
			Schema::process_partial_paths(prop_name, value);
			return true;
		case _.fhh(RESERVED_INDEX_UUID_FIELD):
			Schema::process_index_uuid_field(prop_name, value);
			return true;
		case _.fhh(RESERVED_VALUE):
			Schema::process_value(prop_name, value);
			return true;
		case _.fhh(RESERVED_ENDPOINT):
			Schema::process_endpoint(prop_name, value);
			return true;
		case _.fhh(RESERVED_SCRIPT):
			Schema::process_script(prop_name, value);
			return true;
		case _.fhh(RESERVED_FLOAT):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_POSITIVE):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_INTEGER):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOLEAN):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_TERM):  // FIXME: remove legacy term
		case _.fhh(RESERVED_KEYWORD):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_TEXT):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_STRING):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_DATETIME):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_UUID):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_EWKT):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_POINT):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_CIRCLE):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_CONVEX):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_POLYGON):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_CHULL):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_MULTIPOINT):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_MULTICIRCLE):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_MULTICONVEX):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_MULTIPOLYGON):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_MULTICHULL):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_GEO_COLLECTION):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_GEO_INTERSECTION):
			Schema::process_cast_object(prop_name, value);
			return true;
		case _.fhh(RESERVED_CHAI):
			Schema::process_cast_object(prop_name, value);
			return true;
		// Next functions only check the consistency of user provided data.
		case _.fhh(RESERVED_SLOT):
			Schema::consistency_slot(prop_name, value);
			return true;
		case _.fhh(RESERVED_NGRAM):
			Schema::consistency_ngram(prop_name, value);
			return true;
		case _.fhh(RESERVED_CJK_NGRAM):
			Schema::consistency_cjk_ngram(prop_name, value);
			return true;
		case _.fhh(RESERVED_CJK_WORDS):
			Schema::consistency_cjk_words(prop_name, value);
			return true;
		case _.fhh(RESERVED_LANGUAGE):
			Schema::consistency_language(prop_name, value);
			return true;
		case _.fhh(RESERVED_STOP_STRATEGY):
			Schema::consistency_stop_strategy(prop_name, value);
			return true;
		case _.fhh(RESERVED_STEM_STRATEGY):
			Schema::consistency_stem_strategy(prop_name, value);
			return true;
		case _.fhh(RESERVED_STEM_LANGUAGE):
			Schema::consistency_stem_language(prop_name, value);
			return true;
		case _.fhh(RESERVED_TYPE):
			Schema::consistency_type(prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOL_TERM):
			Schema::consistency_bool_term(prop_name, value);
			return true;
		case _.fhh(RESERVED_ACCURACY):
			Schema::consistency_accuracy(prop_name, value);
			return true;
		case _.fhh(RESERVED_PARTIALS):
			Schema::consistency_partials(prop_name, value);
			return true;
		case _.fhh(RESERVED_ERROR):
			Schema::consistency_error(prop_name, value);
			return true;
		case _.fhh(RESERVED_DYNAMIC):
			Schema::consistency_dynamic(prop_name, value);
			return true;
		case _.fhh(RESERVED_STRICT):
			Schema::consistency_strict(prop_name, value);
			return true;
		case _.fhh(RESERVED_DATE_DETECTION):
			Schema::consistency_date_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_DATETIME_DETECTION):
			Schema::consistency_datetime_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_TIME_DETECTION):
			Schema::consistency_time_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_TIMEDELTA_DETECTION):
			Schema::consistency_timedelta_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_NUMERIC_DETECTION):
			Schema::consistency_numeric_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_GEO_DETECTION):
			Schema::consistency_geo_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_BOOL_DETECTION):
			Schema::consistency_bool_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_TEXT_DETECTION):
			Schema::consistency_text_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_UUID_DETECTION):
			Schema::consistency_uuid_detection(prop_name, value);
			return true;
		case _.fhh(RESERVED_NAMESPACE):
			Schema::consistency_namespace(prop_name, value);
			return true;
		case _.fhh(RESERVED_SCHEMA):
			Schema::consistency_schema(prop_name, value);
			return true;
		case _.fhh(RESERVED_SETTINGS):
			Schema::consistency_settings(prop_name, value);
			return true;
		default:
			return false;
	}
}


void
Schema::dispatch_write_all_properties(MsgPack& mut_properties, const MsgPack& object, FieldVector& fields, Field** id_field)
{
	L_CALL("Schema::dispatch_write_all_properties({}, {}, <fields>)", repr(mut_properties.to_string()), repr(object.to_string()));

	auto it_e = object.end();
	for (auto it = object.begin(); it != it_e; ++it) {
		auto str_key = it->str_view();
		auto& value = it.value();
		if (is_reserved(str_key)) {
			auto key = hh(str_key);
			if (!_dispatch_write_properties(key, mut_properties, str_key, value)) {
				if (!_dispatch_process_properties(key, str_key, value)) {
					if (!_dispatch_process_concrete_properties(key, str_key, value)) {
						fields.emplace_back(str_key, &value);
						if (id_field != nullptr && key == hh(ID_FIELD_NAME)) {
							*id_field = &fields.back();
						}
					}
				}
			}
		} else {
			fields.emplace_back(str_key, &value);
		}
	}

#ifdef XAPIAND_CHAISCRIPT
	write_script(mut_properties);
#endif
}


inline void
Schema::dispatch_write_properties(MsgPack& mut_properties, const MsgPack& object, FieldVector& fields, Field** id_field)
{
	L_CALL("Schema::dispatch_write_properties({}, <object>, <fields>)", repr(mut_properties.to_string()));

	if (specification.flags.concrete) {
		dispatch_write_concrete_properties(mut_properties, object, fields, id_field);
	} else {
		dispatch_write_all_properties(mut_properties, object, fields, id_field);
	}
}


inline bool
has_dispatch_set_default_spc(uint32_t key)
{
	constexpr static auto _ = phf::make_phf({
		hh(ID_FIELD_NAME),
		hh(RESERVED_VERSION),
	});
	return _.count(key) != 0u;
}


inline void
Schema::dispatch_set_default_spc(MsgPack& mut_properties)
{
	L_CALL("Schema::dispatch_set_default_spc({})", repr(mut_properties.to_string()));

	auto key = hh(specification.full_meta_name);
	constexpr static auto _ = phf::make_phf({
		hh(ID_FIELD_NAME),
		hh(RESERVED_VERSION),
	});
	switch (_.find(key)) {
		case _.fhh(ID_FIELD_NAME):
			set_default_spc_id(mut_properties);
			break;
		case _.fhh(RESERVED_VERSION):
			set_default_spc_version(mut_properties);
			break;
	}
}


void
Schema::add_field(MsgPack*& mut_properties, const MsgPack& object, FieldVector& fields)
{
	L_CALL("Schema::add_field({}, {}, <fields>)", repr(mut_properties->to_string()), repr(object.to_string()));

	specification.flags.field_found = false;

	mut_properties = &mut_properties->get(specification.meta_name);

	const auto& stem = _get_stem_language(specification.meta_name);
	if (stem.first && stem.second != "unknown") {
		specification.language = stem.second;
		specification.aux_language = stem.second;
	}

	if (specification.full_meta_name.empty()) {
		specification.full_meta_name.assign(specification.meta_name);
	} else {
		specification.full_meta_name.append(1, DB_OFFSPRING_UNION).append(specification.meta_name);
	}

	// Write obj specifications.
	dispatch_write_all_properties(*mut_properties, object, fields);

	// Load default specifications.
	dispatch_set_default_spc(*mut_properties);

	// Write prefix in properties.
	mut_properties->get(RESERVED_PREFIX) = specification.local_prefix.field;

	update_prefixes();
}


void
Schema::add_field(MsgPack*& mut_properties)
{
	L_CALL("Schema::add_field({})", repr(mut_properties->to_string()));

	mut_properties = &mut_properties->get(specification.meta_name);

	const auto& stem = _get_stem_language(specification.meta_name);
	if (stem.first && stem.second != "unknown") {
		specification.language = stem.second;
		specification.aux_language = stem.second;
	}

	if (specification.full_meta_name.empty()) {
		specification.full_meta_name.assign(specification.meta_name);
	} else {
		specification.full_meta_name.append(1, DB_OFFSPRING_UNION).append(specification.meta_name);
	}

	// Load default specifications.
	dispatch_set_default_spc(*mut_properties);

	// Write prefix in properties.
	mut_properties->get(RESERVED_PREFIX) = specification.local_prefix.field;

	update_prefixes();
}


void
Schema::dispatch_feed_properties(const MsgPack& properties)
{
	L_CALL("Schema::dispatch_feed_properties({})", repr(properties.to_string()));

	const auto it_e = properties.end();
	for (auto it = properties.begin(); it != it_e; ++it) {
		auto str_key = it->str_view();
		if (is_reserved(str_key)) {
			auto& value = it.value();
			auto key = hh(str_key);
			_dispatch_feed_properties(key, value);
		}
	}
}


void
Schema::feed_weight(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_weight({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.weight.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_number()) {
				specification.weight.push_back(static_cast<Xapian::termpos>(prop_item_obj.u64()));
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_WEIGHT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else if (prop_obj.is_number()) {
		specification.weight.clear();
		specification.weight.push_back(static_cast<Xapian::termpos>(prop_obj.u64()));
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_WEIGHT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_position(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_position({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.position.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_number()) {
				specification.position.push_back(static_cast<Xapian::termpos>(prop_item_obj.u64()));
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_WEIGHT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else if (prop_obj.is_number()) {
		specification.position.clear();
		specification.position.push_back(static_cast<Xapian::termpos>(prop_obj.u64()));
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_POSITION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_spelling(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_spelling({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.spelling.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_boolean()) {
				specification.spelling.push_back(prop_item_obj.boolean());
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_SPELLING, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else if (prop_obj.is_boolean()) {
		specification.spelling.clear();
		specification.spelling.push_back(prop_obj.boolean());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_SPELLING, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_positions(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_positions({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.positions.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_boolean()) {
				specification.positions.push_back(prop_item_obj.boolean());
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_POSITIONS, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else if (prop_obj.is_boolean()) {
		specification.positions.clear();
		specification.positions.push_back(prop_obj.boolean());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_POSITIONS, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_ngram(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.ngram = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_NGRAM, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_cjk_ngram(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_cjk_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.cjk_ngram = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_CJK_NGRAM, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_cjk_words(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_cjk_words({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.cjk_words = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_CJK_WORDS, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_language(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.language = prop_obj.str();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_LANGUAGE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_stop_strategy(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_stop_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.stop_strategy = _get_stop_strategy(prop_obj.str_view());
		if (specification.stop_strategy == StopStrategy::INVALID) {
			THROW(Error, "Schema is corrupt: '{}' in {} must be one of {}.", RESERVED_STOP_STRATEGY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name), str_set_stop_strategy);
		}
	} else if (prop_obj.is_number()) {
		specification.stop_strategy = static_cast<StopStrategy>(prop_obj.u64());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_STOP_STRATEGY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_stem_strategy(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_stem_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.stem_strategy = enum_type<StemStrategy>(prop_obj.str_view());
		if (specification.stem_strategy == StemStrategy::INVALID) {
			THROW(Error, "Schema is corrupt: '{}' in {} must be one of {}.", RESERVED_STEM_STRATEGY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name), str_set_stem_strategy);
		}
	} else if (prop_obj.is_number()) {
		specification.stem_strategy = static_cast<StemStrategy>(prop_obj.u64());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_STEM_STRATEGY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_stem_language(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_stem_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.stem_language = prop_obj.str();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_STEM_LANGUAGE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_type(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_type({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.set_types(prop_obj.str_view());
		specification.flags.concrete = specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_TYPE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_accuracy(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_accuracy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.accuracy.clear();
		specification.accuracy.reserve(prop_obj.size());
		for (const auto& prop_item_obj : prop_obj) {
			uint64_t accuracy;
			if (prop_item_obj.is_string()) {
				auto accuracy_date = _get_accuracy_datetime(prop_item_obj.str_view());
				if (accuracy_date != UnitTime::INVALID) {
					accuracy = toUType(accuracy_date);
				} else {
					THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACCURACY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
			} else if (prop_item_obj.is_number()) {
				accuracy = prop_item_obj.u64();
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACCURACY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
			specification.accuracy.push_back(accuracy);
		}
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACC_PREFIX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_acc_prefix(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_acc_prefix({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.acc_prefix.clear();
		specification.acc_prefix.reserve(prop_obj.size());
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_string()) {
				specification.acc_prefix.push_back(prop_item_obj.str());
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACC_PREFIX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACC_PREFIX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_prefix(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_prefix({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.local_prefix.field.assign(prop_obj.str_view());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_PREFIX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_slot(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_slot({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		specification.slot = static_cast<Xapian::valueno>(prop_obj.u64());
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_SLOT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_index(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_index({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.index = _get_index(prop_obj.str_view());
		if (specification.index == TypeIndex::INVALID) {
			THROW(Error, "Schema is corrupt: '{}' in {} must be one of {}.", RESERVED_INDEX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name), str_set_index);
		}
		specification.flags.has_index = true;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_INDEX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_store(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_store({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.parent_store = specification.flags.store;
		specification.flags.store = prop_obj.boolean() && specification.flags.parent_store;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_STORE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_recurse(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_recurse({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.recurse = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_RECURSE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_ignore(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_ignore({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.ignored.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_string()) {
				auto ignored = prop_item_obj.str();
				if (ignored == "*") {
					specification.flags.recurse = false;
				}
				specification.ignored.insert(ignored);
			} else {
				THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_INDEX, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else if (prop_obj.is_string()) {
		auto ignored = prop_obj.str();
		if (ignored == "*") {
			specification.flags.recurse = false;
		}
		specification.ignored.clear();
		specification.ignored.insert(ignored);
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_IGNORE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_dynamic(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_dynamic({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.dynamic = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_DYNAMIC, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_strict(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_strict({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.strict = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_STRICT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_date_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_date_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.date_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_DATE_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_datetime_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_datetime_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.datetime_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_DATETIME_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_time_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_time_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.time_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_TIME_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_timedelta_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_timedelta_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.timedelta_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_TIMEDELTA_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_numeric_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_numeric_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.numeric_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_NUMERIC_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_geo_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_geo_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.geo_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_GEO_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_bool_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_bool_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.bool_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_BOOL_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_text_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_text_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.text_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_TEXT_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_uuid_detection(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_uuid_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.uuid_detection = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_UUID_DETECTION, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_bool_term(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_bool_term({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.bool_term = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_BOOL_TERM, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_partials(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_partials({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.partials = prop_obj.boolean();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_PARTIALS, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_error(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_error({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		specification.error = prop_obj.f64();
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ERROR, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_namespace(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_namespace({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.is_namespace = prop_obj.boolean();
		specification.flags.has_namespace = true;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_NAMESPACE, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_partial_paths(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_partial_paths({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.partial_paths = prop_obj.boolean();
		specification.flags.has_partial_paths = true;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_PARTIAL_PATHS, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_index_uuid_field(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_index_uuid_field({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.index_uuid_field = _get_index_uuid_field(prop_obj.str_view());
		if (specification.index_uuid_field == UUIDFieldIndex::INVALID) {
			THROW(Error, "Schema is corrupt: '{}' in {} must be one of {}.", RESERVED_INDEX_UUID_FIELD, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name), str_set_index_uuid_field);
		}
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_INDEX_UUID_FIELD, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::feed_script([[maybe_unused]] const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_script({})", repr(prop_obj.to_string()));

#ifdef XAPIAND_CHAISCRIPT
	specification.script = std::make_unique<const MsgPack>(prop_obj);
	specification.flags.normalized_script = true;
#else
	THROW(ClientError, "{} only is allowed when ChaiScript is actived", RESERVED_SCRIPT);
#endif
}


void
Schema::feed_endpoint(const MsgPack& prop_obj)
{
	L_CALL("Schema::feed_endpoint({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.endpoint.assign(prop_obj.str_view());
		specification.flags.static_endpoint = true;
	} else {
		THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ENDPOINT, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
	}
}


void
Schema::write_position(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_POSITION is heritable and can change between documents.
	L_CALL("Schema::write_position({})", repr(prop_obj.to_string()));

	process_position(prop_name, prop_obj);
	mut_properties[prop_name] = specification.position;
}


void
Schema::write_weight(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_WEIGHT property is heritable and can change between documents.
	L_CALL("Schema::write_weight({})", repr(prop_obj.to_string()));

	process_weight(prop_name, prop_obj);
	mut_properties[prop_name] = specification.weight;
}


void
Schema::write_spelling(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SPELLING is heritable and can change between documents.
	L_CALL("Schema::write_spelling({})", repr(prop_obj.to_string()));

	process_spelling(prop_name, prop_obj);
	mut_properties[prop_name] = specification.spelling;
}


void
Schema::write_positions(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_POSITIONS is heritable and can change between documents.
	L_CALL("Schema::write_positions({})", repr(prop_obj.to_string()));

	process_positions(prop_name, prop_obj);
	mut_properties[prop_name] = specification.positions;
}


void
Schema::write_index(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_INDEX is heritable and can change.
	L_CALL("Schema::write_index({})", repr(prop_obj.to_string()));

	process_index(prop_name, prop_obj);
	mut_properties[prop_name] = _get_str_index(specification.index);
}


void
Schema::write_store(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_store({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_STORE is heritable and can change, but once fixed in false
	 * it cannot change in its offsprings.
	 */

	process_store(prop_name, prop_obj);
	mut_properties[prop_name] = prop_obj.boolean();
}


void
Schema::write_recurse(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_recurse({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_RECURSE is heritable and can change, but once fixed in false
	 * it does not process its children.
	 */

	process_recurse(prop_name, prop_obj);
	mut_properties[prop_name] = static_cast<bool>(specification.flags.recurse);
}


void
Schema::write_ignore(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_ignore({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_IGNORE is heritable and can change, but once fixed in false
	 * it does not process its children.
	 */

	process_ignore(prop_name, prop_obj);
	if (!specification.ignored.empty()) {
		mut_properties[prop_name] = MsgPack::ARRAY();
		for (const auto& item : specification.ignored) {
			mut_properties[prop_name].append(item);
		}
	}
}


void
Schema::write_dynamic(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DYNAMIC is heritable but can't change.
	L_CALL("Schema::write_dynamic({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.dynamic = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.dynamic);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_strict(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STRICT is heritable but can't change.
	L_CALL("Schema::write_strict({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.strict = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.strict);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_date_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DATE_DETECTION is heritable and can't change.
	L_CALL("Schema::write_date_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.date_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.date_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_datetime_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DATETIME_DETECTION is heritable and can't change.
	L_CALL("Schema::write_datetime_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.datetime_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.datetime_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_time_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TIME_DETECTION is heritable and can't change.
	L_CALL("Schema::write_time_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.time_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.time_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_timedelta_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TD_DETECTION is heritable and can't change.
	L_CALL("Schema::write_timedelta_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.timedelta_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.timedelta_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_numeric_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_N_DETECTION is heritable and can't change.
	L_CALL("Schema::write_numeric_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.numeric_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.numeric_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_geo_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_G_DETECTION is heritable and can't change.
	L_CALL("Schema::write_geo_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.geo_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.geo_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_bool_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_B_DETECTION is heritable and can't change.
	L_CALL("Schema::write_bool_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.bool_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.bool_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_text_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_T_DETECTION is heritable and can't change.
	L_CALL("Schema::write_text_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.text_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.text_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_uuid_detection(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_U_DETECTION is heritable and can't change.
	L_CALL("Schema::write_uuid_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.uuid_detection = prop_obj.boolean();
		mut_properties[prop_name] = static_cast<bool>(specification.flags.uuid_detection);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_bool_term(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_BOOL_TERM isn't heritable and can't change.
	L_CALL("Schema::write_bool_term({})", repr(prop_obj.to_string()));

	process_bool_term(prop_name, prop_obj);
	mut_properties[prop_name] = static_cast<bool>(specification.flags.bool_term);
}


void
Schema::write_namespace(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_NAMESPACE isn't heritable and can't change once fixed.
	L_CALL("Schema::write_namespace({})", repr(prop_obj.to_string()));

	if (specification.flags.field_found) {
		return consistency_namespace(prop_name, prop_obj);
	}

	if (prop_obj.is_boolean()) {
		// Only save in Schema if RESERVED_NAMESPACE is true.
		specification.flags.is_namespace = prop_obj.boolean();
		if (specification.flags.is_namespace && !specification.flags.has_partial_paths) {
			specification.flags.partial_paths = specification.flags.partial_paths;
		}
		specification.flags.has_namespace = true;
		mut_properties[prop_name] = static_cast<bool>(specification.flags.is_namespace);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::write_partial_paths(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_partial_paths({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_PARTIAL_PATHS is heritable and can change.
	 */

	process_partial_paths(prop_name, prop_obj);
	mut_properties[prop_name] = static_cast<bool>(specification.flags.partial_paths);
}


void
Schema::write_index_uuid_field(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_index_uuid_field({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_INDEX_UUID_FIELD is heritable and can change.
	 */

	process_index_uuid_field(prop_name, prop_obj);
	mut_properties[prop_name] = _get_str_index_uuid_field(specification.index_uuid_field);
}


void
Schema::write_schema(MsgPack& /*unused*/, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_schema({})", repr(prop_obj.to_string()));

	consistency_schema(prop_name, prop_obj);
}


void
Schema::write_settings(MsgPack& /*unused*/, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_settings({})", repr(prop_obj.to_string()));

	consistency_settings(prop_name, prop_obj);
}


void
Schema::write_endpoint(MsgPack& mut_properties, std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::write_endpoint({})", repr(prop_obj.to_string()));

	process_endpoint(prop_name, prop_obj);
	specification.flags.static_endpoint = true;
	mut_properties[prop_name] = specification.endpoint;
}


void
Schema::process_ngram(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.ngram = prop_obj.boolean();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_cjk_ngram(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_cjk_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.cjk_ngram = prop_obj.boolean();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_cjk_words(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_cjk_words({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.cjk_words = prop_obj.boolean();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_language(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		const auto str_language = prop_obj.str_view();
		const auto& stem = _get_stem_language(str_language);
		if (stem.first && stem.second != "unknown") {
			specification.language = stem.second;
			specification.aux_language = stem.second;
		} else {
			THROW(ClientError, "{}: {} is not supported", repr(prop_name), repr(str_language));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_prefix(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_prefix isn't heritable and can't change once fixed.
	L_CALL("Schema::process_prefix({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.local_prefix.field.assign(prop_obj.str_view());
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_slot(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SLOT isn't heritable and can't change once fixed.
	L_CALL("Schema::process_slot({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		auto slot = static_cast<Xapian::valueno>(prop_obj.u64());
		if (slot == Xapian::BAD_VALUENO) {
			THROW(ClientError, "{} invalid slot ({} not supported)", repr(prop_name), slot);
		}
		specification.slot = slot;
	} else {
		THROW(ClientError, "Data inconsistency, {} must be integer", repr(prop_name));
	}
}


void
Schema::process_stop_strategy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STOP_STRATEGY isn't heritable and can't change once fixed.
	L_CALL("Schema::process_stop_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		auto str_stop_strategy = prop_obj.str_view();
		specification.stop_strategy = _get_stop_strategy(str_stop_strategy);
		if (specification.stop_strategy == StopStrategy::INVALID) {
			THROW(ClientError, "{} can be in {} ({} not supported)", repr(prop_name), str_set_stop_strategy, repr(str_stop_strategy));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_stem_strategy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STEM_STRATEGY isn't heritable and can't change once fixed.
	L_CALL("Schema::process_stem_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		auto str_stem_strategy = prop_obj.str_view();
		specification.stem_strategy = enum_type<StemStrategy>(str_stem_strategy);
		if (specification.stem_strategy == StemStrategy::INVALID) {
			THROW(ClientError, "{} can be in {} ({} not supported)", repr(prop_name), str_set_stem_strategy, repr(str_stem_strategy));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_stem_language(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STEM_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_stem_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		auto str_stem_language = prop_obj.str_view();
		const auto& stem = _get_stem_language(str_stem_language);
		if (stem.second != "unknown") {
			specification.stem_language = stem.second.empty() ? stem.second : str_stem_language;
			specification.aux_stem_language = stem.second;
		} else {
			THROW(ClientError, "{}: {} is not supported", repr(prop_name), repr(str_stem_language));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


void
Schema::process_type(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TYPE isn't heritable and can't change once fixed.
	L_CALL("Schema::process_type({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		specification.set_types(prop_obj.str_view());
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
	if (!specification.endpoint.empty()) {
		if (specification.sep_types[SPC_FOREIGN_TYPE] != FieldType::foreign) {
			THROW(ClientError, "Data inconsistency, {} must be foreign", repr(prop_name));
		}
	}
}


void
Schema::process_accuracy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_ACCURACY isn't heritable and can't change once fixed.
	L_CALL("Schema::process_accuracy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.doc_acc = std::make_unique<const MsgPack>(prop_obj);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be array", repr(prop_name));
	}
}


void
Schema::process_acc_prefix(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_ACC_PREFIX isn't heritable and can't change once fixed.
	L_CALL("Schema::process_acc_prefix({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		specification.acc_prefix.clear();
		specification.acc_prefix.reserve(prop_obj.size());
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_string()) {
				specification.acc_prefix.push_back(prop_item_obj.str());
			} else {
				THROW(ClientError, "Data inconsistency, {} must be an array of strings", repr(prop_name));
			}
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be an array of strings", repr(prop_name));
	}
}


void
Schema::process_bool_term(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_BOOL_TERM isn't heritable and can't change.
	L_CALL("Schema::process_bool_term({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.bool_term = prop_obj.boolean();
		specification.flags.has_bool_term = true;
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a boolean", repr(prop_name));
	}
}


void
Schema::process_partials(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	L_CALL("Schema::process_partials({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		specification.flags.partials = prop_obj.boolean();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


void
Schema::process_error(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	L_CALL("Schema::process_error({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		specification.error = prop_obj.f64();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a double", repr(prop_name));
	}
}


void
Schema::process_position(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_POSITION is heritable and can change between documents.
	L_CALL("Schema::process_position({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		if (prop_obj.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
		}
		specification.position.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_number()) {
				specification.position.push_back(static_cast<unsigned>(prop_item_obj.u64()));
			} else {
				THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
			}
		}
	} else if (prop_obj.is_number()) {
		specification.position.clear();
		specification.position.push_back(static_cast<unsigned>(prop_obj.u64()));
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
	}
}


inline void
Schema::process_data(std::string_view /*unused*/, [[maybe_unused]] const MsgPack& prop_obj)
{
	// RESERVED_DATA is ignored by the schema.
	L_CALL("Schema::process_data({})", repr(prop_obj.to_string()));
}


inline void
Schema::process_weight(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_WEIGHT property is heritable and can change between documents.
	L_CALL("Schema::process_weight({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		if (prop_obj.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
		}
		specification.weight.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_number()) {
				specification.weight.push_back(static_cast<unsigned>(prop_item_obj.u64()));
			} else {
				THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
			}
		}
	} else if (prop_obj.is_number()) {
		specification.weight.clear();
		specification.weight.push_back(static_cast<unsigned>(prop_obj.u64()));
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
	}
}


inline void
Schema::process_spelling(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SPELLING is heritable and can change between documents.
	L_CALL("Schema::process_spelling({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		if (prop_obj.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a boolean or a not-empty array of booleans", repr(prop_name));
		}
		specification.spelling.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_boolean()) {
				specification.spelling.push_back(prop_item_obj.boolean());
			} else {
				THROW(ClientError, "Data inconsistency, {} must be a positive integer or a not-empty array of positive integers", repr(prop_name));
			}
		}
	} else if (prop_obj.is_boolean()) {
		specification.spelling.clear();
		specification.spelling.push_back(prop_obj.boolean());
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a boolean or a not-empty array of booleans", repr(prop_name));
	}
}


inline void
Schema::process_positions(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_POSITIONS is heritable and can change between documents.
	L_CALL("Schema::process_positions({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		if (prop_obj.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a boolean or a not-empty array of booleans", repr(prop_name));
		}
		specification.positions.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_boolean()) {
				specification.positions.push_back(prop_item_obj.boolean());
			} else {
				THROW(ClientError, "Data inconsistency, {} must be a boolean or a not-empty array of booleans", repr(prop_name));
			}
		}
	} else if (prop_obj.is_boolean()) {
		specification.positions.clear();
		specification.positions.push_back(prop_obj.boolean());
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a boolean or a not-empty array of booleans", repr(prop_name));
	}
}


inline void
Schema::process_index(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_INDEX is heritable and can change.
	L_CALL("Schema::process_index({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		auto str_index = prop_obj.str_view();
		specification.index = _get_index(str_index);
		if (specification.index == TypeIndex::INVALID) {
			THROW(ClientError, "{} not supported, {} must be one of {}", repr(str_index), repr(prop_name), str_set_index);
		}
		specification.flags.has_index = true;
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::process_store(std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::process_store({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_STORE is heritable and can change, but once fixed in false
	 * it cannot change in its offsprings.
	 */

	if (prop_obj.is_boolean()) {
		specification.flags.store = specification.flags.parent_store && prop_obj.boolean();
		specification.flags.parent_store = specification.flags.store;
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::process_recurse(std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::process_recurse({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_RECURSE is heritable and can change, but once fixed in false
	 * it does not process its children.
	 */

	if (prop_obj.is_boolean()) {
		specification.flags.recurse = prop_obj.boolean();
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::process_ignore(std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::process_ignore({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_IGNORE is heritable and can change, but once fixed in false
	 * it does not process its children.
	 */

	if (prop_obj.is_array()) {
		specification.ignored.clear();
		for (const auto& prop_item_obj : prop_obj) {
			if (prop_item_obj.is_string()) {
				auto ignored = prop_item_obj.str();
				if (ignored == "*") {
					specification.flags.recurse = false;
				}
				specification.ignored.insert(ignored);
			} else {
				THROW(ClientError, "Data inconsistency, {} must be an array of strings", repr(prop_name));
			}
		}
	} else if (prop_obj.is_string()) {
		auto ignored = prop_obj.str();
		if (ignored == "*") {
			specification.flags.recurse = false;
		}
		specification.ignored.clear();
		specification.ignored.insert(ignored);
	} else {
		THROW(ClientError, "Data inconsistency, {} must be an array of strings", repr(prop_name));
	}
}


inline void
Schema::process_partial_paths(std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::process_partial_paths({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_PARTIAL_PATHS is heritable and can change.
	 */

	if (prop_obj.is_boolean()) {
		specification.flags.partial_paths = prop_obj.boolean();
		specification.flags.has_partial_paths = true;
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::process_index_uuid_field(std::string_view prop_name, const MsgPack& prop_obj)
{
	L_CALL("Schema::process_index_uuid_field({})", repr(prop_obj.to_string()));

	/*
	 * RESERVED_INDEX_UUID_FIELD is heritable and can change.
	 */

	if (prop_obj.is_string()) {
		auto str_index_uuid_field = prop_obj.str_view();
		specification.index_uuid_field = _get_index_uuid_field(str_index_uuid_field);
		if (specification.index_uuid_field == UUIDFieldIndex::INVALID) {
			THROW(ClientError, "{} not supported, {} must be one of {} ({} not supported)", repr(str_index_uuid_field), repr(prop_name), str_set_index_uuid_field);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::process_value(std::string_view /*unused*/, const MsgPack& prop_obj)
{
	// RESERVED_VALUE isn't heritable and is not saved in schema.
	L_CALL("Schema::process_value({})", repr(prop_obj.to_string()));

	if (specification.value || specification.value_rec) {
		THROW(ClientError, "Object already has a value");
	} else {
		specification.value = std::make_unique<const MsgPack>(prop_obj);
	}
}


inline void
Schema::process_script(std::string_view /*unused*/, [[maybe_unused]] const MsgPack& prop_obj)
{
	// RESERVED_SCRIPT isn't heritable.
	L_CALL("Schema::process_script({})", repr(prop_obj.to_string()));

#ifdef XAPIAND_CHAISCRIPT
	specification.script = std::make_unique<const MsgPack>(prop_obj);
	specification.flags.normalized_script = false;
#else
	THROW(ClientError, "'{}' only is allowed when ChaiScript is actived", RESERVED_SCRIPT);
#endif
}


inline void
Schema::process_endpoint(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_ENDPOINT isn't heritable.
	L_CALL("Schema::process_endpoint({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		const auto _endpoint = prop_obj.str_view();
		if (_endpoint.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a valid endpoint", repr(prop_name));
		}
		std::string_view _path, _id;
		split_path_id(_endpoint, _path, _id);
		if (_path.empty() || _id.empty()) {
			THROW(ClientError, "Data inconsistency, {} must be a valid endpoint", repr(prop_name));
		}
		if (specification.endpoint != _endpoint) {
			if (
				specification.sep_types[SPC_FOREIGN_TYPE] != FieldType::foreign && (
					specification.sep_types[SPC_OBJECT_TYPE] != FieldType::empty ||
					specification.sep_types[SPC_ARRAY_TYPE] != FieldType::empty ||
					specification.sep_types[SPC_CONCRETE_TYPE] != FieldType::empty
				)
			) {
				THROW(ClientError, "Data inconsistency, {} cannot be used in non-foreign fields", repr(prop_name));
			}
			specification.flags.static_endpoint = false;
			specification.endpoint.assign(_endpoint);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::process_cast_object(std::string_view prop_name, const MsgPack& prop_obj)
{
	// This property isn't heritable and is not saved in schema.
	L_CALL("Schema::process_cast_object({})", repr(prop_obj.to_string()));

	if (specification.value || specification.value_rec) {
		THROW(ClientError, "Object already has a value");
	} else {
		specification.value_rec = std::make_unique<const MsgPack>(MsgPack({
			{ prop_name, prop_obj },
		}));
	}
}


inline void
Schema::consistency_slot(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SLOT isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_slot({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		auto slot = static_cast<Xapian::valueno>(prop_obj.u64());
		if (specification.slot != slot) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), specification.slot, slot, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be integer", repr(prop_name));
	}
}


inline void
Schema::consistency_ngram(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto ngram = prop_obj.boolean();
		if (specification.flags.ngram != ngram) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), bool(specification.flags.ngram), ngram, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_cjk_ngram(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_cjk_ngram({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto cjk_ngram = prop_obj.boolean();
		if (specification.flags.cjk_ngram != cjk_ngram) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), bool(specification.flags.cjk_ngram), cjk_ngram, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_cjk_words(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_cjk_words({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto cjk_words = prop_obj.boolean();
		if (specification.flags.cjk_words != cjk_words) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), bool(specification.flags.cjk_words), cjk_words, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_language(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		const auto str_language = prop_obj.str_view();
		if (specification.language != str_language) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), specification.language, repr(str_language), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_stop_strategy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STOP_STRATEGY isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_stop_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::text) {
			const auto _stop_strategy = strings::lower(prop_obj.str_view());
			const auto stop_strategy = enum_name(specification.stop_strategy);
			if (stop_strategy != _stop_strategy) {
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), stop_strategy, _stop_strategy, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		} else {
			THROW(ClientError, "{} only is allowed in text type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_stem_strategy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STEM_STRATEGY isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_stem_strategy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::text) {
			const auto _stem_strategy = strings::lower(prop_obj.str_view());
			const auto stem_strategy = enum_name(specification.stem_strategy);
			if (stem_strategy != _stem_strategy) {
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(stem_strategy), repr(_stem_strategy), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		} else {
			THROW(ClientError, "{} only is allowed in text type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_stem_language(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STEM_LANGUAGE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_stem_language({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::text) {
			const auto _stem_language = strings::lower(prop_obj.str_view());
			if (specification.stem_language != _stem_language) {
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(specification.stem_language), repr(_stem_language), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		} else {
			THROW(ClientError, "{} only is allowed in text type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}
}


inline void
Schema::consistency_type(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TYPE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_type({})", repr(prop_obj.to_string()));

	if (prop_obj.is_string()) {
		const auto _str_type = prop_obj.str_view();
		auto init_pos = _str_type.rfind('/');
		if (init_pos == std::string::npos) {
			init_pos = 0;
		} else {
			++init_pos;
		}
		const auto str_type = enum_name(specification.sep_types[SPC_CONCRETE_TYPE]);
		if (_str_type.compare(init_pos, std::string::npos, str_type) != 0) {
			auto str_concretr_type = _str_type.substr(init_pos);
			if ((str_concretr_type != "term" || str_type != "keyword") && (str_concretr_type != "keyword" || str_type != "term")) {  // FIXME: remove legacy term
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(str_type), repr(str_concretr_type), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be string", repr(prop_name));
	}

	if (!specification.endpoint.empty()) {
		if (specification.sep_types[SPC_FOREIGN_TYPE] != FieldType::foreign) {
			THROW(ClientError, "Data inconsistency, {} must be foreign", repr(prop_name));
		}
	}
}


inline void
Schema::consistency_accuracy(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_ACCURACY isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_accuracy({})", repr(prop_obj.to_string()));

	if (prop_obj.is_array()) {
		std::set<uint64_t> set_acc;
		switch (specification.sep_types[SPC_CONCRETE_TYPE]) {
			case FieldType::geo: {
				if (prop_obj.is_array()) {
					for (const auto& prop_item_obj : prop_obj) {
						if (prop_item_obj.is_number()) {
							set_acc.insert(prop_item_obj.u64());
						} else {
							THROW(ClientError, "Data inconsistency, level value in '{}': '{}' must be a positive number between 0 and {}", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL);
						}
					}
				} else {
					THROW(ClientError, "Data inconsistency, level value in '{}': '{}' must be a positive number between 0 and {}", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL);
				}
				if (!std::equal(specification.accuracy.begin(), specification.accuracy.end(), set_acc.begin(), set_acc.end())) {
					std::string str_accuracy, _str_accuracy;
					for (const auto& acc : set_acc) {
						str_accuracy.append(strings::format("{}", acc)).push_back(' ');
					}
					for (const auto& acc : specification.accuracy) {
						_str_accuracy.append(strings::format("{}", acc)).push_back(' ');
					}
					THROW(ClientError, "It is not allowed to change {} [{} ->  {}] in {}", repr(prop_name), repr(str_accuracy), repr(_str_accuracy), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				return;
			}
			case FieldType::date:
			case FieldType::datetime: {
				if (prop_obj.is_array()) {
					for (const auto& prop_item_obj : prop_obj) {
						uint64_t accuracy;
						if (prop_item_obj.is_string()) {
							auto accuracy_date = _get_accuracy_datetime(prop_item_obj.str_view());
							if (accuracy_date != UnitTime::INVALID) {
								accuracy = toUType(accuracy_date);
							} else {
								THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, DATE_STR, repr(str_set_acc_date), repr(prop_item_obj.str_view()));
							}
						} else if (prop_item_obj.is_number()) {
							accuracy = prop_item_obj.u64();
							if (!validate_acc_date(static_cast<UnitTime>(accuracy))) {
								THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, DATE_STR, repr(str_set_acc_date));
							}
						} else {
							THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, DATE_STR, repr(str_set_acc_date));
						}
						set_acc.insert(accuracy);
					}
				} else {
					THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, DATE_STR, repr(str_set_acc_date));
				}
				if (!std::equal(specification.accuracy.begin(), specification.accuracy.end(), set_acc.begin(), set_acc.end())) {
					std::string str_accuracy, _str_accuracy;
					for (const auto& acc : set_acc) {
						str_accuracy.append(_get_str_acc_date(static_cast<UnitTime>(acc))).push_back(' ');
					}
					for (const auto& acc : specification.accuracy) {
						_str_accuracy.append(_get_str_acc_date(static_cast<UnitTime>(acc))).push_back(' ');
					}
					THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(str_accuracy), repr(_str_accuracy), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				return;
			}
			case FieldType::time:
			case FieldType::timedelta: {
				if (prop_obj.is_array()) {
					for (const auto& prop_item_obj : prop_obj) {
						if (prop_item_obj.is_string()) {
							auto accuracy_time = _get_accuracy_time(prop_item_obj.str_view());
							if (accuracy_time != UnitTime::INVALID) {
								set_acc.insert(toUType(accuracy_time));
							} else {
								THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, DATE_STR, repr(str_set_acc_date), repr(prop_item_obj.str_view()));
							}
						} else {
							THROW(ClientError, "Data inconsistency, '{}': '{}' must be a subset of {} ({} not supported)", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), repr(str_set_acc_time), repr(prop_item_obj.str_view()));
						}
					}
				} else {
					THROW(ClientError, "Data inconsistency, '{}' in '{}' must be a subset of {}", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), repr(str_set_acc_time));
				}
				if (!std::equal(specification.accuracy.begin(), specification.accuracy.end(), set_acc.begin(), set_acc.end())) {
					std::string str_accuracy, _str_accuracy;
					for (const auto& acc : set_acc) {
						str_accuracy.append(_get_str_acc_date(static_cast<UnitTime>(acc))).push_back(' ');
					}
					for (const auto& acc : specification.accuracy) {
						_str_accuracy.append(_get_str_acc_date(static_cast<UnitTime>(acc))).push_back(' ');
					}
					THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(str_accuracy), repr(_str_accuracy), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				return;
			}
			case FieldType::integer:
			case FieldType::positive:
			case FieldType::floating: {
				if (prop_obj.is_array()) {
					for (const auto& prop_item_obj : prop_obj) {
						if (prop_item_obj.is_number()) {
							set_acc.insert(prop_item_obj.u64());
						} else {
							THROW(ClientError, "Data inconsistency, {} in {} must be an array of positive numbers in {}", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
						}
					}
				} else {
					THROW(ClientError, "Data inconsistency, {} in {} must be an array of positive numbers in {}", RESERVED_ACCURACY, enum_name(specification.sep_types[SPC_CONCRETE_TYPE]), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				if (!std::equal(specification.accuracy.begin(), specification.accuracy.end(), set_acc.begin(), set_acc.end())) {
					std::string str_accuracy, _str_accuracy;
					for (const auto& acc : set_acc) {
						str_accuracy.append(strings::format("{}", acc)).push_back(' ');
					}
					for (const auto& acc : specification.accuracy) {
						_str_accuracy.append(strings::format("{}", acc)).push_back(' ');
					}
					THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), repr(str_accuracy), repr(_str_accuracy), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
				return;
			}
			default:
				THROW(ClientError, "{} is not allowed in {} type fields", repr(prop_name), enum_name(specification.sep_types[SPC_CONCRETE_TYPE]));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be array", repr(prop_name));
	}
}


inline void
Schema::consistency_bool_term(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_BOOL_TERM isn't heritable and can't change.
	L_CALL("Schema::consistency_bool_term({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::keyword) {
			const auto _bool_term = prop_obj.boolean();
			if (specification.flags.bool_term != _bool_term) {
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}] in {}", repr(prop_name), bool(specification.flags.bool_term), _bool_term, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		} else {
			THROW(ClientError, "{} only is allowed in keyword type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_partials(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_partials({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::geo) {
			const auto _partials = prop_obj.boolean();
			if (specification.flags.partials != _partials) {
				THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.partials), _partials);
			}
		} else {
			THROW(ClientError, "{} only is allowed in geospatial type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_error(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_error({})", repr(prop_obj.to_string()));

	if (prop_obj.is_number()) {
		if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::geo) {
			const auto _error = prop_obj.f64();
			if (specification.error != _error) {
				THROW(ClientError, "It is not allowed to change {} [{:.2}  ->  {:.2}]", repr(prop_name), specification.error, _error);
			}
		} else {
			THROW(ClientError, "{} only is allowed in geospatial type fields", repr(prop_name));
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be a double", repr(prop_name));
	}
}


inline void
Schema::consistency_dynamic(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DYNAMIC is heritable but can't change.
	L_CALL("Schema::consistency_dynamic({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _dynamic = prop_obj.boolean();
		if (specification.flags.dynamic != _dynamic) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.dynamic), _dynamic);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_strict(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_STRICT is heritable but can't change.
	L_CALL("Schema::consistency_strict({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _strict = prop_obj.boolean();
		if (specification.flags.strict != _strict) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.strict), _strict);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_date_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DATE_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_date_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _date_detection = prop_obj.boolean();
		if (specification.flags.date_detection != _date_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.date_detection), _date_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_datetime_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_DATETIME_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_datetime_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _datetime_detection = prop_obj.boolean();
		if (specification.flags.datetime_detection != _datetime_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.datetime_detection), _datetime_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_time_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TIME_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_time_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _time_detection = prop_obj.boolean();
		if (specification.flags.time_detection != _time_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.time_detection), _time_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_timedelta_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TIMEDELTA_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_timedelta_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _timedelta_detection = prop_obj.boolean();
		if (specification.flags.timedelta_detection != _timedelta_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.timedelta_detection), _timedelta_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_numeric_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_NUMERIC_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_numeric_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _numeric_detection = prop_obj.boolean();
		if (specification.flags.numeric_detection != _numeric_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.numeric_detection), _numeric_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_geo_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_GEO_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_geo_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _geo_detection = prop_obj.boolean();
		if (specification.flags.geo_detection != _geo_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.geo_detection), _geo_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_bool_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_BOOL_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_bool_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _bool_detection = prop_obj.boolean();
		if (specification.flags.bool_detection != _bool_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.bool_detection), _bool_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_text_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_TEXT_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_text_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _text_detection = prop_obj.boolean();
		if (specification.flags.text_detection != _text_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.text_detection), _text_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_uuid_detection(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_UUID_DETECTION is heritable and can't change.
	L_CALL("Schema::consistency_uuid_detection({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _uuid_detection = prop_obj.boolean();
		if (specification.flags.uuid_detection != _uuid_detection) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.uuid_detection), _uuid_detection);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_namespace(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_NAMESPACE isn't heritable and can't change once fixed.
	L_CALL("Schema::consistency_namespace({})", repr(prop_obj.to_string()));

	if (prop_obj.is_boolean()) {
		const auto _is_namespace = prop_obj.boolean();
		if (specification.flags.is_namespace != _is_namespace) {
			THROW(ClientError, "It is not allowed to change {} [{}  ->  {}]", repr(prop_name), bool(specification.flags.is_namespace), _is_namespace);
		}
	} else {
		THROW(ClientError, "Data inconsistency, {} must be boolean", repr(prop_name));
	}
}


inline void
Schema::consistency_schema(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SCHEMA isn't heritable and is only allowed in root object.
	L_CALL("Schema::consistency_schema({})", repr(prop_obj.to_string()));

	if (specification.full_meta_name.empty()) {
		if (!prop_obj.is_string() && !prop_obj.is_map()) {
			THROW(ClientError, "{} must be string or map", repr(prop_name));
		}
	} else {
		THROW(ClientError, "{} is only allowed in root object", repr(prop_name));
	}
}


inline void
Schema::consistency_settings(std::string_view prop_name, const MsgPack& prop_obj)
{
	// RESERVED_SETTINGS isn't heritable and is only allowed in root object.
	L_CALL("Schema::consistency_settings({})", repr(prop_obj.to_string()));

	if (specification.full_meta_name.empty()) {
		if (!prop_obj.is_map()) {
			THROW(ClientError, "{} must be string or map", repr(prop_name));
		}
	} else {
		THROW(ClientError, "{} is only allowed in root object", repr(prop_name));
	}
}


#ifdef XAPIAND_CHAISCRIPT
inline void
Schema::write_script(MsgPack& mut_properties)
{
	// RESERVED_SCRIPT isn't heritable and can't change once fixed.
	L_CALL("Schema::write_script({})", repr(mut_properties.to_string()));

	if (specification.script) {
		Script script(*specification.script);
		specification.script = std::make_unique<const MsgPack>(script.process_script(specification.flags.strict));
		mut_properties[RESERVED_SCRIPT] = *specification.script;
		specification.flags.normalized_script = true;
	}
}


void
Schema::normalize_script()
{
	// RESERVED_SCRIPT isn't heritable.
	L_CALL("Schema::normalize_script()");

	if (specification.script && !specification.flags.normalized_script) {
		Script script(*specification.script);
		specification.script = std::make_unique<const MsgPack>(script.process_script(specification.flags.strict));
		specification.flags.normalized_script = true;
	}
}
#endif


void
Schema::set_namespace_spc_id(required_spc_t& spc)
{
	L_CALL("Schema::set_namespace_spc_id(<spc>)");

	// ID_FIELD_NAME cannot be text or string.
	if (spc.sep_types[SPC_CONCRETE_TYPE] == FieldType::text || spc.sep_types[SPC_CONCRETE_TYPE] == FieldType::string) {
		spc.sep_types[SPC_CONCRETE_TYPE] = FieldType::keyword;
	}
	spc.prefix.field = NAMESPACE_PREFIX_ID_FIELD_NAME;
	spc.slot = get_slot(spc.prefix.field, spc.get_ctype());
}


void
Schema::set_default_spc_id(MsgPack& mut_properties)
{
	L_CALL("Schema::set_default_spc_id({})", repr(mut_properties.to_string()));

	specification.flags.bool_term = true;
	specification.flags.has_bool_term = true;
	mut_properties[RESERVED_BOOL_TERM] = true;  // force bool term

	if (!specification.flags.has_index) {
		const auto index = specification.index | TypeIndex::FIELD_ALL;  // force field_all
		if (specification.index != index) {
			specification.index = index;
			mut_properties[RESERVED_INDEX] = _get_str_index(index);
		}
		specification.flags.has_index = true;
	}

	// ID_FIELD_NAME cannot be TEXT nor STRING.
	if (specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::text || specification.sep_types[SPC_CONCRETE_TYPE] == FieldType::string) {
		specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::keyword;
		L_DEBUG("{} cannot be type string or text, it's type was changed to keyword", ID_FIELD_NAME);
	}

	// Set default prefix
	specification.local_prefix.field = DOCUMENT_ID_TERM_PREFIX;

	// Set default RESERVED_SLOT
	specification.slot = DB_SLOT_ID;
}

void
Schema::set_default_spc_version([[maybe_unused]] MsgPack& mut_properties)
{
	L_CALL("Schema::set_default_spc_version({})", repr(mut_properties.to_string()));

	specification.flags.store = false;
	specification.slot = DB_SLOT_VERSION;
	specification.index = TypeIndex::FIELD_VALUES;
	specification.sep_types[SPC_CONCRETE_TYPE] = FieldType::positive;
}


const MsgPack
Schema::get_full(bool readable) const
{
	L_CALL("Schema::get_full({})", readable);

	auto full_schema = get_schema();
	if (readable) {
		dispatch_readable(full_schema, true);
	}
	if (!origin.empty()) {
		full_schema[RESERVED_TYPE] = "foreign/object";
		full_schema[RESERVED_ENDPOINT] = origin;
	}
	return full_schema;
}


inline bool
Schema::_dispatch_readable(uint32_t key, MsgPack& value, MsgPack& properties)
{
	L_CALL("Schema::_dispatch_readable({})", repr(value.to_string()));

	constexpr static auto _ = phf::make_phf({
		hh(RESERVED_TYPE),
		hh(RESERVED_PREFIX),
		hh(RESERVED_SLOT),
		hh(RESERVED_STEM_LANGUAGE),
		hh(RESERVED_ACC_PREFIX),
		hh(RESERVED_SCRIPT),
	});

	switch (key) {
		case _.fhh(RESERVED_PREFIX):
			return Schema::readable_prefix(value, properties);
		case _.fhh(RESERVED_SLOT):
			return Schema::readable_slot(value, properties);
		case _.fhh(RESERVED_STEM_LANGUAGE):
			return Schema::readable_stem_language(value, properties);
		case _.fhh(RESERVED_ACC_PREFIX):
			return Schema::readable_acc_prefix(value, properties);
		case _.fhh(RESERVED_SCRIPT):
			return Schema::readable_script(value, properties);
		default:
			throw std::out_of_range("Invalid readable");
	}
}


void
Schema::dispatch_readable(MsgPack& item_schema, bool at_root)
{
	L_CALL("Schema::dispatch_readable({}, {})", repr(item_schema.to_string()), at_root);

	// Change this item of schema in readable form.
	for (auto it = item_schema.begin(); it != item_schema.end(); ) {
		auto str_key = it->str_view();
		auto& value = it.value();
		auto key = hh(str_key);
		try {
			if (is_reserved(str_key)) {
				if (!_dispatch_readable(key, value, item_schema)) {
					it = item_schema.erase(it);
					continue;
				}
				++it;
				continue;
			}
		} catch (const std::out_of_range&) { }

		if (is_valid(str_key)) {
			if (value.is_map()) {
				dispatch_readable(value, false);
			}
		} else if (has_dispatch_set_default_spc(key)) {
			if (at_root) {
				it = item_schema.erase(it);
				continue;
			}
			if (value.is_map()) {
				dispatch_readable(value, false);
			}
		}
		++it;
	}
}


inline bool
Schema::readable_prefix(MsgPack& /*unused*/, MsgPack& /*unused*/)
{
	L_CALL("Schema::readable_prefix(...)");

	return false;
}


inline bool
Schema::readable_slot(MsgPack& /*unused*/, MsgPack& /*unused*/)
{
	L_CALL("Schema::readable_slot(...)");

	return false;
}


inline bool
Schema::readable_stem_language(MsgPack& prop_obj, MsgPack& properties)
{
	L_CALL("Schema::readable_stem_language({})", repr(prop_obj.to_string()));

	const auto language = properties[RESERVED_LANGUAGE].str_view();
	const auto stem_language = prop_obj.str_view();

	return (language != stem_language);
}


inline bool
Schema::readable_acc_prefix(MsgPack& /*unused*/, MsgPack& /*unused*/)
{
	L_CALL("Schema::readable_acc_prefix(...)");

	return false;
}


inline bool
Schema::readable_script(MsgPack& prop_obj, MsgPack& /*unused*/)
{
	L_CALL("Schema::readable_script({})", repr(prop_obj.to_string()));

	dispatch_readable(prop_obj, false);
	return true;
}


std::shared_ptr<const MsgPack>
Schema::get_modified_schema()
{
	L_CALL("Schema::get_modified_schema()");

	if (!mut_schema) {
		return std::shared_ptr<const MsgPack>();
	}
	auto m_schema = std::shared_ptr<const MsgPack>(mut_schema.release());
	m_schema->lock();
	return m_schema;
}


std::shared_ptr<const MsgPack>
Schema::get_const_schema() const
{
	L_CALL("Schema::get_const_schema()");

	return schema;
}


std::string
Schema::to_string(bool prettify) const
{
	L_CALL("Schema::to_string({})", prettify);

	return get_full(true).to_string(static_cast<int>(prettify));
}


required_spc_t
_get_data_id(required_spc_t& spc_id, const MsgPack& id_properties)
{
	auto id_it_e = id_properties.end();

	auto id_type_it = id_properties.find(RESERVED_TYPE);
	if (id_type_it != id_it_e) {
		spc_id.sep_types[SPC_CONCRETE_TYPE] = required_spc_t::get_types(id_type_it.value().str_view())[SPC_CONCRETE_TYPE];
	}
	auto id_slot_it = id_properties.find(RESERVED_SLOT);
	if (id_slot_it != id_it_e) {
		spc_id.slot = static_cast<Xapian::valueno>(id_slot_it.value().u64());
	}
	auto id_prefix_it = id_properties.find(RESERVED_PREFIX);
	if (id_slot_it != id_it_e) {
		spc_id.prefix.field.assign(id_prefix_it.value().str_view());
	}

	// Get required specification.
	switch (spc_id.sep_types[SPC_CONCRETE_TYPE]) {
		case FieldType::geo: {
			auto id_partials_it = id_properties.find(RESERVED_PARTIALS);
			if (id_partials_it != id_it_e) {
				spc_id.flags.partials = id_partials_it.value().boolean();
			}
			auto id_error_it = id_properties.find(RESERVED_ERROR);
			if (id_error_it != id_it_e) {
				spc_id.error = id_error_it.value().f64();
			}
			break;
		}
		case FieldType::keyword: {
			auto id_bool_term_it = id_properties.find(RESERVED_BOOL_TERM);
			if (id_bool_term_it != id_it_e) {
				spc_id.flags.bool_term = id_bool_term_it.value().boolean();
			}
			break;
		}
		default:
			break;
	}

	return spc_id;
}


required_spc_t
Schema::get_data_id() const
{
	L_CALL("Schema::get_data_id()");

	required_spc_t spc_id;

	// Set default prefix
	spc_id.prefix.field = DOCUMENT_ID_TERM_PREFIX;

	// Set default RESERVED_SLOT
	spc_id.slot = DB_SLOT_ID;

	const auto& properties = get_newest_properties();
	auto it = properties.find(ID_FIELD_NAME);
	if (it == properties.end()) {
		return spc_id;
	}

	const auto& id_properties = it.value();
	if (!id_properties.is_map()) {
		return spc_id;
	}
	return _get_data_id(spc_id, id_properties);
}


void
Schema::set_data_id(const required_spc_t& spc_id)
{
	L_CALL("Schema::set_data_id(<spc_id>)");

	auto& mut_properties = get_mutable_properties(ID_FIELD_NAME);

	mut_properties[RESERVED_TYPE] = spc_id.get_str_type();
	mut_properties[RESERVED_SLOT] = spc_id.slot;
	mut_properties[RESERVED_PREFIX] = spc_id.prefix.field;

	switch (spc_id.get_type()) {
		case FieldType::geo: {
			mut_properties[RESERVED_PARTIALS] = spc_id.flags.partials;
			mut_properties[RESERVED_ERROR] = spc_id.error;
			break;
		}
		case FieldType::keyword: {
			mut_properties[RESERVED_BOOL_TERM] = spc_id.flags.bool_term;
			break;
		}
		default:
			break;
	}
}


MsgPack
Schema::get_data_script() const
{
	L_CALL("Schema::get_data_script()");

	const auto& properties = get_newest_properties();
	auto it_e = properties.end();
	auto it = properties.find(RESERVED_SCRIPT);
	if (it != it_e) {
		return it.value();
	}
	return MsgPack();
}


std::pair<required_spc_t, std::string>
Schema::get_data_field(std::string_view field_name, bool is_range) const
{
	L_CALL("Schema::get_data_field({}, {})", repr(field_name), is_range);

	required_spc_t res;

	if (field_name.empty()) {
		return std::make_pair(std::move(res), "");
	}

	auto spc = get_dynamic_subproperties(get_properties(), field_name);
	res.flags.inside_namespace = spc.inside_namespace;
	res.prefix.field = std::move(spc.prefix);

	if (!spc.acc_field.empty()) {
		res.sep_types[SPC_CONCRETE_TYPE] = spc.acc_field_type;
		return std::make_pair(res, std::move(spc.acc_field));
	}

	if (!res.flags.inside_namespace) {
		const auto& properties = *spc.properties;
		auto it_e = properties.end();

		auto type_it = properties.find(RESERVED_TYPE);
		if (type_it != it_e) {
			res.sep_types[SPC_CONCRETE_TYPE] = required_spc_t::get_types(type_it.value().str_view())[SPC_CONCRETE_TYPE];
		}
		if (res.sep_types[SPC_CONCRETE_TYPE] == FieldType::empty) {
			return std::make_pair(std::move(res), "");
		}

		if (is_range) {
			if (spc.has_uuid_prefix) {
				res.slot = get_slot(res.prefix.field, res.get_ctype());
			} else {
				auto slot_it = properties.find(RESERVED_SLOT);
				if (slot_it != it_e) {
					res.slot = static_cast<Xapian::valueno>(slot_it.value().u64());
				}
			}

			// Get required specification.
			switch (res.sep_types[SPC_CONCRETE_TYPE]) {
				case FieldType::geo: {
					auto partials_it = properties.find(RESERVED_PARTIALS);
					if (partials_it != it_e) {
						res.flags.partials = partials_it.value().boolean();
					}
					auto error_it = properties.find(RESERVED_ERROR);
					if (error_it != it_e) {
						res.error = error_it.value().f64();
					}
				}
				[[fallthrough]];
				case FieldType::floating:
				case FieldType::integer:
				case FieldType::positive:
				case FieldType::date:
				case FieldType::datetime:
				case FieldType::time:
				case FieldType::timedelta: {
					auto accuracy_it = properties.find(RESERVED_ACCURACY);
					if (accuracy_it != it_e) {
						for (const auto& acc : accuracy_it.value()) {
							uint64_t accuracy;
							if (acc.is_string()) {
								auto accuracy_date = _get_accuracy_datetime(acc.str_view());
								if (accuracy_date != UnitTime::INVALID) {
									accuracy = toUType(accuracy_date);
								} else {
									THROW(Error, "Schema is corrupt: '{}' in {} is not valid.", RESERVED_ACCURACY, specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
								}
							} else {
								accuracy = acc.u64();
							}
							res.accuracy.push_back(accuracy);
						}
					}
					auto acc_prefix_it = properties.find(RESERVED_ACC_PREFIX);
					if (acc_prefix_it != it_e) {
						for (const auto& acc_p : acc_prefix_it.value()) {
							res.acc_prefix.push_back(res.prefix.field + acc_p.str());
						}
					}
					break;
				}
				case FieldType::string:
				case FieldType::text: {
					auto ngram_it = properties.find(RESERVED_NGRAM);
					if (ngram_it != it_e) {
						res.flags.ngram = ngram_it.value().boolean();
					}
					auto cjk_ngram_it = properties.find(RESERVED_CJK_NGRAM);
					if (cjk_ngram_it != it_e) {
						res.flags.cjk_ngram = cjk_ngram_it.value().boolean();
					}
					auto cjk_words_it = properties.find(RESERVED_CJK_WORDS);
					if (cjk_words_it != it_e) {
						res.flags.cjk_words = cjk_words_it.value().boolean();
					}
					auto language_it = properties.find(RESERVED_LANGUAGE);
					if (language_it != it_e) {
						res.language = language_it.value().str();
					}
					if (!res.language.empty()) {
						auto stop_strategy_it = properties.find(RESERVED_STOP_STRATEGY);
						if (stop_strategy_it != it_e) {
							res.stop_strategy = _get_stop_strategy(stop_strategy_it.value().str_view());
						}
					}
					auto stem_language_it = properties.find(RESERVED_STEM_LANGUAGE);
					if (stem_language_it != it_e) {
						res.stem_language = stem_language_it.value().str();
					}
					if (!res.stem_language.empty()) {
						auto stem_strategy_it = properties.find(RESERVED_STEM_STRATEGY);
						if (stem_strategy_it != it_e) {
							res.stem_strategy = enum_type<StemStrategy>(stem_strategy_it.value().str_view());
						}
					}
					break;
				}
				case FieldType::keyword: {
					auto bool_term_it = properties.find(RESERVED_BOOL_TERM);
					if (bool_term_it != it_e) {
						res.flags.bool_term = bool_term_it.value().boolean();
					}
					break;
				}
				default:
					break;
			}
		} else {
			// Get required specification.
			switch (res.sep_types[SPC_CONCRETE_TYPE]) {
				case FieldType::geo: {
					auto partials_it = properties.find(RESERVED_PARTIALS);
					if (partials_it != it_e) {
						res.flags.partials = partials_it.value().boolean();
					}
					auto error_it = properties.find(RESERVED_ERROR);
					if (error_it != it_e) {
						res.error = error_it.value().f64();
					}
					break;
				}
				case FieldType::string:
				case FieldType::text: {
					auto ngram_it = properties.find(RESERVED_NGRAM);
					if (ngram_it != it_e) {
						res.flags.ngram = ngram_it.value().boolean();
					}
					auto cjk_ngram_it = properties.find(RESERVED_CJK_NGRAM);
					if (cjk_ngram_it != it_e) {
						res.flags.cjk_ngram = cjk_ngram_it.value().boolean();
					}
					auto cjk_words_it = properties.find(RESERVED_CJK_WORDS);
					if (cjk_words_it != it_e) {
						res.flags.cjk_words = cjk_words_it.value().boolean();
					}
					auto language_it = properties.find(RESERVED_LANGUAGE);
					if (language_it != it_e) {
						res.language = language_it.value().str();
					}
					if (!res.language.empty()) {
						auto stop_strategy_it = properties.find(RESERVED_STOP_STRATEGY);
						if (stop_strategy_it != it_e) {
							res.stop_strategy = _get_stop_strategy(stop_strategy_it.value().str_view());
						}
					}
					auto stem_language_it = properties.find(RESERVED_STEM_LANGUAGE);
					if (stem_language_it != it_e) {
						res.stem_language = stem_language_it.value().str();
					}
					if (!res.stem_language.empty()) {
						auto stem_strategy_it = properties.find(RESERVED_STEM_STRATEGY);
						if (stem_strategy_it != it_e) {
							res.stem_strategy = enum_type<StemStrategy>(stem_strategy_it.value().str_view());
						}
					}
					break;
				}
				case FieldType::keyword: {
					auto bool_term_it = properties.find(RESERVED_BOOL_TERM);
					if (bool_term_it != it_e) {
						res.flags.bool_term = bool_term_it.value().boolean();
					}
					break;
				}
				default:
					break;
			}
		}
	}

	return std::make_pair(std::move(res), "");
}


required_spc_t
Schema::get_slot_field(std::string_view field_name) const
{
	L_CALL("Schema::get_slot_field({})", repr(field_name));

	required_spc_t res;

	if (field_name.empty()) {
		return res;
	}

	auto spc = get_dynamic_subproperties(get_properties(), field_name);
	res.flags.inside_namespace = spc.inside_namespace;

	if (!spc.acc_field.empty()) {
		THROW(ClientError, "Field {} is an accuracy, therefore does not have slot", repr(field_name));
	}

	if (res.flags.inside_namespace) {
		res.sep_types[SPC_CONCRETE_TYPE] = FieldType::keyword;
		res.slot = get_slot(spc.prefix, res.get_ctype());
	} else {
		const auto& properties = *spc.properties;
		auto it_e = properties.end();

		auto type_it = properties.find(RESERVED_TYPE);
		if (type_it != it_e) {
			res.sep_types[SPC_CONCRETE_TYPE] = required_spc_t::get_types(type_it.value().str_view())[SPC_CONCRETE_TYPE];
		}

		if (spc.has_uuid_prefix) {
			res.slot = get_slot(spc.prefix, res.get_ctype());
		} else {
			auto slot_it = properties.find(RESERVED_SLOT);
			if (slot_it != it_e) {
				res.slot = static_cast<Xapian::valueno>(slot_it.value().u64());
			}
		}

		// Get required specification.
		switch (res.sep_types[SPC_CONCRETE_TYPE]) {
			case FieldType::geo: {
				auto partials_it = properties.find(RESERVED_PARTIALS);
				if (partials_it != it_e) {
					res.flags.partials = partials_it.value().boolean();
				}
				auto error_it = properties.find(RESERVED_ERROR);
				if (error_it != it_e) {
					res.error = error_it.value().f64();
				}
				break;
			}
			case FieldType::string:
			case FieldType::text: {
				auto ngram_it = properties.find(RESERVED_NGRAM);
				if (ngram_it != it_e) {
					res.flags.ngram = ngram_it.value().boolean();
				}
				auto cjk_ngram_it = properties.find(RESERVED_CJK_NGRAM);
				if (cjk_ngram_it != it_e) {
					res.flags.cjk_ngram = cjk_ngram_it.value().boolean();
				}
				auto cjk_words_it = properties.find(RESERVED_CJK_WORDS);
				if (cjk_words_it != it_e) {
					res.flags.cjk_words = cjk_words_it.value().boolean();
				}
				auto language_it = properties.find(RESERVED_LANGUAGE);
				if (language_it != it_e) {
					res.language = language_it.value().str();
				}
				if (!res.language.empty()) {
					auto stop_strategy_it = properties.find(RESERVED_STOP_STRATEGY);
					if (stop_strategy_it != it_e) {
						res.stop_strategy = _get_stop_strategy(stop_strategy_it.value().str_view());
					}
				}
				auto stem_language_it = properties.find(RESERVED_STEM_LANGUAGE);
				if (stem_language_it != it_e) {
					res.stem_language = stem_language_it.value().str();
				}
				if (!res.stem_language.empty()) {
					auto stem_strategy_it = properties.find(RESERVED_STEM_STRATEGY);
					if (stem_strategy_it != it_e) {
						res.stem_strategy = enum_type<StemStrategy>(stem_strategy_it.value().str_view());
					}
				}
				break;
			}
			case FieldType::keyword: {
				auto bool_term_it = properties.find(RESERVED_BOOL_TERM);
				if (bool_term_it != it_e) {
					res.flags.bool_term = bool_term_it.value().boolean();
				}
				break;
			}
			default:
				break;
		}
	}

	return res;
}


Schema::dynamic_spc_t
Schema::get_dynamic_subproperties(const MsgPack& properties, std::string_view full_name) const
{
	L_CALL("Schema::get_dynamic_subproperties({}, {})", repr(properties.to_string()), repr(full_name));

	Split<std::string_view> field_names(full_name, DB_OFFSPRING_UNION);

	dynamic_spc_t spc(&properties);

	const auto it_e = field_names.end();
	const auto it_b = field_names.begin();
	for (auto it = it_b; it != it_e; ++it) {
		auto field_name = *it;
		if (!is_valid(field_name)) {
			// Check if the field_name is accuracy.
			if (it == it_b) {
				if (!has_dispatch_set_default_spc(hh(field_name))) {
					if (++it == it_e) {
						auto acc_data = _get_acc_data(field_name);
						spc.prefix.append(acc_data.first);
						spc.acc_field.assign(std::move(field_name));
						spc.acc_field_type = acc_data.second;
						return spc;
					}
					THROW(ClientError, "The field name: {} in {} is not valid", repr_field(full_name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
			} else if (++it == it_e) {
				auto acc_data = _get_acc_data(field_name);
				spc.prefix.append(acc_data.first);
				spc.acc_field.assign(std::move(field_name));
				spc.acc_field_type = acc_data.second;
				return spc;
			} else {
				THROW(ClientError, "Field {} in {} is not valid", repr_field(full_name, field_name), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
			}
		}

		auto field_it = spc.properties->find(field_name);
		if (field_it != spc.properties->end()) {
			spc.properties = &field_it.value();
			auto prefix_it = spc.properties->find(RESERVED_PREFIX);
			if (prefix_it != spc.properties->end()) {
				spc.prefix.append(prefix_it.value().str());
			} else {
				spc.prefix.append(get_prefix(field_name));
			}
		} else {
			if (Serialise::possiblyUUID(field_name)) {
				try {
					const auto prefix_uuid = Serialise::uuid(field_name);
					spc.has_uuid_prefix = true;
					field_it = spc.properties->find(UUID_FIELD_NAME);
					if (field_it != spc.properties->end()) {
						spc.properties = &field_it.value();
					}
					spc.prefix.append(prefix_uuid);
				} catch (const SerialisationError&) {
					spc.prefix.append(get_prefix(field_name));
				}
			} else {
				spc.prefix.append(get_prefix(field_name));
			}

			// It is a search using partial prefix.
			size_t depth_partials = std::distance(it, it_e);
			if (depth_partials > LIMIT_PARTIAL_PATHS_DEPTH) {
				THROW(ClientError, "Partial paths limit depth is {}, and partial paths provided has a depth of {}", LIMIT_PARTIAL_PATHS_DEPTH, depth_partials);
			}
			spc.inside_namespace = true;
			for (++it; it != it_e; ++it) {
				auto partial_field = *it;
				if (is_valid(partial_field)) {
					if (Serialise::possiblyUUID(field_name)) {
						try {
							spc.prefix.append(Serialise::uuid(partial_field));
							spc.has_uuid_prefix = true;
						} catch (const SerialisationError&) {
							spc.prefix.append(get_prefix(partial_field));
						}
					} else {
						spc.prefix.append(get_prefix(partial_field));
					}
				} else if (++it == it_e) {
					auto acc_data = _get_acc_data(partial_field);
					spc.prefix.append(acc_data.first);
					spc.acc_field.assign(std::move(partial_field));
					spc.acc_field_type = acc_data.second;
					return spc;
				} else {
					THROW(ClientError, "Field {} in {} is not valid", repr_field(full_name, partial_field), specification.full_meta_name.empty() ? "<root>" : repr(specification.full_meta_name));
				}
			}
			return spc;
		}
	}

	return spc;
}
