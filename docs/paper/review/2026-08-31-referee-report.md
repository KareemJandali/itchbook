# Review of "Avellaneda-Stoikov on NASDAQ TotalView-ITCH, with fill-model uncertainty bands"

Reviewed at commit 5ee2b7b, August 31, 2026.

A disclosure belongs first. I helped sketch the experimental design that became section 11.3 of the build plan, so my reading is partly informed. I wrote none of the code and none of the paper, and no results had reached me before this review. Everything below was checked from a fresh clone.

My verdict is major revision, and I expect the findings to survive it. Most of what follows is disclosure debt. One comment concerns a real methodological problem, and one of them strengthens your result. The honesty machinery in this repository, by which I mean the generated verdicts and the drift checks, together with refusals stamped into the artifacts, is better than most published empirical microstructure work. That is also why I hold the paper to its own standard.

## What I checked myself

Your own gates were run before I read the text.

- paper-report.py with the check flag passes. The committed tables match the committed artifacts.
- paper-figures.sh with the check flag passes. The figures match the manifest.
- The git record supports the preregistration claim. The commit carrying the prediction bars (4c06ab7, Aug 22 at 00:35) precedes the commit carrying results (5690965, same day at 17:32) by about 17 hours. The gap is measured in hours, and the ordering is what you claimed.
- I recomputed the headline from validation/as-experiment.json so that the tables were not doing the work for me. At every swept gamma, A-S captures less edge than the touch maker in 36 of 36 cells. Depending on gamma, the count of negative cells runs from 14 to 21. My recount reproduces your gamma 0.02 table exactly.

## Major comments

**1. The calibration is circular and the paper never says so.** calibrate_intensity generates its exposure by running the A-S maker itself, parameterised with a placeholder k. Where your orders rested therefore depends on a guessed parameter, and so does the question of which depth buckets accumulated exposure. The k measured under guessed-k quoting may differ from the k you would measure under measured-k quoting. That is a fixed point iteration, you stopped after one step, and section 6 offers neither a convergence check nor any indication that an iteration exists.

A second problem sits in the same place. Depth is integrated as the mid moves, which you do correctly, but the consequence is that most of your exposure at large depth accumulates in moments after the mid has moved away from a stale quote. Such moments are adversely selected. A-S consumes lambda of delta as the fill rate for an order placed fresh at that depth. Two different conditional distributions are involved here, and I see no reason to assume the bias is neutral.

The first part has a cheap and decisive fix. Recalibrate once under the measured parameters and report how far k moves. Should it barely move, section 6 gains a sentence and your position improves. Should it move a great deal, that is something you needed to know more than any reviewer did. For the second part, state the mismatch in section 6 at minimum. Better still, split the estimate into exposure from orders placed at a depth and exposure from orders that drifted there, then show whether the two agree.

**2. The preregistration promised an ETF and the paper quietly dropped it.** Section 11.3 of the plan specifies MSFT, a mid cap and an ETF. You evaluated GOOG, MSFT and STOR. No intensity artifact exists for QQQ, so the ETF was never calibrated, yet the status block reports that the preregistered scope was met. It was met on the counts, and the composition tells a different story, which the paper never mentions. The consequence is larger than it first appears. By the mechanism you set out in section 8.3, a penny-wide ETF with a deep queue is precisely the large-k case where the floor lands inside the tick. So the preregistered symbol you skipped is the strongest available test of your central claim. AMD's exclusion was disclosed properly in 8.4. Treat QQQ the same way: calibrate it and either evaluate it or place it on the refusal list with a reason, then amend the status block.

**3. Max drawdown was promised and never delivered.** Plan item 11.3.5 commits to markouts at three horizons, an inventory path plot, and a reported max drawdown. Three markouts exist in the artifact, and only the one-second horizon reaches any table. The inventory path is absent, because the gamma inventory figure is a sweep. No drawdown field appears anywhere, and the artifact schema does not define one either. A paper that makes a point of transcription fidelity in 7.5 cannot lose line items from the same preregistration two clauses later in silence. Add them, or add a short section that lists deviations from the preregistered design together with reasons. Such a list would be perfectly acceptable. Saying nothing is the problem.

