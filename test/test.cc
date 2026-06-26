// Smoke test for the standalone datetime library.
//
// Exercises datetime.h / datetime.cc: the ISO 8601 + Elasticsearch-style
// date-math parser/formatter, the time and timedelta parsers, the
// timestamp <-> tm_t round-trip, the iso8601() formatter, and the throwing
// error paths (which now raise the std::runtime_error-based DatetimeError /
// DateISOError / TimeError instead of Xapiand's THROW macros).
//
// Build via CMake: cmake -B build && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "datetime.h"


static bool approx(double a, double b, double eps = 1e-6) {
	return std::fabs(a - b) < eps;
}


// ---------------------------------------------------------------------------
// ISO 8601 parsing: full timestamp with fractional seconds and Z, date-only,
// and an explicit offset.
// ---------------------------------------------------------------------------
static void test_iso8601_parse() {
	// Full timestamp with fractional seconds, UTC.
	auto tm = Datetime::DatetimeParser(std::string_view("2021-03-14T15:09:26.5Z"));
	assert(tm.year == 2021 && tm.mon == 3 && tm.day == 14);
	assert(tm.hour == 15 && tm.min == 9 && tm.sec == 26);
	assert(approx(tm.fsec, 0.5));
	assert(tm.is_utc);

	// Date-only.
	auto d = Datetime::DatetimeParser(std::string_view("2021-03-14"));
	assert(d.year == 2021 && d.mon == 3 && d.day == 14);
	assert(d.hour == 0 && d.min == 0 && d.sec == 0);
	assert(!d.is_utc);

	// Offset: +05:30 east of UTC -> stored as the UTC instant.
	auto off = Datetime::DatetimeParser(std::string_view("2021-03-14T15:09:26+05:30"));
	// 15:09:26 +05:30 == 09:39:26 UTC.
	assert(off.year == 2021 && off.mon == 3 && off.day == 14);
	assert(off.hour == 9 && off.min == 39 && off.sec == 26);

	std::printf("datetime iso8601 parse OK: fractional Z, date-only, +05:30 offset\n");
}


// ---------------------------------------------------------------------------
// Date math: anchored expression and a `now`-style relative one are accepted.
// 2021-01-01 + 1 month - 1 day, then rounded down to the day -> 2021-01-31.
// ---------------------------------------------------------------------------
static void test_date_math() {
	// +1M -1d lands on 2021-01-31; /d (single slash) rounds UP to end-of-day.
	auto tm = Datetime::DatetimeParser(std::string_view("2021-01-01||+1M-1d/d"));
	assert(tm.year == 2021 && tm.mon == 1 && tm.day == 31);
	assert(tm.hour == 23 && tm.min == 59 && tm.sec == 59);

	// //d (double slash) rounds DOWN to the start of the day instead.
	auto down = Datetime::DatetimeParser(std::string_view("2021-01-01||+1M-1d//d"));
	assert(down.year == 2021 && down.mon == 1 && down.day == 31);
	assert(down.hour == 0 && down.min == 0 && down.sec == 0);

	// // rounds to the start of the unit; / rounds to the end.
	auto end_of_month = Datetime::DatetimeParser(std::string_view("2021-02-10||/M"));
	assert(end_of_month.year == 2021 && end_of_month.mon == 2 && end_of_month.day == 28);
	assert(end_of_month.hour == 23 && end_of_month.min == 59 && end_of_month.sec == 59);

	auto start_of_month = Datetime::DatetimeParser(std::string_view("2021-02-10||//M"));
	assert(start_of_month.day == 1 && start_of_month.hour == 0 && start_of_month.min == 0);

	std::printf("datetime date-math OK: +1M-1d/d, /M end-of-month, //M start-of-month\n");
}


// ---------------------------------------------------------------------------
// timestamp() <-> to_tm_t() round-trip, plus iso8601() formatting.
// ---------------------------------------------------------------------------
static void test_timestamp_roundtrip() {
	// A known epoch second: 2021-03-14T15:09:26Z == 1615734566.
	Datetime::tm_t tm(2021, 3, 14, 15, 9, 26);
	double ts = Datetime::timestamp(tm);
	assert(approx(ts, 1615734566.0));

	auto back = Datetime::to_tm_t(ts);
	assert(back.year == 2021 && back.mon == 3 && back.day == 14);
	assert(back.hour == 15 && back.min == 9 && back.sec == 26);

	// iso8601() formats a tm_t back to a string.
	Datetime::tm_t utc(2021, 3, 14, 15, 9, 26, 0.0, true);
	assert(Datetime::iso8601(utc) == "2021-03-14T15:09:26Z");

	Datetime::tm_t frac(2021, 3, 14, 15, 9, 26, 0.5, true);
	assert(Datetime::iso8601(frac) == "2021-03-14T15:09:26.5Z");

	// iso8601(double) round-trips a timestamp through the formatter.
	assert(Datetime::iso8601(1615734566.0, true, 'T').substr(0, 19) == "2021-03-14T15:09:26");

	std::printf("datetime timestamp round-trip OK: timestamp<->to_tm_t, iso8601 formatting\n");
}


