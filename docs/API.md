# datetime API reference

## Usage

```cpp
#include "datetime.h"

auto tm = Datetime::DatetimeParser("2021-03-14T15:09:26.5Z"); // tm_t
double ts = Datetime::timestamp(tm);                          // Unix seconds
std::string s = Datetime::iso8601(tm);                        // "2021-03-14T15:09:26.5Z"

auto t = Datetime::TimeParser("15:09:26");                    // clk_t
auto d = Datetime::TimedeltaParser("+01:30:00");              // clk_t

// Date math: anchor || expression.
auto eom = Datetime::DatetimeParser("2021-02-10||/M");        // end of February
```

## Scope

This library is the **pure string/number core**. Xapiand also parses dates given
as a MsgPack structured object (`{year: 2021, month: 3, ...}`). That path
depends on MsgPack, Xapiand's reserved-key tokens, and a perfect-hash table, so
it lives in Xapiand as a thin glue layer that extracts the value and delegates
to this library's string/double parsers. There is no `now` keyword here either;
relative-to-now resolution is a caller concern.

## What it parses and formats

Two files, `datetime.h` + `datetime.cc`, in the `Datetime` namespace:

- **Datetime strings.** `DatetimeParser("2021-03-14T15:09:26.5Z")` returns a
  `tm_t`. It accepts date-only (`2021-03-14`), full ISO 8601 with fractional
  seconds, `Z` or `[+-]HH:MM` offsets, and an Elasticsearch-style date-math
  suffix after `||` (e.g. `2021-01-01||+1M-1d/d`). `/unit` rounds up to the end
  of the unit; `//unit` rounds down to the start. Units are `y M w d h m s`.
- **Times.** `TimeParser("15:09:26.5")` -> `clk_t`, with `time_to_double` /
  `time_to_clk_t` / `time_to_string` to convert and format.
- **Timedeltas.** `TimedeltaParser("+01:30:00")` -> `clk_t`, with the matching
  `timedelta_to_*` conversions.
- **Conversions.** `timestamp(tm_t)` (Unix seconds with a fractional part),
  `to_tm_t(time_t)` / `to_tm_t(double)`, `timegm`, `iso8601(...)` formatters, and
  the `isDate` / `isDatetime` / `isTime` / `isTimedelta` predicates.

Behavior notes:

- Both `Iso8601Parser` overloads return the same `Format` for every input. A
  negative or out-of-range hour/minute/second is `OUT_OF_RANGE`; a fraction needs
  1 to 62 digits (`...:05.` and `...:05.Z` are `ERROR`).
- A field wider than the grammar is invalid, whatever its value: a fraction has 1 to 62
  digits in a datetime, a time and a timedelta (`DatetimeError` / `TimeError` /
  `TimedeltaError`, "Error format in ..."), and the number of a date-math term has at most
  63 characters, sign excluded (`+0...010d` with 64 characters is
  `DatetimeError("Invalid format in Datetime Math unit: 'd'. '0...010' must be numeric")`).
  A date-math number outside `int` is the same error. The library checks the widths itself, so they
  do not depend on strict-stox.
- A loose-format string such as `2020/01/02 03:04:05Z` ends in `Z` = UTC and sets
  `is_utc`, like the ISO form; a loose-format offset (`...05+05:30`) is applied
  and also sets `is_utc`.
- Month arithmetic (`+N M`, `-N M`) is exact for any N: `normalizeMonths` maps a
  month to 1..12 and carries whole years in constant time; the day clamps to the
  new month's length. A carried year outside `int` throws
  `DatetimeError("Year is out of range")` and leaves the arguments unchanged.
- The fraction of a timestamp lies above its whole second: `timestamp` of
  1969-12-31T23:59:58.9 is -1.1, and `to_tm_t(-2.9)` is 23:59:57.1.
- Date-math terms are 64-bit; the supported years are 1 to `INT_MAX`. A result
  outside them, and year 1 with an offset that reaches before 0001-01-01, throw
  `DatetimeError("Year is out of range")`.

Bad input throws `DatetimeError` (and its `DateISOError` / `TimeError` /
`TimedeltaError` subclasses), all derived from `std::runtime_error`.

The only third-party dependency is the header-only
[strict-stox](https://github.com/Kronuz/strict-stox) library for strict numeric
parsing; everything else is the C++ standard library (`<regex>`, `<format>`,
`<chrono>`).
