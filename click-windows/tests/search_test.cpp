#include "../src/Searcher.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace cw;

// Mock game: each orb needs an input of the right kind (a press, or a release when rel=true)
// at a frame in [lo,hi], otherwise you die right after frame hi.
// State (frame + which orbs were hit) is what a checkpoint stores.
// dep/depBase: this orb's window starts later (or earlier) by however much orb `dep` was hit later (earlier)
// than its recorded frame depBase. That is how one click can squeeze the next one.
struct Orb { int64_t lo, hi; bool rel = false; int dep = -1; int64_t depBase = 0; };

struct Mock {
	std::vector<Orb> orbs;
	int64_t endFrame = 0;
	int64_t frame = 0;
	std::vector<bool> hit;
	std::vector<int64_t> pf; // frame each orb was hit on
	struct Cp { int64_t frame; std::vector<bool> hit; std::vector<int64_t> pf; } cp{};
	bool pendingLoad = false, finished = false;
	int saves = 0, loads = 0, deaths = 0;
	int64_t stepsRun = 0;
	bool dup = false; // call beforeStep twice per frame (like half ticks)

	Mock(std::vector<Orb> o) : orbs(std::move(o)) {
		hit.assign(orbs.size(), false);
		pf.assign(orbs.size(), -1);
		for (auto const& orb : orbs) endFrame = std::max(endFrame, orb.hi + 20);
	}

	void run(Searcher& s) {
		int tick = 0;
		while (!finished) {
			tick++;
			// ---- one "update()" ----
			if (pendingLoad) {
				frame = cp.frame; hit = cp.hit; pf = cp.pf; pendingLoad = false; loads++;
				s.onLoaded(frame);
			}
			int stepsThisUpdate = 1 + (tick * 7) % 5;
			for (int i = 0; i < stepsThisUpdate && !finished && !pendingLoad; i++) {
				if (!step(s)) break;
			}
			assert(tick < 5000000);
		}
	}

	bool handle(StepCmd const& c) {
		if (c.saveCheckpoint) { cp = {frame, hit, pf}; saves++; }
		for (auto const& e : c.inputs) {
			if (e.button != 1) continue;
			for (size_t i = 0; i < orbs.size(); i++) {
				if (orbs[i].rel != !e.down || hit[i]) continue;
				int64_t lo = orbs[i].lo;
				if (orbs[i].dep >= 0 && pf[orbs[i].dep] >= 0) lo += pf[orbs[i].dep] - orbs[i].depBase;
				if (frame >= lo && frame <= orbs[i].hi) { hit[i] = true; pf[i] = frame; }
			}
		}
		if (c.loadCheckpoint) pendingLoad = true;
		if (c.done) finished = true;
		return !(c.skipStep || c.done);
	}

	bool step(Searcher& s) {
		auto c = s.beforeStep(frame, (float)frame * 10, (float)frame);
		if (!handle(c)) return false;
		if (dup) { auto c2 = s.beforeStep(frame, (float)frame * 10, (float)frame); assert(c2.inputs.empty() && !c2.saveCheckpoint && !c2.loadCheckpoint); }
		stepsRun++;
		for (size_t i = 0; i < orbs.size(); i++) {
			if (orbs[i].hi == frame && !hit[i]) {
				deaths++;
				auto d = s.onDeath(frame);
				handle(d);
				return false;
			}
		}
		if (frame >= endFrame) { auto d = s.onLevelComplete(frame); handle(d); return false; }
		frame++;
		return true;
	}
};

static std::vector<InputEvent> macroFor(std::vector<int64_t> presses, int hold = 3) {
	std::vector<InputEvent> ev;
	for (auto p : presses) {
		ev.push_back({p, 1, false, true});
		ev.push_back({p + hold, 1, false, false});
	}
	return ev;
}

