# Source layout

The library is one STATIC target, `datetime`, built from the files below. `datetime.h` is the only
public header; users include it and nothing else.

## The library

| File | Holds |
|---|---|
| `datetime.h` | The `Datetime` namespace API: `tm_t`, `clk_t`, `Format`, the parsers and formatters, the `DatetimeError` hierarchy. |
| `datetime.cc` | Datetime parsing: the field readers, the ISO 8601 scanner (`iso8601_scan<Fill>`), the loose-format scanner (`scan_date_re`), `DatetimeParser` and `Iso8601Parser`, `isDate` and `isDatetime`. |
| `datetime_math.cc` | Date math: `processDateMath`, `computeTimeZone`, `computeDateMath` and the 64-bit term arithmetic (`add_term`). |
| `datetime_calendar.cc` | Calendar arithmetic: leap years, month lengths, `normalizeMonths`, `toordinal`, `timegm`, `to_tm_t`, `timestamp`, `isValidDate`. |
| `datetime_format.cc` | ISO 8601 formatting: the integer writer, the `std::format` fallback and the four `iso8601` overloads. |
| `datetime_time.cc` | Time and timedelta: `TimeParser`, `TimedeltaParser`, the `clk_t` and double conversions, the `*_to_string` formatters and the predicates. |
| `datetime_detail.h` | Internal, not for users: the civil-calendar core (`days_from_civil`, `civil_from_secs`, `apply_tz`, ...), the grammar's widths and the fraction parsers that several pieces share. |

The helpers in `datetime_detail.h` are the `static` functions they were in the single file; each
piece that includes the header gets its own inlinable copy. They are not in a `detail` namespace
because that would rename every call site.

## Tests

| File | Holds |
|---|---|
| `test/test.cc` | Smoke test of every entry point. |
| `test/datemath_test.cc` | Years 1 to 9999 and huge date-math terms against independent references. |
| `test/diff_test.cc` | Differential test against the frozen reference in `test/old/`. |

## Kept whole

`test/diff_test.cc` (1,182 lines) is one differential run, and its pieces cannot be read apart:

1. One seeded random stream feeds every test in a fixed order, and the run's digest of those inputs
   is compared with a recorded constant, so cutting the file changes what the digest proves only if
   the order stays; the order is the file's `main`.
2. The tests share the stream, the check and failure counts, the class tallies and the digest as
   file-scope state. Separate translation units would turn that state into `extern` globals.
3. The classes where the library differs from the reference, with their oracles, are described once
   at the head of the file and apply to every test below it.

Tests by topic would repeat that header and the state in a support file, and each piece would
still need the others to mean anything.

`test/old/datetime_old.cc` is the frozen reference and is never edited.
