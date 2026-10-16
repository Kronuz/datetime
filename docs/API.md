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

This library is the **pure string/number core**. The original Xapiand code also
parsed dates given as a MsgPack structured object (`{year: 2021, month: 3, ...}`).
That path depends on MsgPack, Xapiand's reserved-key tokens, and a perfect-hash
table, so it stays in Xapiand as a thin glue layer that extracts the value and
delegates to this library's string/double parsers. There is no `now` keyword
here either; relative-to-now resolution is a caller concern.

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

Bad input throws `DatetimeError` (and its `DateISOError` / `TimeError` /
`TimedeltaError` subclasses), all derived from `std::runtime_error`.

The only third-party dependency is the header-only
[strict-stox](https://github.com/Kronuz/strict-stox) library for strict numeric
parsing; everything else is the C++ standard library (`<regex>`, `<format>`,
`<chrono>`).
