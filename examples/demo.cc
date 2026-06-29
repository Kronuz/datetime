// A runnable tour of datetime.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/datetime_demo
//
// The library turns three vocabularies of date/time STRINGS into structured
// values and back: ISO 8601 timestamps (optionally with an Elasticsearch-style
// `|| <date math>` suffix), standalone times, and signed timedeltas. The thread
// running through the demo is parse -> structured value -> format, so each block
// prints the input it was given, what came out, and what it formats back to.
#include <cstdio>
#include <string>
#include <string_view>

#include "datetime.h"

static void rule(const char* title) {
	std::printf("\n\033[1m── %s ──\033[0m\n", title);
}

// One line: the raw input, the tm_t it parsed to, and the iso8601 of that tm_t.
// is_utc decides whether iso8601 prints a trailing Z.
static void show_dt(std::string_view in) {
	Datetime::tm_t tm = Datetime::DatetimeParser(in);
	std::printf("  %-26s -> %04d-%02d-%02d %02d:%02d:%02d fsec=%g utc=%s -> %s\n",
		std::string(in).c_str(),
		tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec, tm.fsec,
		tm.is_utc ? "yes" : "no",
		Datetime::iso8601(tm).c_str());
}

int main() {
	std::puts("datetime demo  (parse -> structured value -> format)");

	// --- 1. ISO 8601 parsing -------------------------------------------------
	rule("ISO 8601 strings parse to a tm_t");
	// Length keys the fast scanner: 10 = date, 19 = date+time, 20 = ...Z, else
	// fractional / offset. An east offset is stored as the UTC instant, so the
	// wall-clock hour shifts: 15:09 +05:30 lands at 09:39 UTC.
	show_dt("2021-03-14");
	show_dt("2021-03-14T15:09:26");
	show_dt("2021-03-14T15:09:26.5Z");
	show_dt("2021-03-14T15:09:26+05:30");

	// --- 2. date math: anchor || expression ----------------------------------
	rule("Elasticsearch-style date math after ||");
	// +N / -N add or subtract whole units (y M w d h m s); the renormalize step
	// carries overflow, so +1M-1d off Jan 1 lands on Jan 31. A single /unit
	// rounds UP to the end of that unit, a double //unit rounds DOWN to its start.
	show_dt("2021-01-01||+1M-1d");      // shift only
	show_dt("2021-01-01||+1M-1d/d");    // ...then round up to end-of-day
	show_dt("2021-01-01||+1M-1d//d");   // ...or down to start-of-day
	show_dt("2021-02-10||/M");          // end of February (leap-aware: 28 in 2021)
	show_dt("2020-02-10||/M");          // end of February in a leap year -> 29
	show_dt("2021-02-10||//M");         // start of the month

	// --- 3. timestamp <-> tm_t, and iso8601 formatting knobs -----------------
	rule("timestamp() <-> to_tm_t(), and iso8601() formatting");
	// timestamp() is Unix seconds (with a fractional part); to_tm_t() inverts it.
	Datetime::tm_t tm(2021, 3, 14, 15, 9, 26, 0.5, /*is_utc=*/true);
	double ts = Datetime::timestamp(tm);
	Datetime::tm_t back = Datetime::to_tm_t(ts);
	std::printf("  tm_t(2021-03-14 15:09:26.5Z) -> timestamp %.1f\n", ts);
	std::printf("  to_tm_t(%.1f)               -> %s\n", ts, Datetime::iso8601(back).c_str());
	// iso8601 has three knobs: the value (tm_t / double / time_point), trim of
	// trailing fractional zeros, and the date/time separator.
	std::printf("  iso8601(tm, trim=true)       -> %s\n", Datetime::iso8601(tm).c_str());
	std::printf("  iso8601(tm, trim=false)      -> %s\n", Datetime::iso8601(tm, false).c_str());
	std::printf("  iso8601(tm, trim=true, ' ')  -> %s\n", Datetime::iso8601(tm, true, ' ').c_str());
	std::printf("  iso8601(%.1f)               -> %s\n", ts, Datetime::iso8601(ts).c_str());

	// --- 4. standalone times -------------------------------------------------
	rule("TimeParser(): HH:MM(:SS(.ffff)) round-tripped through a double");
	const char* times[] = { "08:30", "15:09:26", "15:09:26.5" };
	for (const char* t : times) {
		Datetime::clk_t clk = Datetime::TimeParser(t);
		double secs = Datetime::time_to_double(clk);                  // seconds since midnight
		std::string s = Datetime::time_to_string(clk);               // trims trailing zeros
		std::printf("  %-12s -> %g s of day -> %s\n", t, secs, s.c_str());
	}

	// --- 5. signed timedeltas ------------------------------------------------
	rule("TimedeltaParser(): a signed [+-]HH:MM(:SS) duration");
	// The leading sign is required (that is what separates a timedelta from a
	// time), and it survives the round-trip through seconds.
	const char* deltas[] = { "+01:30:00", "-00:00:30", "+12:00" };
	for (const char* d : deltas) {
		Datetime::clk_t clk = Datetime::TimedeltaParser(d);
		double secs = Datetime::timedelta_to_double(clk);            // signed seconds
		std::string s = Datetime::timedelta_to_string(clk);
		std::printf("  %-12s -> %+g s -> %s\n", d, secs, s.c_str());
	}

	// --- 6. predicates and the error hierarchy -------------------------------
	rule("validity predicates, and the throwing path");
	// The is* predicates classify a string without throwing.
	std::printf("  isDate(\"2021-03-14\")     = %s\n", Datetime::isDate("2021-03-14") ? "true" : "false");
	std::printf("  isDate(\"2021-13-14\")     = %s   (month 13)\n", Datetime::isDate("2021-13-14") ? "true" : "false");
	std::printf("  isTime(\"15:09:26\")       = %s\n", Datetime::isTime("15:09:26") ? "true" : "false");
	std::printf("  isTimedelta(\"01:30:00\")  = %s   (no sign)\n", Datetime::isTimedelta("01:30:00") ? "true" : "false");
	std::printf("  isleapYear(2020)          = %s,  getDays_month(2020, 2) = %d\n",
		Datetime::isleapYear(2020) ? "true" : "false", Datetime::getDays_month(2020, 2));

	// Bad input throws a DatetimeError subclass; all derive from
	// std::runtime_error, so one catch can cover the family.
	try {
		Datetime::DatetimeParser(std::string_view("not-a-date"));
	} catch (const DatetimeError& e) {
		std::printf("  DatetimeParser(\"not-a-date\") threw DatetimeError: %s\n", e.what());
	}
	try {
		Datetime::TimeParser("99:99x");
	} catch (const std::runtime_error& e) {
		std::printf("  TimeParser(\"99:99x\") caught as std::runtime_error: %s\n", e.what());
	}

	std::puts("\ndone.");
	return 0;
}
