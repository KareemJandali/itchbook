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

## Sizing phase 9, out of the same pass

Phase 9 puts every symbol in one process, which means one shared reference map
and one shared pool. Both have to be sized from something, and the only honest
something is a count of the real file. Two opt-in flags do it in the same pass
that checks the framing:

```bash
time ./build-release/itch_census 12302019.NASDAQ_ITCH50.gz \
    --peak-orders --per-symbol validation/census-2019-12-30.json
ls -l 12302019.NASDAQ_ITCH50.gz            # the compressed size, next to the 8.25 GB
```

`--peak-orders` reports the high-water mark of orders resting simultaneously
across every symbol. It keeps twelve bytes per live order rather than the book's
forty — no levels, no pool — because the question is how many, not where. The
structure also reports whether `inserted == live + removed + emptied`, which is
an identity rather than an estimate: if it does not hold, the count is worthless
and says so.

`--per-symbol` writes one record per locate: message counts by kind, the range
of prices that symbol actually **quoted** (trade prints excluded, because the
dense band has to cover where orders rest and a cross prints outside it),
opening and closing cross counts, and the two message types that can make a
vendor's daily bar legitimately disagree with ours — `B` broken trades and `h`
operational halts.

`time` is not decoration. This pass decompresses, frames and length-checks the
whole file while building nothing, so its wall clock is the **floor** for any
all-symbols replay: no end-to-end number phase 9 reports can be below it, and
the gap between the two is the book's own cost. It is the cheapest measurement
in the project and it was skipped the first time this ran.

The table below is **generated** from `validation/census-2019-12-30.json` by
`scripts/census-report.py`, and CI runs that script with `--check` on every push.
Nothing here was typed off a terminal.

<!-- census:begin -->

| | |
|---|---:|
| messages | 268,744,780 |
| uncompressed | 8.25 GB |
| on disk (gzip) | 3.52 GB — a 2.34x ratio |
| locates seen | 8,907 |
| with a stock directory entry | 8,906 |
| that ever quoted an order | 8,892 |
| that ever printed a trade | 7,196 |
| with a closing cross | 8,906 |

**Live orders, one shared reference space across every symbol.**

| | |
|---|---:|
| peak resting at once | **1,924,078** |
| resting at the close | 0 |
| adds (`A`+`F`) | 118,631,456 |
| replaces (`U`) | 21,639,067 |
| deletes (`D`) | 114,360,997 |
| executions that emptied an order | 4,270,459 |
| executions that did not | 1,552,282 |
| cancels that emptied an order | 0 |
| cancels that did not | 2,787,676 |
| **references naming no live order** | **0** |

The pass took **65.94 s** — 125 MB/s of feed, 4.08 M msg/s — and it was the `framing + live-order tracking` pass.

**Stub quotes, and why a symbol's quoted range sizes nothing.**

Of the 8,892 symbols that quoted at all, **6,896 (77.6%) posted an order at or above $100,000**, and **7,131 (80.2%) posted one at or below $0.01**. Those are orders parked where they will never fill, satisfying a two-sided quoting obligation. Nothing on the wire marks them; what identifies them is a price nothing could trade at.

The consequence is concrete: the range of prices a symbol *quoted* spans almost the whole price axis for three symbols in four, so it cannot centre a dense band or predict which symbols will overflow one. The range it *printed* can, which is why the census records both — and no generated feed in this repository produces a stub quote, so nothing here would ever have shown it.

**1 locate with no directory entry:** `0` (10 messages). That is the session itself — `S` system events and the market-wide `V` carry stock locate 0, and no `R` describes it. A message for an undirectoried locate is counted rather than ignored, because the benign explanation and a framing bug look identical until someone looks.


**The floor.** A pass that decompresses, frames and length-checks the file while building nothing:

| pass | seconds | MB/s | M msg/s |
|---|---:|---:|---:|
| `framing only` | **16.51** | 500 | 16.28 |
| `framing + live-order tracking` | 65.94 | 125 | 4.08 |

No replay of this file can beat 16.51 s, and the difference between that and any end-to-end number is what the book costs. The second row prices the live-order table on its own: **49.4 s** for roughly 263 M hash operations against a table far larger than L2 — which is the same shape of work the book's reference map does, and the reason the multi-symbol throughput prediction in the build plan is what it is.
<!-- census:end -->


Also checked on every push, on generated data: the census and the book agree.
Two implementations that share no code count the live orders in the same feed
and must return the same number.

## The regression baseline, and why it is generated

`validation/regression/` holds one feed's reconstruction, frozen: the snapshot
CSV, the summary JSON, and the same feed through `--all-symbols`. CI regenerates
and diffs them on every push.

```bash
./scripts/regression-gate.sh            # check
./scripts/regression-gate.sh --update   # re-record, deliberately
```

It exists because phase 9 rewrote the inside of the book — storage moved out
from under it, every order gained a field, messages now arrive through a router
— and none of that is supposed to change what a reconstruction produces.
"Supposed to" is not a standard this project accepts, so the claim is checked
rather than asserted.

The baseline is a **generated** feed with a fixed seed, not MSFT, for the
obvious reason: licensed market data cannot live in a repository. That is a real
weakness and worth naming — a generated feed does not contain the paths a real
day contains, which is the entire argument of
[`what-synthetic-data-hides.md`](../docs/writing/what-synthetic-data-hides.md).
What this gate catches is *drift*: a change that alters output the author did
not intend to alter. What it cannot catch is a reconstruction that was already
wrong, which is what the oracle differential and the Databento comparison above
are for. Three checks, three different questions.

The gate hashes the feed before it compares anything else. A generator whose
output moved and a book whose output moved fail in the same place and mean
opposite things, so it says which — the difference between a five-minute fix and
an afternoon.

**The real-day equivalent belongs here too, and cannot be committed.** With
`12302019.NASDAQ_ITCH50.gz` on the machine:

```bash
./build-release/book_replay data/sliced/MSFT.gz --symbol MSFT \
    --snapshots msft-snapshots.csv --interval-ms 1000 --json msft-summary.json
```

Keep those two files somewhere outside the repository and diff them after any
change to the book. The committed `MSFT_2019-12-30.json` above already serves
part of this purpose: it is a recorded reconstruction, and a later change that
alters it will show up when it is re-graded.

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
