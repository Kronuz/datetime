# datetime

A small C++20 **date/time parser and formatter** extracted from
[Xapiand](https://github.com/Kronuz/Xapiand). It parses ISO 8601 timestamps,
Elasticsearch-style date math (`||+1M-1d/d`), standalone times and time deltas,
and formats them back out.

## What it is

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

## Scope

This library is the **pure string/number core**. The original Xapiand code also
parsed dates given as a MsgPack structured object (`{year: 2021, month: 3, ...}`).
That path depends on MsgPack, Xapiand's reserved-key tokens, and a perfect-hash
table, so it stays in Xapiand as a thin glue layer that extracts the value and
delegates to this library's string/double parsers. There is no `now` keyword
here either; relative-to-now resolution is a caller concern.

## Install

CMake with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  datetime
  GIT_REPOSITORY https://github.com/Kronuz/datetime.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(datetime)

target_link_libraries(your_target PRIVATE datetime::datetime)
```

`datetime` is a compiled `STATIC` library; it pulls in `strict-stox` via
`FetchContent` and puts both `datetime.h` and `strict_stox.hh` on your include
path. The header keeps its original Xapiand name, so a codebase that already
`#include "datetime.h"` just needs this repo on its include path.

Requires C++20. On macOS it builds with AppleClang/libc++, the same toolchain
Xapiand uses (`std::format` and `std::regex` from libc++).

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

## Build & test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The test covers ISO 8601 parses (fractional `Z`, date-only, offsets), date math
(`+1M-1d/d`, `/M`, `//M`, `//d`), the `timestamp` <-> `to_tm_t` round-trip,
`iso8601` formatting, `TimeParser` / `TimedeltaParser` round-trips through their
double representations, the throwing error paths, and the validity predicates. It
prints `all datetime tests passed` and exits 0.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand). The standalone delta
is pure decoupling: the exception hierarchy was reparented from Xapiand's
`ClientError` to `std::runtime_error` (same names, same message constructors);
`THROW(...)` macros became `throw DatetimeError(std::format(...))`;
`strings::format` became `std::format`; `repr(...)` quoting was inlined into the
error messages; and the MsgPack / reserved-token / perfect-hash structured-object
path was lifted out into Xapiand-side glue. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the design and [AGENTS.md](AGENTS.md) for the repo map and invariants.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
