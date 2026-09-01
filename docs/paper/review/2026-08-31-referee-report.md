# Review of "Avellaneda-Stoikov on NASDAQ TotalView-ITCH, with fill-model uncertainty bands"

Reviewed at commit 5ee2b7b, August 31, 2026.

Disclosure first. I helped sketch the experimental design that became section 11.3 of the build plan, so I am not a fully blind reviewer. I wrote none of the code and none of the paper, and I had not seen any results before this review. Everything below was checked from a fresh clone.

Overall verdict: major revision, though I expect the findings to survive it. Most of my comments are disclosure debt, one is a real methodological problem, and one actually makes your result stronger. For what it is worth, the honesty machinery here (generated verdicts, drift checks, refusals stamped into artifacts) is better than most published empirical microstructure work. That is also exactly why I am holding the paper to its own standard.

## What I checked myself

I ran your own gates before reading the text.

- paper-report.py with the check flag passes. The committed tables match the committed artifacts.
- paper-figures.sh with the check flag passes. The figures match the manifest.
- The git record supports the preregistration claim. The commit with the prediction bars (4c06ab7, Aug 22 at 00:35) lands about 17 hours before the commit with results (5690965, same day at 17:32). Hours rather than days, but the order is what you claimed.
- I recomputed the headline from validation/as-experiment.json instead of trusting the tables. At every swept gamma, A-S captures less edge than the touch maker in 36 of 36 cells. The count of negative cells runs from 14 to 21 depending on gamma. Your gamma 0.02 table matches my recount exactly.

## Major comments

**1. The calibration is circular and the paper never says so.** calibrate_intensity generates its exposure by running the A-S maker itself, parameterised with a placeholder k. So where your orders rested, and therefore which depth buckets accumulated exposure, depends on a guessed parameter. The k you measure under guessed k quoting is not necessarily the k you would measure under measured k quoting. That is a fixed point iteration and you stopped after one step with no convergence check and no mention in section 6 that an iteration exists.

There is a second issue hiding in the same place. Depth is integrated as the mid moves, which you do correctly, but it means most of your exposure at large depth comes from moments after the mid ran away from a stale quote. Those are adversely selected moments. A-S consumes lambda of delta as the fill rate for an order placed at that depth fresh. Those are two different conditional distributions and the bias is not obviously neutral.

The fix for the first part is cheap and decisive. Recalibrate once under the measured parameters and report how much k moves. If it barely moves, section 6 gains a sentence and you got stronger. If it moves a lot, you needed to know that more than any reviewer did. For the second part, at minimum state the mismatch in section 6, and ideally split the estimate into exposure from orders placed at a depth versus orders that drifted there, and show whether the two agree.

**2. The preregistration promised an ETF and the paper quietly dropped it.** Section 11.3 of the plan says MSFT plus a mid cap plus an ETF. You evaluated GOOG, MSFT and STOR. There is no intensity artifact for QQQ at all, so the ETF was never even calibrated, and yet the status block says the preregistered scope was met. It was met on the counts and not on the composition, and the paper never mentions the difference. This matters more than it looks. By your own section 8.3 mechanism, a penny wide ETF with a deep queue is exactly the large k case where the floor lands inside the tick. The preregistered symbol you skipped is the strongest available test of your central claim. You disclosed AMD's exclusion properly in 8.4. Do the same here. Calibrate QQQ and either evaluate it or put it on the refusal list with its reason, and amend the status block either way.

**3. Max drawdown was promised and never delivered.** Plan item 11.3.5 commits to markouts at three horizons, an inventory path plot, and max drawdown reported. The three markouts exist in the artifact but only the one second horizon appears in any table. The inventory path is not plotted, since the gamma inventory figure is a sweep rather than a path. And there is no drawdown field anywhere, not even in the artifact schema. A paper that makes a point of transcription fidelity in 7.5 cannot shed line items from the same preregistration two clauses over without a word. Either add them, or add a short section listing deviations from the preregistered design with reasons. The list is fine. The silence is not.

**4. P4 is graded kept on thin evidence.** The latency sweep is two points, 0 and 500,000 ns, against a plan that pointed at your existing latency_sweep tool, which runs a proper ladder elsewhere in this repository. The verdict rests on 19 of 36 cells, which is 52.8 percent and one cell away from falsified, and the cells are correlated within a symbol day. Your own edge based check (25 of 36 overall, but 4 of 12 on STOR) is more convincing and is honestly labeled as not preregistered. Run the actual ladder and regrade, or annotate the kept verdict as a bare and correlated majority. The single word kept currently overstates.

**5. The 36 of 36 language overstates the sample.** You never pool and you claim no significance, which is right. But four lanes of one symbol day are four scorings of nearly the same world, and on GOOG three of them are identical to the last digit. The honest replication unit is nine symbol days, arguably three symbols. Add one sentence wherever a count out of 36 appears in the abstract or the conclusion, saying that lanes within a symbol day are not independent. The result survives being counted honestly, so count it honestly.

## Minor comments

1. The generated calibration block prints nonsense for AMD. It says k spans 483.9 to 483.9, a factor of 1.00, and then delivers the cross lane cost sentence, when only one lane fitted. Guard that template on at least two fitted lanes.
2. STOR's fits are weak, with R squared between 0.64 and 0.77 on roughly 500 to 600 fills, and STOR is also the one symbol where P4 fails on edge. Those two facts deserve a connecting sentence. Note also that the headline trivially survives dropping STOR, since by my recount it holds 24 of 24 on GOOG and MSFT alone. You could say that.
3. GOOG's optimistic, mbo and pessimistic lanes are identical to four decimals. I assume that is because on a book sixty ticks wide your order is usually alone at its price level, so front of queue equals back of queue. Say so in one sentence, because as printed it looks like a copy bug.
4. P3 says A-S loses money after fees. The paper never says whether the fee model includes the maker rebate. With equity numbers this small the sign can hinge on it. State the schedule.
5. The tables fix gamma at 0.02 with no stated reason. Use my recount here. The 36 of 36 result holds at all four swept gammas, so one sentence turns what currently looks like a chosen setting into a robustness result.
6. There is no table of the actual parameter values used. Sigma window and warmup, the inventory unit, the requote threshold, the inventory cap. Quote size and the gamma grid are stated, and the rest has to be dug out of artifacts.
7. The calibration day sits four months before the evaluation days and the stability of k across that gap is untested. A refit on one evaluation day, clearly labeled post hoc, would bound the transfer error cheaply.
8. You already flag P5 as the weakest row in the table. Agreed. It should not be cited outside 7.5 until phase 6 is rerun on a feed this paper also evaluates.

## Questions for the author

1. Confirm from the artifact that the GOOG lane coincidence is queue degeneracy rather than a copy error.
2. In closed_loop.hpp, does the strategy see its fill before or after it sees the book update from the event that caused the fill? State the ordering in section 4. Getting that ordering wrong is the classic closed loop lookahead.
3. What gamma and k did the calibration bootstrap actually run with, and are they recorded in the intensity artifacts? If they are not, then a committed artifact has unrecorded inputs, which breaks your own provenance rule.

## Closing

The spine of the paper is good. Measure what everyone assumes, admit what the measurement is conditioned on, and grade your own predictions in public. Section 8.3 is a genuinely nice observation and it exists only because you measured k. Nothing above threatens the direction of the findings, and only the calibration point could move numbers. But this project's brand is that nothing drifts silently, and I found three silent drifts. A dropped ETF, a dropped drawdown, and a two point comparison called a sweep. Fix them the way the rest of the repository fixes things, in print, with each deviation named, and I will sign off.
