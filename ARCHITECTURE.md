# ARCHITECTURE

The internal design of the `datetime` library.

## What this library is

A single compiled translation unit (`datetime.cc`) behind one header
(`datetime.h`), all in the `Datetime` namespace. It turns date/time *strings and
numbers* into structured values and back. Three input vocabularies:

- **Datetime**: ISO 8601, optionally suffixed by `|| <date math>`. Returns
  `tm_t` (year, mon, day, hour, min, sec, fractional sec, `is_utc`).
- **Time**: `HH:MM(:SS(.ffff)([+-]HH:MM))`. Returns `clk_t`.
- **Timedelta**: `[+-]HH:MM(:SS(.ffff))`. Returns `clk_t`.

plus the conversions between those and `time_t` / `double` (Unix seconds), and
the `iso8601(...)` / `*_to_string(...)` formatters.

## Parse paths

`DatetimeParser(string_view)` delegates to a file-local `process_date_datetime`
that does the work in two stages:

1. **Fast ISO 8601 path** (`Iso8601Parser`). A hand-written, allocation-free
   scanner keyed on the string length (10 = date, 19 = date+time, 20 = `...Z`,
   else offset/fractional). It returns a `Format` enum (`VALID` / `INVALID` /
   `OUT_OF_RANGE` / `ERROR`) rather than throwing, so the caller can fall through
   to the slow path on `INVALID`. There are two overloads, one function
   template (`iso8601_scan<Fill>`): one fills a `tm_t`, one only validates (used
   by `isDatetime`). Fixed-width fields are read as plain digits; any other byte
   takes an exact emulation of the `strtoul` grammar (leading whitespace, a
   sign), so odd fields such as `"+5"` or `" 5"` parse. The two overloads run
   every check (a negative or out-of-range field is `OUT_OF_RANGE`; a fraction
   dot needs 1 to 62 digits, else `ERROR`) and differ only in whether they write
   the `tm_t`, so they return the same `Format` for every input.
2. **Loose-format scanner** (`scan_date_re`). A hand scanner for the grammar of
   `date_re` (`/`, space or no separator, 1-digit hours, `,` fractions, spaced
   offsets) that also captures the `|| <date math>` suffix. Only reached when the
   fast path says `INVALID`. A loose-format string ending in `Z`, or carrying an
   offset, sets `is_utc` like the ISO path. `date_re` and `date_math_re` are
   exported for ABI/API compatibility; the library does not use them.

If the string contains `||`, the part before it is parsed as a datetime and the
part after is fed to `processDateMath`.

### Date math

`processDateMath` scans the grammar of `date_math_re`
(`([+-]\d+|/{1,2})([dyMwhms])`) contiguously and calls `computeDateMath` per term. `+N`/`-N` add or
subtract; `/unit` rounds *up* to the end of the unit (e.g. `/M` -> last day,
23:59:59.999999); `//unit` rounds *down* to the start. After each operation the
`tm_t` is renormalized by round-tripping through `timegm` and an integer
civil-from-days conversion, so overflow (e.g. day 32) carries correctly. A
`+N`/`-N` term is applied in 64-bit arithmetic (`|N| <= 2^31`, so no step can
overflow); the supported years are 1 to `INT_MAX`, and a step that leaves them
throws `DatetimeError("Year is out of range")`. Months use floor division:
`normalizeMonths` maps any month to 1..12 and carries whole years, so `+12M`
from December is December of the next year, and the day clamps to the length of
the new month.
`computeTimeZone` of `+05:30` means `-5h -30m` of date math (the wall-clock value
becomes the UTC instant); the parsers apply it as one integer sum with the same
results, including the `Year is out of range` throw when the first step leaves
year 0. Other offsets run through `computeDateMath`.

`Time` and `Timedelta` use their own length-keyed scanners
(`TimeParser` / `TimedeltaParser`), not the regex.

### Numeric core

`days_from_civil` (an integer proleptic Gregorian day count from 1970-01-01, with an
unsigned variant for years >= 1 that needs no sign handling in its divisions) and
`civil_from_days` are the only calendar arithmetic. `toordinal` validates its
arguments (year >= 1, then the day below 1, the month, the day against the month
length) and adds the ordinal of 1970-01-01; `timegm` and `timestamp(tm_t)` normalize
the month with `normalizeMonths`, require year >= 1, and sum days, hours, minutes
and seconds in 64-bit integers. `timestamp` adds the fraction to those whole seconds,
before 1970 as after, so instants rise with the clock. `to_tm_t(time_t)` and
`to_tm_t(double)` go the other way with floor semantics (the fraction of a timestamp
is its distance above the whole second) and no libc time-zone state. Leap-year and
days-in-month logic lives in `isleapYear` / `getDays_month` with a static `days`
table.

## Relation to Xapiand

The library is the *pure half* of Xapiand's `src/datetime.{h,cc}`. It differs
from that file in these ways:

- **Exceptions.** `DatetimeError` derives from `std::runtime_error` (Xapiand's
  derives from `ClientError`, a located exception), and `DateISOError` /
  `TimeError` / `TimedeltaError` derive from `DatetimeError`. The names and the
  message-string constructors match Xapiand's, so existing
  `catch (const DatetimeError&)` (and subclass) sites work. There is no forward
  `class MsgPack;` and no `#include "exception.h"`.
- **`throw X(std::format("fmt", a))`.** The located-exception `THROW` macro needs
  a `BaseException`-shaped constructor (`__func__, __FILE__, __LINE__, ...`) that
  a `std::runtime_error` subclass does not have, so the throws are spelled out.
- **`std::format`** (`<format>`) replaces `strings::format` and `strings.hh`.
- **Quoting.** Two date-math error messages quote the offending token with plain
  `'{}'` in the `std::format` string, where Xapiand used `repr`. Each passes the
  unit and the token.
- **No logging.** There is no `log.h`.
- **No MsgPack path.** The `process_date_year/_month/.../_datetime` static
  helpers and the `DatetimeParser(const MsgPack&)` /
  `time_to_double(const MsgPack&)` / `timedelta_to_double(const MsgPack&)`
  overloads are the only users of MsgPack, the `RESERVED_*` tokens, and
  `phf::make_phf` / `hh(...)`. They live in Xapiand-side glue that delegates back
  to this library's string/double parsers.

## Dependencies

- **strict-stox** (header-only, FetchContent). Fractions longer than 6 digits
  and the time/timedelta parsers use `strict_stod` / `strict_stoul`; the
  datetime date/time fields are read directly. The widths of the grammar (a fraction's 1 to
  62 digits, a date-math number's 63 characters) are checked by datetime's own scanners
  before a field reaches strict-stox, so its width limit does not show. This is the library's only third-party dependency.
- **No hashes / phf.** The perfect-hash table is MsgPack-key dispatch in the
  glue half. The pure core does not hash anything, so `hashes` is *not* a
  dependency here (unlike some sibling libraries).
- Standard library: `<regex>` (only for the exported `date_re` objects), `<format>`, `<chrono>`, `<ctime>`.

## Trade-offs

- The ISO 8601 scanner is one template for both overloads, so they cannot drift
  apart; `test/diff_test.cc` compares every entry point against the frozen
  reference (`test/old/`) on random, mutated and exhaustively byte-mutated
  inputs, and asserts each input class where the result differs against an
  independent oracle. The loose-format scanner is the catch-all for the less
  common separator styles and the date-math suffix.
- `timestamp` / `timegm` are only correct for year > 0 (proleptic Gregorian from
  year 1). That is the documented domain; `isValidDate` rejects year < 1.
- The library assumes a 64-bit `time_t` for the full datetime range.
