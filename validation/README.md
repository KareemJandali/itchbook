# Validation records

One JSON per reconstruction of a real trading day: what the book produced, kept
so a later change can be checked against it.

| Symbol | Date | Messages | Volume | Open | High | Low | Close | Graded? |
|---|---|---|---|---|---|---|---|---|
| MSFT | 2019-12-30 | 1,220,796 | 6,154,278 | 159.2000 | 159.3000 | 156.7300 | 157.5700 | **PASS** |

Graded against Databento `XNAS.ITCH` `ohlcv-1d`. All five fields match exactly.
The message count is the record's own `messages_read` — the `--utc-day` window,
not the whole file. The JSON's `symbol` is `null` because the reconstruction
that produced it was run on a pre-sliced file without `--symbol`; the filename
carries it, and the command below now passes the flag so a re-run records it.

Source: `12302019.NASDAQ_ITCH50.gz` from `emi.nasdaq.com/ITCH/Nasdaq ITCH/`,
sliced with `python/slice_symbol.py`.

The C++ book and the Python oracle agree on all of it — 61,228 identical
snapshot rows at a one-second interval, identical summaries, zero unknown order
references. That is phase 3's done-condition.

Two windows are in play and their figures differ, which is expected rather than
a discrepancy:

| | messages | snapshot rows at 1 s |
|---|---:|---:|
| whole file — the differential above | 1,221,484 | 61,228 |
| `--utc-day 2019-12-30` — the graded record | 1,220,796 | 57,291 |

The 688-message difference is the after-hours tail described below. The
differential runs unbounded on both sides because it asks whether two
implementations agree, not whether either matches a vendor's bucket.

## The session window

Reproduce with:

```bash
python3 python/reference/replay.py data/sliced/MSFT.gz --symbol MSFT \
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


## The framing, checked against a whole day of every symbol

Everything else on this page is about one symbol. This is about the wire format
itself, and it is the only check here that needs the *full* file rather than a
slice — a single-symbol slice contains that symbol's messages and the system
events, so it can never exercise a type the symbol did not produce.

```
./build-release/itch_census 12302019.NASDAQ_ITCH50.gz
```

**268,744,780 messages. 8.25 GB. No length mismatch.**

| | |
|---|---:|
| distinct message types present | 18 |
| modelled (the book interprets them) | 12 |
| framed and length-checked, not interpreted | 6 |
| messages not modelled | 4,248,527 — 1.58% |

The parser throws on a prefix that disagrees with the type's spec length, so
completing at all means every one of those 268M frames agreed with the table in
`messages.hpp`. That is the strongest evidence in the repository that the
framing is right, and it is cheap: one pass, no oracle, no subscription.

It also settles a question the synthetic feeds could not. Ten spec lengths were
added for types the book does not model, so that a desync landing inside one is
caught at that message rather than at the next one the book cares about. No
generator in this repository emits them, so CI cannot reach them, and a wrong
constant would have sat there until someone ran a real file. This day exercised
six of the ten — `I` (4,024,315 of them), `L`, `Y`, `J`, `K` and `V` — and all
six framed correctly.

`V` is the interesting one. It appears **exactly once**, at 35 bytes, which is
the MWCB Decline Level published at the start of the session. Had `V` and `W`
been transposed — the obvious mistake, since both are circuit-breaker messages
— that single message would have been checked against 12 bytes and thrown
immediately. One occurrence in 268 million was enough.

**Still unexercised: `W`, `h`, `B`, `N`, `O`.** Nothing on this day breached a
circuit-breaker level, operationally halted a symbol, busted a trade, or
published retail price improvement, and `O` postdates the file. Those five
constants remain read from the spec and unconfirmed against real bytes. A day
containing a halt would settle `h` and `W`; that is the argument for running
this against a second, more eventful day.

## Which oracle, and why not the one the plan named

The build plan's done-condition says match "NASDAQ's published daily summary
for that date — or match LOBSTER's published orderbook file". This project
matched **Databento's XNAS.ITCH daily bar** instead. That was a substitution,
and it is worth stating plainly rather than being caught on.

The reason is that the obvious NASDAQ sources do not answer the question:

* A **consolidated** daily bar — what almost every public source publishes — is
  every venue at once. ITCH is one venue. MSFT traded roughly 20M shares that
  day across the market and **6.15M of them on NASDAQ**. Comparing the two
  fails for an entirely correct reason, and passing would mean something was
  wrong.
* **NasdaqTrader's per-symbol matched volume** is venue-specific and would be
  exactly right. The site keeps a rolling window and will not go back to 2019.
* **LOBSTER's** free samples are 2012-06-21 for five tickers, and NASDAQ's free
  ITCH sample days do not include that date. Checking against it means buying
  data to confirm something already confirmed.

Databento is venue-specific (`XNAS.ITCH`), which is the property that matters,
and is arguably a stronger oracle than a summary line because it is a full
independent reconstruction rather than an aggregate. But it is not a source the
plan named, and the plan named those sources for a reason: they are free and
anyone can check them.

## The check anyone can run

One NASDAQ-published figure survives all of the above, needs no subscription,
and is checkable in a minute: the **official opening and closing prices**. Both
are auctions, both arrive in the feed as `Q` cross trades, and for a
NASDAQ-listed stock the official closing price *is* the closing cross.

Two minutes, start to finish:

1. Open `www.nasdaq.com/market-activity/stocks/msft/historical`. Set a custom
   date range covering **30 December 2019** — the picker goes back ten years,
   unlike NasdaqTrader's, which is why this route works where the volume one
   does not. Or hit *Download historical data* for the CSV.
2. Read `Open` and `Close/Last` off that row.
3. Run:

```bash
python3 python/analysis/check_cross.py validation/MSFT_2019-12-30.json \
    --official-open <open> --official-close <close> \
    --source "nasdaq.com historical quotes, MSFT 2019-12-30"