// ---------------------------------------------------------------------------
// TimeParser / time round-trips.
// ---------------------------------------------------------------------------
static void test_time_parser() {
	auto clk = Datetime::TimeParser(std::string_view("15:09:26.5"));
	assert(clk.hour == 15 && clk.min == 9 && clk.sec == 26);
	assert(approx(clk.fsec, 0.5));

	// Round-trip through the double representation.
	double t = Datetime::time_to_double(clk);
	auto back = Datetime::time_to_clk_t(t);
	assert(back.hour == 15 && back.min == 9 && back.sec == 26);
	assert(approx(back.fsec, 0.5));

	// String form trims trailing zeros.
	assert(Datetime::time_to_string(clk) == "15:09:26.5");

	// hh:mm only.
	auto hm = Datetime::TimeParser(std::string_view("08:30"));
	assert(hm.hour == 8 && hm.min == 30 && hm.sec == 0);

	std::printf("datetime TimeParser OK: 15:09:26.5 parse + double round-trip\n");
}


// ---------------------------------------------------------------------------
// TimedeltaParser / timedelta round-trips.
// ---------------------------------------------------------------------------
static void test_timedelta_parser() {
	auto clk = Datetime::TimedeltaParser(std::string_view("+01:30:00"));
	assert(clk.tz_s == '+' && clk.hour == 1 && clk.min == 30 && clk.sec == 0);

	double t = Datetime::timedelta_to_double(clk);
	assert(approx(t, 5400.0)); // 1h30m == 5400s

	auto back = Datetime::timedelta_to_clk_t(t);
	assert(back.hour == 1 && back.min == 30 && back.sec == 0);

	// Negative delta.
	auto neg = Datetime::TimedeltaParser(std::string_view("-00:00:30"));
	assert(neg.tz_s == '-' && neg.sec == 30);
	assert(approx(Datetime::timedelta_to_double(neg), -30.0));

	assert(Datetime::timedelta_to_string(clk) == "+01:30:00");

	std::printf("datetime TimedeltaParser OK: +01:30:00 parse + double round-trip\n");
}


// ---------------------------------------------------------------------------
// Malformed inputs must throw the right exception type.
// ---------------------------------------------------------------------------
static void test_throwing() {
	// Bad datetime -> DatetimeError.
	bool threw = false;
	try {
		Datetime::DatetimeParser(std::string_view("not-a-date"));
	} catch (const DatetimeError&) {
		threw = true;
	}
	assert(threw);

	// Out-of-range date -> DatetimeError (a DatetimeError subtype is fine too).
	threw = false;
	try {
		Datetime::DatetimeParser(std::string_view("2021-13-01"));
	} catch (const DatetimeError&) {
		threw = true;
	}
	assert(threw);

	// Bad time -> TimeError (which is a DatetimeError).
	threw = false;
	try {
		Datetime::TimeParser(std::string_view("99:99x"));
	} catch (const TimeError& e) {
		threw = true;
		// TimeError is catchable as DatetimeError too.
		const DatetimeError* base = &e;
		(void)base;
	}
	assert(threw);

	// Bad timedelta -> TimedeltaError.
	threw = false;
	try {
		Datetime::TimedeltaParser(std::string_view("01:30:00")); // missing sign
	} catch (const TimedeltaError&) {
		threw = true;
	}
	assert(threw);

	// A TimeError is also catchable through std::exception / std::runtime_error,
	// confirming the reparenting to std::runtime_error.
	threw = false;
	try {
		Datetime::TimeParser(std::string_view("bogus"));
	} catch (const std::runtime_error&) {
		threw = true;
	}
	assert(threw);

	std::printf("datetime throwing OK: DatetimeError / TimeError / TimedeltaError raised, catchable as std::runtime_error\n");
}


// ---------------------------------------------------------------------------
// Validity predicates.
// ---------------------------------------------------------------------------
static void test_predicates() {
	assert(Datetime::isDate("2021-03-14"));
	assert(!Datetime::isDate("2021-13-14"));
	assert(Datetime::isDatetime("2021-03-14T15:09:26Z"));
	assert(Datetime::isTime("15:09:26"));
	assert(!Datetime::isTime("15:09:26x"));
	assert(Datetime::isTimedelta("+01:30:00"));
	assert(!Datetime::isTimedelta("01:30:00"));
	assert(Datetime::isleapYear(2020));
	assert(!Datetime::isleapYear(2021));
	assert(Datetime::getDays_month(2020, 2) == 29);

	std::printf("datetime predicates OK: isDate/isDatetime/isTime/isTimedelta/isleapYear\n");
}


int main() {
	test_iso8601_parse();
	test_date_math();
	test_timestamp_roundtrip();
	test_time_parser();
	test_timedelta_parser();
	test_throwing();
	test_predicates();
	std::printf("all datetime tests passed\n");
	return 0;
}
