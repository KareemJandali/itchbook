# Validation records

One JSON per reconstruction of a real trading day: what the book produced, kept
so a later change can be checked against it.

| Symbol | Date | Messages | Volume | Open | High | Low | Close | Graded? |
|---|---|---|---|---|---|---|---|---|
| MSFT | 2019-12-30 | 1,221,484 | 6,154,797 | 159.2000 | 159.3000 | 156.7300 | 157.6300 | not yet |

Source: `12302019.NASDAQ_ITCH50.gz` from `emi.nasdaq.com/ITCH/Nasdaq ITCH/`,
sliced with `python/slice_symbol.py`.

The C++ book and the Python oracle agree on all of it — 61,228 identical
snapshot rows at a one-second interval, identical summaries, zero unknown order
references across 1.22M messages. That is phase 3's done-condition.

**"Graded?" is the column that matters.** Until it says yes, the two
implementations agree with each other and nothing else. To fill it in:

```bash
export DATABENTO_API_KEY=db-...
python3 python/analysis/validate.py validation/MSFT_2019-12-30.json \
    --symbol MSFT --date 2019-12-30
```