```

Read those two numbers off the page yourself. Do not take them from a chat
transcript, a model, or this file — the whole value of the check is that the
figure came from somewhere that is not this project, and supplying it from
memory and then agreeing with it is not validation, it is a tautology with
extra steps.

Expect the **close** to be the strong signal. For a NASDAQ-listed security the
official closing price *is* the closing cross, so a match verifies the `C`
reconstruction outright. The **open** is weaker: some sources publish the
opening cross and others the first consolidated print of the day, which are
different numbers. If the close matches and the open does not, find out which
definition that page uses before concluding the reconstruction is wrong.

This is a real test rather than a formality. Cross handling is the part of an
ITCH book most likely to be quietly wrong — it is rare, it is a separate
message type, and it never appears in a day's ordinary flow, so nothing else
exercises it. Note it is a different number from the summary's `close`, which
is the last trade of the session including late prints.

**Status: the closing cross is verified. The opening cross is not, and cannot
be from this source.**

```
auction                 ours   published  verdict
----------------------------------------------------
official open       158.9900    158.9870  NOT AN AUCTION PRICE
official close      157.5900      157.59  match
----------------------------------------------------
PASS — 1 auction price(s) match nasdaq.com historical quotes CSV, MSFT 2019-12-30
```

Source: nasdaq.com's own historical-quotes CSV (MAX range, downloaded), row
`12/30/2019,$157.59,16356720,$158.987,$159.02,$156.73`.

**The close matches to the cent, from NASDAQ's own website.** For a
NASDAQ-listed security the official closing price *is* the closing cross, so
this is the auction print itself and not a proxy for it — and cross handling is
the part of an ITCH book most likely to be quietly wrong, because it is rare, it
is a separate message type, and nothing in a day's ordinary flow exercises it.
That is now checked against a figure anyone can download for free, which is
what the Databento comparison above is not.

**The open is a different quantity, and finding that out was the point.** That
CSV gives $158.987 — a sub-penny price. NASDAQ's crosses clear at a single
price built from orders that are themselves priced in pennies, since Reg NMS
Rule 612 forbids sub-penny quoting at or above $1.00, so an auction cannot
print at $158.987. Whatever that column is — most likely the first consolidated
print of the session, which *can* be sub-penny — it is not the opening cross,
and it cannot check ours.

The first version of this check reported it as a MATCH. It compared at two
decimals, rounded $158.987 to $158.99, and agreed with our $158.9900. That is a
check passing by discarding the precision that would have shown its two inputs
were not the same kind of number. `check_cross.py` now refuses a figure that is
not on a penny increment and says why, rather than rounding it into agreement.

The rest of the day's OHLC corroborates the reading. That CSV's low, $156.73,
equals ours exactly; its high, $159.02, is below our $159.30, and its volume,
16,356,720, is 2.7x our 6,154,278 — which is what a regular-session
consolidated figure should look like against a full-session NASDAQ-only one.
Same day, different windows and different venues, agreeing where they overlap.