int main() {
	SearchSettings cfg; cfg.maxOffset = 12;
	SearchSettings clicksOnly = cfg; clicksOnly.analyzeReleases = false;

	// ---- test 1: assorted click windows ----
	{
		std::vector<Orb> orbs = { {5, 5}, {100, 103}, {200, 206}, {300, 400}, {500, 504} };
		std::vector<int64_t> presses = { 5, 101, 205, 350, 500 };
		for (bool dup : {false, true}) {
			Mock m(orbs); m.dup = dup;
			Searcher s(macroFor(presses), clicksOnly);
			m.run(s);
			assert(s.finished() && !s.aborted());
			auto const& r = s.results();
			assert(r.size() == 5);
			int expectWin[] = {1, 4, 7, 25, 5};
			for (size_t i = 0; i < r.size(); i++) {
				std::printf("click %zu @%lld: early=%d late=%d window=%d%s  pos=(%.0f,%.0f)\n", i, (long long)r[i].frame,
					r[i].early, r[i].late, r[i].window(), r[i].capped() ? "+" : "", r[i].x, r[i].y);
				assert(r[i].baselineOk && !r[i].release);
				assert(r[i].window() == expectWin[i]);
				assert(r[i].hasPos && r[i].x == r[i].frame * 10.f);
			}
			assert(r[3].cappedEarly && r[3].cappedLate);
			assert(!r[1].capped() && !r[0].capped());
			std::printf("  saves=%d loads=%d steps=%lld deaths=%d (dup=%d)\n", m.saves, m.loads, (long long)m.stepsRun, m.deaths, dup);
		}
	}

	// ---- test 2: clicks very close together share checkpoints ----
	{
		std::vector<Orb> orbs = { {50, 53}, {58, 59}, {64, 70} };
		Mock m(orbs);
		Searcher s(macroFor({52, 58, 65}, 2), clicksOnly);
		m.run(s);
		assert(s.finished());
		auto const& r = s.results();
		int expectWin[] = {4, 2, 7};
		for (size_t i = 0; i < r.size(); i++) {
			std::printf("close click %zu: window=%d\n", i, r[i].window());
			assert(r[i].window() == expectWin[i]);
		}
	}

	// ---- test 3: bad macro (click misses its orb) is reported ----
	{
		std::vector<Orb> orbs = { {100, 103}, {200, 203} };
		Mock m(orbs);
		Searcher s(macroFor({50, 201}), clicksOnly);
		m.run(s);
		std::printf("bad macro: aborted=%d err='%s'\n", s.aborted(), s.error().c_str());
		assert(s.aborted() && !s.error().empty());
	}

	// ---- test 4: empty macros finish immediately ----
	{
		Searcher s({}, cfg);
		assert(s.finished());
		assert(s.beforeStep(0, 0, 0).done);
	}

	// ---- test 5: duplicate presses / stray releases don't confuse pairing ----
	{
		std::vector<InputEvent> ev = { {10,1,false,false}, {20,1,false,true}, {21,1,false,true}, {25,1,false,false}, {26,1,false,false} };
		assert(Searcher(ev, cfg).targetCount() == 2);        // the click and its release
		assert(Searcher(ev, clicksOnly).targetCount() == 1);
		SearchSettings relOnly = cfg; relOnly.analyzeClicks = false;
		assert(Searcher(ev, relOnly).targetCount() == 1);
	}

	// ======================= RELEASES =======================

	// ---- test 6: click and release each get their own window ----
	{
		// press 100 (orb [100,102]) hold 10, release 110 (orb [108,113])
		std::vector<Orb> orbs = { {100, 102, false}, {108, 113, true} };
		std::vector<InputEvent> ev = { {100,1,false,true}, {110,1,false,false} };
		for (bool dup : {false, true}) {
			Mock m(orbs); m.dup = dup;
			Searcher s(ev, cfg);
			m.run(s);
			assert(s.finished() && !s.aborted());
			auto const& r = s.results();
			assert(r.size() == 2);
			std::printf("click   @%lld: early=%d late=%d window=%d\n", (long long)r[0].frame, r[0].early, r[0].late, r[0].window());
			std::printf("release @%lld: early=%d late=%d window=%d pos=(%.0f,%.0f)\n", (long long)r[1].frame, r[1].early, r[1].late, r[1].window(), r[1].x, r[1].y);
			assert(!r[0].release && r[1].release);
			assert(r[0].early == 0 && r[0].late == 2 && r[0].window() == 3);
			assert(r[1].early == 2 && r[1].late == 3 && r[1].window() == 6);
			assert(r[1].hasPos && r[1].x == 1100.f);
		}
	}

	// ---- test 7: release can't move before its press or past the next press ----
	{
		// press 100, release 103 (hold 3), next press 113, release 116
		std::vector<Orb> orbs = { {97, 105, false}, {60, 200, true}, {113, 114, false} };
		std::vector<InputEvent> ev = { {100,1,false,true}, {103,1,false,false}, {113,1,false,true}, {116,1,false,false} };
		Mock m(orbs);
		Searcher s(ev, cfg);
		m.run(s);
		assert(s.finished() && !s.aborted());
		auto const& r = s.results();
		assert(r.size() == 4); // press, release, press, release
		for (auto const& x : r) std::printf("  %s @%lld early=%d late=%d%s%s\n", x.release ? "release" : "click  ", (long long)x.frame, x.early, x.late, x.cappedEarly ? " capE" : "", x.cappedLate ? " capL" : "");
		// click 1: free orb [97,105] -> 3 early, 5 late
		assert(r[0].early == 3 && r[0].late == 5);
		// release 1: any time works, but only 2 frames earlier before it would sit on its press (hold >= 1),
		// and 9 frames later before the next press at 113. Those are real limits, not caps.
		assert(r[1].release && r[1].early == 2 && r[1].late == 9);
		assert(!r[1].cappedEarly && !r[1].cappedLate);
		assert(r[1].window() == 12);
		assert(r[2].window() == 2);
	}

	// ---- test 8: release shift off: click can't run past its own release ----
	{
		std::vector<Orb> orbs = { {95, 110, false} };
		std::vector<InputEvent> ev = { {100,1,false,true}, {104,1,false,false} };
		SearchSettings noShift = clicksOnly; noShift.shiftRelease = false;
		{
			Mock m(orbs);
			Searcher s(ev, noShift);
			m.run(s);
			auto const& r = s.results();
			std::printf("no-shift click: early=%d late=%d capL=%d\n", r[0].early, r[0].late, r[0].cappedLate);
			assert(r[0].early == 5 && r[0].late == 3 && !r[0].cappedLate); // late stops at release-1
		}
		{
			Mock m(orbs);
			Searcher s(ev, clicksOnly); // release shifts along: no limit until the orb ends
			m.run(s);
			auto const& r = s.results();
			assert(r[0].early == 5 && r[0].late == 10 && r[0].window() == 16);
		}
	}

	// ---- test 9: releases only ----
	{
		std::vector<Orb> orbs = { {50, 60, false}, {70, 74, true} };
		std::vector<InputEvent> ev = { {55,1,false,true}, {72,1,false,false} };
		SearchSettings relOnly = cfg; relOnly.analyzeClicks = false;
		Mock m(orbs);
		Searcher s(ev, relOnly);
		m.run(s);
		auto const& r = s.results();
		assert(r.size() == 1 && r[0].release);
		assert(r[0].early == 2 && r[0].late == 2 && r[0].window() == 5);
	}

	// ======================= FREE RELEASE =======================
	SearchSettings freeCfg = clicksOnly; freeCfg.freeRelease = true; freeCfg.freeReleaseRange = 4;

	// ---- test 10: the ship example. Pressing earlier/later also needs a different release ----
	{
		// press 104, release 109. Press anywhere in [98,110]; the release must land in [108,110].
		std::vector<Orb> orbs = { {98, 110, false}, {108, 110, true} };
		std::vector<InputEvent> ev = { {104,1,false,true}, {109,1,false,false} };
		Mock m1(orbs); Searcher fixedS(ev, clicksOnly); m1.run(fixedS);
		Mock m2(orbs); Searcher freeS(ev, freeCfg);     m2.run(freeS);
		auto const& f = fixedS.results()[0];
		auto const& r = freeS.results()[0];
		std::printf("ship: fixed hold early=%d late=%d window=%d | free release early=%d late=%d window=%d\n",
			f.early, f.late, f.window(), r.early, r.late, r.window());
		assert(f.early == 1 && f.late == 1 && f.window() == 3);     // hold length fixed: only 1 frame each way
		assert(r.early == 5 && r.late == 5 && r.window() == 11);    // free release: the click really has 11
	}

	// ---- test 11: next press close behind: a shorter hold still fits, the original hold doesn't ----
	{
		// click 1: press 100, release 105; click 2: press 108. Release only has to land in [100,107].
		std::vector<Orb> orbs = { {95, 106, false}, {100, 107, true}, {108, 109, false} };
		std::vector<InputEvent> ev = { {100,1,false,true}, {105,1,false,false}, {108,1,false,true}, {112,1,false,false} };
		Mock m1(orbs); Searcher fixedS(ev, clicksOnly); m1.run(fixedS);
		Mock m2(orbs); Searcher freeS(ev, freeCfg);     m2.run(freeS);
		auto const& f = fixedS.results();
		auto const& r = freeS.results();
		std::printf("short gap: fixed late=%d | free late=%d (limit, not a cap: %d)\n", f[0].late, r[0].late, !r[0].cappedLate);
		assert(f[0].early == 5 && f[0].late == 2 && f[0].window() == 8);
		assert(r[0].early == 5 && r[0].late == 6 && r[0].window() == 12 && !r[0].cappedLate);
		assert(f[1].window() == 2 && r[1].window() == 2);           // second click unaffected
	}

	// ---- test 12: randomized: free release matches a brute-force answer and never beats/loses to fixed wrongly ----
	{
		uint32_t rng = 12345;
		auto rnd = [&](int lo, int hi) { rng = rng * 1664525u + 1013904223u; return lo + (int)((rng >> 8) % (uint32_t)(hi - lo + 1)); };
		int checked = 0, widened = 0;
		for (int iter = 0; iter < 80; iter++) {
			int64_t p = 40 + rnd(0, 30);
			int hold = rnd(2, 9);
			int64_t rel = p + hold;
			int a = rnd(0, 6), b = rnd(0, 6), c = rnd(0, 6), d = rnd(0, 6);
			Orb po{p - a, p + b, false}, ro{rel - c, rel + d, true};
			bool shiftRel = rnd(0, 1) == 1;
			SearchSettings fx = clicksOnly; fx.shiftRelease = shiftRel;
			SearchSettings fr = freeCfg;    fr.shiftRelease = shiftRel;
			std::vector<InputEvent> ev = { {p,1,false,true}, {rel,1,false,false} };
			Mock m1({po, ro}); Searcher s1(ev, fx); m1.run(s1);
			Mock m2({po, ro}); Searcher s2(ev, fr); m2.run(s2);
			auto const& rf = s1.results()[0];
			auto const& rr = s2.results()[0];
			assert(rf.baselineOk && rr.baselineOk);
			// brute force for free mode
			auto passes = [&](int k) {
				if (p + k < po.lo || p + k > po.hi) return false;
				int64_t anchor = shiftRel ? rel + k : rel;
				for (int j = -4; j <= 4; j++) {
					int64_t r2 = anchor + j;
					if (r2 >= p + k + 1 && r2 >= ro.lo && r2 <= ro.hi) return true;
				}
				return false;
			};
			int expLate = 0, expEarly = 0;
			for (int k = 1; k <= 12 && passes(k); k++) expLate = k;
			for (int k = 1; k <= 12 && passes(-k); k++) expEarly = k;
			if (rr.early != expEarly || rr.late != expLate) {
				std::printf("MISMATCH iter %d (shiftRel=%d p=%lld hold=%d po=[%lld,%lld] ro=[%lld,%lld]): free early=%d late=%d, expected %d/%d\n",
					iter, shiftRel, (long long)p, hold, (long long)po.lo, (long long)po.hi, (long long)ro.lo, (long long)ro.hi, rr.early, rr.late, expEarly, expLate);
				assert(false);
			}
			// free mode can only ever find MORE room than a fixed hold
			assert(rr.early >= rf.early && rr.late >= rf.late);
			if (rr.window() > rf.window()) widened++;
			checked++;
		}
		std::printf("random free-release cases matched brute force: %d (free mode was wider in %d)\n", checked, widened);
		assert(widened > 5); // the scenarios really do exercise the feature
	}

	// ======================= CONDITIONAL WINDOWS =======================
	SearchSettings condCfg = clicksOnly; condCfg.conditional = true;

	// ---- test 13: your example. Clicking the first jump later squeezes the next one ----
	{
		// jump A recorded on 100, window [97,103] -> 7 frames. Jump B recorded on 200; B's window starts
		// 1 frame later for every frame A is later (and earlier if A is earlier).
		std::vector<Orb> orbs = { {97, 103, false}, {195, 205, false, 0, 100} };
		auto ev = macroFor({100, 200});
		Mock m(orbs); Searcher s(ev, condCfg); m.run(s);
		assert(s.finished() && !s.aborted());
		auto const& r = s.results();
		assert(r.size() == 2);
		std::printf("A window=%d  next-click windows by position:", r[0].window());
		for (int v : r[0].cond) std::printf(" %d", v);
		std::printf("\n");
		assert(r[0].window() == 7 && r[0].cond.size() == 7);
		int expect[] = {14, 13, 12, 11, 10, 9, 8};      // position 1 = A three frames early ... 7 = three late
		for (int i = 0; i < 7; i++) assert(r[0].cond[i] == expect[i]);
		assert(r[0].condNextFrame == 200);
		assert(r[1].window() == 11 && r[1].cond.empty());   // B alone, with A as recorded
		assert(r[0].cond[3] == r[1].window());              // A on its recorded frame = B's normal window
	}

	// ---- test 14: same with free release on (release tweak must be plumbed into the condition runs) ----
	{
		std::vector<Orb> orbs = { {97, 103, false}, {195, 205, false, 0, 100} };
		SearchSettings both = condCfg; both.freeRelease = true;
		Mock m(orbs); Searcher s(macroFor({100, 200}), both); m.run(s);
		auto const& r = s.results();
		int expect[] = {14, 13, 12, 11, 10, 9, 8};
		assert(r[0].cond.size() == 7);
		for (int i = 0; i < 7; i++) assert(r[0].cond[i] == expect[i]);
	}

	// ---- test 15: a chain A -> B -> C: each click gets its own table ----
	{
		std::vector<Orb> orbs = { {97, 103, false}, {195, 205, false, 0, 100}, {295, 305, false, 1, 200} };
		Mock m(orbs); Searcher s(macroFor({100, 200, 300}), condCfg); m.run(s);
		assert(s.finished() && !s.aborted());
		auto const& r = s.results();
		assert(r[1].window() == 11 && r[1].cond.size() == 11);
		for (int p = 0; p < 11; p++) assert(r[1].cond[p] == 16 - p);   // B early by 5 -> C has 16 ... B late by 5 -> C has 6
		assert(r[0].cond.size() == 7 && r[2].cond.empty());
		std::printf("chain ok: B's table for C = %d ... %d\n", r[1].cond.front(), r[1].cond.back());
	}

	// ---- test 16: conditions off by default, and no table for releases / the last click ----
	{
		std::vector<Orb> orbs = { {97, 103, false}, {195, 205, false, 0, 100} };
		Mock m(orbs); Searcher s(macroFor({100, 200}), clicksOnly); m.run(s);
		assert(s.results()[0].cond.empty());
		SearchSettings withRel = condCfg; withRel.analyzeReleases = true;
		Mock m2(orbs); Searcher s2(macroFor({100, 200}), withRel); m2.run(s2);
		for (auto const& x : s2.results()) if (x.release) assert(x.cond.empty());
		assert(s2.results()[0].cond.size() == 7);   // first click (a press) still gets its table
	}

	std::cout << "ALL SEARCH TESTS PASSED\n";
}