**4. P4 is graded kept on thin evidence.** The latency sweep consists of two points, 0 and 500,000 ns, against a plan that pointed at your own latency_sweep tool, which runs a proper ladder elsewhere in this repository. The verdict rests on 19 of 36 cells. That is 52.8 percent, one cell away from falsified, and the cells are correlated within a symbol day. Your edge-based check, which gives 25 of 36 overall and 4 of 12 on STOR, carries more weight and is honestly labelled as not preregistered. Run the ladder and regrade, or annotate the kept verdict as a bare majority over correlated cells. As printed, the single word kept overstates.

**5. The 36 of 36 language overstates the sample.** You never pool, and you claim no significance, which is correct. Four lanes of one symbol day nevertheless amount to four scorings of almost the same data, and on GOOG three of them agree to the last digit. Nine symbol days is the honest replication unit, and an argument can be made for three symbols. Wherever a count out of 36 appears in the abstract or the conclusion, add one sentence stating that lanes within a symbol day are not independent. Your result survives an honest count, so count it honestly.

## Minor comments

1. The generated calibration block prints a meaningless statement for AMD. It reports that k spans 483.9 to 483.9, a factor of 1.00, and then delivers the cross-lane cost sentence, although only one lane fitted. Guard that template on at least two fitted lanes.
2. STOR's fits are weak, with R squared between 0.64 and 0.77 on roughly 500 to 600 fills, and STOR is also the single symbol where P4 fails on edge. A sentence connecting those two facts is warranted. Worth noting as well: the headline survives dropping STOR without difficulty, since by my recount it holds 24 of 24 on GOOG and MSFT alone. You could say so.
3. GOOG's optimistic, mbo and pessimistic lanes agree to four decimals. My assumption is that on a book sixty ticks wide your order usually sits alone at its price level, so front of queue and back of queue coincide. Put that in one sentence, because the printed table reads like a copy bug.
4. P3 states that A-S loses money after fees. Whether the fee model includes the maker rebate is never addressed. Equity numbers this small can change sign on that alone. State the schedule.
5. The tables fix gamma at 0.02 and give no reason. Use my recount here. Since the 36 of 36 result holds at all four swept gammas, one sentence converts what currently reads as a chosen setting into a robustness result.
6. No table of the parameter values actually used appears anywhere. I mean the sigma window and warmup, the inventory unit, the requote threshold and the inventory cap. Quote size and the gamma grid are stated. The remainder has to be recovered from artifacts.
7. Four months separate the calibration day from the evaluation days, and the stability of k across that gap is untested. A refit on one evaluation day, labelled post hoc, would bound the transfer error cheaply.
8. P5 is already flagged by you as the weakest row in the table, and I agree. It should not be cited outside 7.5 until phase 6 is rerun on a feed that this paper also evaluates.

## Questions for the author

1. Confirm from the artifact that the GOOG lane coincidence follows from queue degeneracy and not from a copy error.
2. In closed_loop.hpp, does the strategy see its fill before or after it sees the book update from the event that caused the fill? State the ordering in section 4. An error in that ordering is the classic closed loop lookahead.
3. Which gamma and k did the calibration bootstrap run with, and are they recorded in the intensity artifacts? If they are absent, a committed artifact carries unrecorded inputs, which breaks your own provenance rule.

## Closing

The paper rests on a sound principle. Measure what everyone assumes, admit what the measurement is conditioned on, and grade your own predictions in public. Section 8.3 is a genuinely good observation, and it exists because you measured k. Nothing above threatens the direction of the findings, and the calibration point is the only one that could move numbers. This project claims that nothing drifts silently, and I found three silent drifts: a dropped ETF, a dropped drawdown, and a two-point comparison described as a sweep. Fix them the way the rest of the repository fixes things, in print, with every deviation named, and I will recommend acceptance.
