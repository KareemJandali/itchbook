# Validation records

One JSON per reconstruction of a real trading day: what the book produced, kept
so a later change can be checked against it.

| Symbol | Date | Messages | Volume | Open | High | Low | Close | Graded? |
|---|---|---|---|---|---|---|---|---|
| MSFT | 2019-12-30 | 1,220,796 | 6,154,278 | 159.2000 | 159.3000 | 156.7300 | 157.5700 | **PASS** |

Graded against Databento `XNAS.ITCH` `ohlcv-1d`. All five fields match exactly.

The message count is `messages_read` from the JSON beside this file — the
`--utc-day`-bounded run, which is the one that was graded. Read the next
section before comparing it to any other number in this repository: the same
day yields two different counts depending on where the session is cut, and
both appear in the README.

Source: `12302019.NASDAQ_ITCH50.gz` from `emi.nasdaq.com/ITCH/Nasdaq ITCH/`,
sliced with `python/slice_symbol.py`.

The C++ book and the Python oracle agree on all of it — 61,228 identical
snapshot rows at a one-second interval, identical summaries, zero unknown order
references across 1.22M messages. That is phase 3's done-condition.

Those 61,228 rows are the **unwindowed** run over the whole file, which is what
`full-day-differential.sh` compares and what the README quotes. The JSON beside
this file is the **windowed** run — `--utc-day 2019-12-30`, 1,220,796 messages
and 57,291 rows — because that is the window the Databento bar covers. Same
book, same agreement, different cut of the session; the next section is why the
cut exists.

## The session window

Reproduce with:

```bash
python3 python/reference/replay.py data/sliced/MSFT.gz \
    --utc-day 2019-12-30 --json validation/MSFT_2019-12-30.json
export DATABENTO_API_KEY=db-...
python3 python/analysis/validate.py validation/MSFT_2019-12-30.json \
    --symbol MSFT --date 2019-12-30
```

`--utc-day` is not optional, and the first grading run is why.

ITCH timestamps are nanoseconds since midnight **Eastern**. Databento buckets
`ohlcv-1d` by **UTC** day, which rolls over at 19:00 ET in winter and 20:00 ET
in summer. Replaying the whole ITCH file therefore includes after-hours trades
that belong to the *next* UTC bar: 519 shares in this case, which showed up as a
volume excess of 519 and a close of 157.63 against their 157.57.

That is two different questions being asked, not a parser bug. Bounding the
replay to the same window makes all five fields match exactly. `validate.py`
now refuses to grade a reconstruction whose window does not match the oracle's,
rather than reporting a confusing FAIL.
