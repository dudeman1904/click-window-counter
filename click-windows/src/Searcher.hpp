#pragma once

// Frame-window search. No Geode / GD dependencies, so it can be unit tested.
//
// A "target" is one input of the macro: a click (press) or a release. Its frame window is how many
// frames wide the range of timings is that still gets you through (e.g. an orb you can hit 4 frames
// early..late). We measure it by re-simulating: restore a checkpoint shortly before the input, shift
// that input by k frames, replay, and see if the player survives.
//
// Conditional windows: a click's timing can change how much room the NEXT click has. For every timing
// that works for a click, we also measure the next click's window with the first click moved there.
//
// The glue code (main.cpp) drives this class:
//   beforeStep()      - call at the start of every physics step
//   onDeath()         - call when the player would die
//   onLevelComplete() - call when the level is completed
//   onLoaded()        - call after the glue restored the checkpoint

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cw {

struct InputEvent {
	int64_t frame = 0;   // frame at 240 Hz
	int button = 1;      // 1 = jump, 2 = left, 3 = right
	bool p2 = false;
	bool down = false;
};

struct SearchSettings {
	int maxOffset = 12;           // test inputs up to +-N frames from the original
	int tailFrames = 30;          // survive this long after the next click
	int maxHorizon = 240;         // never simulate more than this after the input
	bool shiftRelease = true;     // when testing a click, move its release together with the press
	bool onlyJump = true;         // only analyze button 1 (jump)
	bool analyzeClicks = true;    // measure presses
	bool analyzeReleases = true;  // measure releases (press stays fixed, only the release moves)
	// Free release: when testing a click, also try releasing a few frames sooner/later than the shifted hold
	// would. An offset passes if ANY of those releases survives. Slower, but it finds the click window you
	// really have when you may adapt the release (e.g. ship holds).
	bool freeRelease = false;
	int freeReleaseRange = 4;     // release may differ by up to +-N frames from the shifted hold
	// Conditional windows: for each working timing of a click, measure the next click's window.
	bool conditional = false;
};

struct ClickResult {
	int64_t frame = 0;        // original frame of the input
	int button = 1;
	bool p2 = false;
	bool release = false;     // false = click (press), true = release
	bool baselineOk = false;  // unmodified input reproduced (no death)
	int early = 0;            // how many frames earlier still works
	int late = 0;             // how many frames later still works
	bool cappedEarly = false; // hit maxOffset without failing
	bool cappedLate = false;
	bool hasPos = false;
	float x = 0.f, y = 0.f;   // player position when the input happened
	bool hidden = false;      // user switched this marker off

	// Conditional windows (clicks only). cond[p-1] = window of the NEXT click when this click is on
	// position p of its own window (p = 1 is the earliest working frame, p = window() the latest).
	// 0 means the next click couldn't be reproduced at all.
	std::vector<int> cond;
	int64_t condNextFrame = 0; // original frame of that next click

	int window() const { return baselineOk ? early + late + 1 : 0; }
	bool capped() const { return cappedEarly || cappedLate; }
};

struct StepCmd {
	std::vector<InputEvent> inputs; // inputs to apply right now (after saving, if asked)
	bool saveCheckpoint = false;    // save a checkpoint of the state BEFORE applying inputs
	bool loadCheckpoint = false;    // glue must restore the checkpoint, then call onLoaded()
	bool skipStep = false;          // do not run the physics step
	bool done = false;              // analysis finished (or aborted)
	bool aborted = false;
};

class Searcher {
public:
	Searcher(std::vector<InputEvent> events, SearchSettings cfg) : events_(std::move(events)), cfg_(cfg) {
		cfg_.maxOffset = std::max(1, cfg_.maxOffset);
		cfg_.tailFrames = std::max(1, cfg_.tailFrames);
		cfg_.maxHorizon = std::max(cfg_.maxHorizon, cfg_.maxOffset + cfg_.tailFrames + 1);
		cfg_.freeReleaseRange = std::clamp(cfg_.freeReleaseRange, 1, 40);
		std::stable_sort(events_.begin(), events_.end(),
			[](InputEvent const& a, InputEvent const& b) { return a.frame < b.frame; });
		buildTargets();
		if (targets_.empty()) st_ = St::Finished;
	}

	// ----- driven by the glue -----

	StepCmd beforeStep(int64_t frame, float x, float y) {
		StepCmd cmd;
		switch (st_) {
			case St::Finished: cmd.done = true; return cmd;
			case St::Aborted: cmd.done = true; cmd.aborted = true; return cmd;
			case St::WaitLoad: cmd.skipStep = true; return cmd;
			case St::Baseline: {
				if (frame == lastFrame_) return cmd;
				lastFrame_ = frame;
				int64_t cf = checkpointFrameFor(ci_);
				if (frame >= cf) {
					if (frame > targets_[ci_].frame) {
						return abort(cmd, "Passed input " + std::to_string(ci_ + 1) + " before a checkpoint could be saved");
					}
					cpFrame_ = frame;
					heldAtCp_ = held_;
					cmd.saveCheckpoint = true;
					beginTarget();
					setupTrial();
					st_ = St::Trial;
					return stepTrial(frame, x, y, cmd);
				}
				applyBaseline(frame, cmd);
				return cmd;
			}
			case St::Trial: {
				if (frame == lastFrame_) return cmd;
				lastFrame_ = frame;
				return stepTrial(frame, x, y, cmd);
			}
		}
		return cmd;
	}

	StepCmd onDeath(int64_t frame) {
		StepCmd cmd;
		if (st_ == St::Trial) return endTrial(false, cmd);
		if (st_ == St::Baseline) {
			return abort(cmd, "The macro died at frame " + std::to_string(frame) +
				" before analysis reached input " + std::to_string(ci_ + 1) +
				". It probably desyncs (wrong level, version, or tick rate)");
		}
		return cmd; // extra death callbacks while waiting for a load are ignored
	}

	StepCmd onLevelComplete(int64_t) {
		StepCmd cmd;
		if (st_ == St::Trial) return endTrial(true, cmd);
		if (st_ == St::Baseline) {
			st_ = St::Finished;
			cmd.done = true;
		}
		return cmd;
	}

	void onLoaded(int64_t) {
		if (st_ != St::WaitLoad) return;
		lastFrame_ = -1;
		if (loadKind_ == LoadKind::Trial) {
			setupTrial();
			st_ = St::Trial;
		} else {
			held_ = heldAtCp_;
			evIdx_ = std::lower_bound(events_.begin(), events_.end(), cpFrame_,
				[](InputEvent const& e, int64_t f) { return e.frame < f; }) - events_.begin();
			st_ = St::Baseline;
		}
	}

	// ----- queries -----

	int64_t checkpointFrame() const { return cpFrame_; }
	bool finished() const { return st_ == St::Finished; }
	bool aborted() const { return st_ == St::Aborted; }
	std::string const& error() const { return error_; }
	size_t targetCount() const { return targets_.size(); }
	size_t currentTarget() const { return std::min(ci_, targets_.size()); }
	bool inConditions() const { return stage_ == Stage::Inner; }
	size_t conditionPos() const { return std::min(condPos_ + 1, condOffsets_.size()); }
	size_t conditionCount() const { return condOffsets_.size(); }
	std::vector<ClickResult> const& results() const { return res_; }

private:
	enum class St { Baseline, WaitLoad, Trial, Finished, Aborted };
	enum class Phase { Zero, Late, Early };
	enum class LoadKind { Trial, Baseline };
	enum class Stage { Outer, Inner };

	static constexpr size_t npos = static_cast<size_t>(-1);

	struct Target {
		bool release = false;
		size_t idx = 0;            // event that gets shifted
		size_t pairedIdx = npos;   // click: its release, release: its press
		int64_t frame = 0;
		int button = 1;
		bool p2 = false;
		size_t prevReleaseIdx = npos; // clicks: previous release of the same button
		bool hasNextPress = false;    // next press of the same button
		int64_t nextPress = 0;
	};

	struct Shift {
		size_t idx;
		int64_t delta;
	};

	struct Cur {
		bool baselineOk = false;
		int early = 0, late = 0;
		bool cappedEarly = false, cappedLate = false;
	};

	static int key(int button, bool p2) { return (button & 3) * 2 + (p2 ? 1 : 0); }
	using Held = std::array<bool, 8>;

	void buildTargets() {
		Held held{};
		std::array<size_t, 8> openPress; openPress.fill(npos);
		std::array<size_t, 8> openPressTarget; openPressTarget.fill(npos);
		std::array<size_t, 8> lastReleaseIdx; lastReleaseIdx.fill(npos);
		std::array<std::vector<size_t>, 8> waiting; // targets still waiting for "next press of this key"

		for (size_t i = 0; i < events_.size(); i++) {
			auto const& e = events_[i];
			int k = key(e.button, e.p2);
			bool analyzed = !cfg_.onlyJump || e.button == 1;
			if (e.down) {
				if (held[k]) continue; // duplicate press
				held[k] = true;
				for (auto ti : waiting[k]) {
					targets_[ti].hasNextPress = true;
					targets_[ti].nextPress = e.frame;
				}
				waiting[k].clear();
				openPress[k] = i;
				openPressTarget[k] = npos;
				if (analyzed && cfg_.analyzeClicks) {
					Target t;
					t.idx = i;
					t.frame = e.frame;
					t.button = e.button;
					t.p2 = e.p2;
					t.prevReleaseIdx = lastReleaseIdx[k];
					openPressTarget[k] = targets_.size();
					waiting[k].push_back(targets_.size());
					targets_.push_back(t);
				}
			} else {
				if (!held[k]) continue; // stray release
				held[k] = false;
				lastReleaseIdx[k] = i;
				if (openPressTarget[k] != npos) targets_[openPressTarget[k]].pairedIdx = i;
				if (analyzed && cfg_.analyzeReleases) {
					Target t;
					t.release = true;
					t.idx = i;
					t.pairedIdx = openPress[k];
					t.frame = e.frame;
					t.button = e.button;
					t.p2 = e.p2;
					waiting[k].push_back(targets_.size());
					targets_.push_back(t);
				}
				openPress[k] = npos;
				openPressTarget[k] = npos;
			}
		}

		res_.resize(targets_.size());
		nextClick_.assign(targets_.size(), npos);
		size_t nextClickIdx = npos;
		for (size_t i = targets_.size(); i-- > 0;) {
			nextClick_[i] = nextClickIdx;
			if (!targets_[i].release) nextClickIdx = i;
		}
		for (size_t i = 0; i < targets_.size(); i++) {
			res_[i].frame = targets_[i].frame;
			res_[i].button = targets_[i].button;
			res_[i].p2 = targets_[i].p2;
			res_[i].release = targets_[i].release;
		}
		// targets were created in event order and events are sorted, so frames are non-decreasing
	}

	int64_t checkpointFrameFor(size_t ci) const {
		return std::max<int64_t>(0, targets_[ci].frame - (cfg_.maxOffset + 1));
	}

	void emit(InputEvent const& e, Held& held, StepCmd& cmd) {
		int k = key(e.button, e.p2);
		if (held[k] == e.down) return; // redundant
		held[k] = e.down;
		cmd.inputs.push_back(e);
	}

	void applyBaseline(int64_t frame, StepCmd& cmd) {
		while (evIdx_ < events_.size() && events_[evIdx_].frame <= frame) {
			emit(events_[evIdx_++], held_, cmd);
		}
	}

	// frame of an event with the fixed (outer click) modifications applied
	int64_t effFrame(size_t evIdx) const {
		int64_t f = events_[evIdx].frame;
		for (auto const& s : baseShifts_) if (s.idx == evIdx) f += s.delta;
		return f;
	}

	// Starts the search for targets_[ci_] (given the checkpoint at cpFrame_).
	void beginTarget() {
		stage_ = Stage::Outer;
		sub_ = ci_;
		baseShifts_.clear();
		outerPass_.clear();
		condOffsets_.clear();
		condPos_ = 0;
		beginSubject();
	}

	// Sets up a search for targets_[sub_]. The limits keep shifted inputs from changing the input pattern
	// itself (e.g. a release moving past its press or past the next click).
	void beginSubject() {
		auto const& t = targets_[sub_];
		phase_ = Phase::Zero;
		k_ = 0;
		hasPos_ = false;
		cur_ = Cur{};

		constexpr int64_t inf = INT64_MAX / 4;
		int64_t early = std::min<int64_t>(cfg_.maxOffset, t.frame - cpFrame_);
		int64_t late = cfg_.maxOffset;
		if (!t.release) {
			if (t.prevReleaseIdx != npos) early = std::min(early, t.frame - effFrame(t.prevReleaseIdx) - 1);
			if (t.pairedIdx != npos) {
				int64_t rel = events_[t.pairedIdx].frame;
				int64_t room;
				if (cfg_.freeRelease) {
					// a shorter hold may still fit: the press only needs room for a 1 frame hold before the
					// latest allowed release
					int64_t maxRel = t.hasNextPress ? t.nextPress - 1 : inf;
					if (!cfg_.shiftRelease) maxRel = std::min<int64_t>(maxRel, rel + cfg_.freeReleaseRange);
					room = maxRel - 1 - t.frame;
				} else {
					room = cfg_.shiftRelease
						? (t.hasNextPress ? t.nextPress - rel - 1 : inf)
						: rel - t.frame - 1;
				}
				late = std::min(late, room);
			}
		} else {
			if (t.pairedIdx != npos) early = std::min(early, t.frame - effFrame(t.pairedIdx) - 1);
			if (t.hasNextPress) late = std::min(late, t.nextPress - t.frame - 1);
		}
		earlyLimit_ = static_cast<int>(std::max<int64_t>(0, early));
		lateLimit_ = static_cast<int>(std::max<int64_t>(0, late));

		int64_t nextAnyPress = -1;
		for (auto const& e : events_) {
			if (e.down && e.frame > t.frame) { nextAnyPress = e.frame; break; }
		}
		int64_t cap = t.frame + cfg_.maxHorizon;
		if (nextAnyPress < 0) {
			trialEnd_ = cap;
		} else {
			trialEnd_ = std::min(cap, std::max(nextAnyPress + cfg_.tailFrames, t.frame + cfg_.maxOffset + cfg_.tailFrames));
		}
		prepareCands(); // phase Zero: just the unmodified release
	}

	void currentShifts(std::vector<Shift>& out) const {
		out = baseShifts_;
		auto const& t = targets_[sub_];
		out.push_back({t.idx, k_});
		if (!t.release && t.pairedIdx != npos) {
			out.push_back({t.pairedIdx, (cfg_.shiftRelease ? k_ : 0) + currentReleaseTweak()});
		}
	}

	void setupTrial() {
		std::vector<int64_t> frames(events_.size());
		for (size_t i = 0; i < events_.size(); i++) frames[i] = events_[i].frame;
		std::vector<Shift> shifts;
		currentShifts(shifts);
		for (auto const& s : shifts) frames[s.idx] += s.delta;

		trialEv_.clear();
		for (size_t i = 0; i < events_.size(); i++) {
			if (frames[i] < cpFrame_) continue; // already part of the checkpointed state
			InputEvent e = events_[i];
			e.frame = frames[i];
			trialEv_.push_back(e);
		}
		std::stable_sort(trialEv_.begin(), trialEv_.end(),
			[](InputEvent const& a, InputEvent const& b) { return a.frame < b.frame; });
		tp_ = 0;
		trialHeld_ = heldAtCp_;
	}

	StepCmd& stepTrial(int64_t frame, float x, float y, StepCmd& cmd) {
		if (stage_ == Stage::Outer && phase_ == Phase::Zero && !hasPos_ && frame >= targets_[sub_].frame) {
			auto& r = res_[ci_];
			r.x = x; r.y = y; r.hasPos = true;
			hasPos_ = true;
		}
		if (frame >= trialEnd_) return endTrial(true, cmd);
		while (tp_ < trialEv_.size() && trialEv_[tp_].frame <= frame) {
			emit(trialEv_[tp_++], trialHeld_, cmd);
		}
		return cmd;
	}

	bool startLate() {
		phase_ = Phase::Late;
		k_ = 1;
		return lateLimit_ >= 1;
	}

	bool startEarly() {
		phase_ = Phase::Early;
		k_ = -1;
		return earlyLimit_ >= 1;
	}

	// Release timings to try for the click offset k_ (relative to where the shifted hold would release).
	// Ordered 0, -1, +1, -2, +2 ... and limited so the release stays after its press and before the next press.
	// Returns false if no valid release exists for this offset.
	bool prepareCands() {
		cands_.clear();
		candIdx_ = 0;
		auto const& t = targets_[sub_];
		bool freeMode = cfg_.freeRelease && !t.release && t.pairedIdx != npos && phase_ != Phase::Zero;
		if (!freeMode) {
			cands_.push_back(0);
			return true;
		}
		constexpr int64_t inf = INT64_MAX / 4;
		int64_t rel = events_[t.pairedIdx].frame;
		int64_t anchor = cfg_.shiftRelease ? rel + k_ : rel;
		int64_t minRel = t.frame + k_ + 1;                              // hold at least 1 frame
		int64_t maxRel = t.hasNextPress ? t.nextPress - 1 : inf;       // before the next press
		int64_t jmin = std::max<int64_t>(-cfg_.freeReleaseRange, minRel - anchor);
		int64_t jmax = std::min<int64_t>(cfg_.freeReleaseRange, maxRel - anchor);
		for (int d = 0; d <= cfg_.freeReleaseRange; d++) {
			if (d == 0) {
				if (0 >= jmin && 0 <= jmax) cands_.push_back(0);
				continue;
			}
			if (-d >= jmin && -d <= jmax) cands_.push_back(-d);
			if (d >= jmin && d <= jmax) cands_.push_back(d);
		}
		return !cands_.empty();
	}

	int currentReleaseTweak() const {
		return candIdx_ < cands_.size() ? cands_[candIdx_] : 0;
	}

	// Decides what the result of the trial that just finished means. Returns true if another trial is needed.
	// (An offset only fails once every release option for it has failed.)
	bool scheduleNext(bool pass) {
		if (phase_ != Phase::Zero && !pass && candIdx_ + 1 < cands_.size()) {
			++candIdx_; // same offset, try the next release timing
			return true;
		}
		bool again = advance(pass);
		while (again) {
			if (prepareCands()) return true;
			again = advance(false); // no valid release at all for this offset: counts as a failure
		}
		return finishSearch();
	}

	// Offset-level bookkeeping. Returns true if another offset should be tested.
	bool advance(bool pass) {
		switch (phase_) {
			case Phase::Zero:
				if (!pass) return false;
				cur_.baselineOk = true;
				if (stage_ == Stage::Outer) outerPass_.push_back({0, 0});
				if (startLate()) return true;
				return startEarly();
			case Phase::Late:
				if (pass) {
					cur_.late = k_;
					if (stage_ == Stage::Outer) outerPass_.push_back({k_, currentReleaseTweak()});
					if (k_ >= lateLimit_) {
						cur_.cappedLate = lateLimit_ >= cfg_.maxOffset;
						return startEarly();
					}
					++k_;
					return true;
				}
				return startEarly();
			case Phase::Early:
				if (pass) {
					cur_.early = -k_;
					if (stage_ == Stage::Outer) outerPass_.push_back({k_, currentReleaseTweak()});
					if (-k_ >= earlyLimit_) {
						cur_.cappedEarly = earlyLimit_ >= cfg_.maxOffset;
						return false;
					}
					--k_;
					return true;
				}
				return false;
		}
		return false;
	}

	// The current search (outer click, or the next click under a condition) is complete.
	// Returns true if more trials follow (a condition search started), false if the target is done.
	bool finishSearch() {
		if (stage_ == Stage::Outer) {
			auto& r = res_[ci_];
			r.baselineOk = cur_.baselineOk;
			r.early = cur_.early;
			r.late = cur_.late;
			r.cappedEarly = cur_.cappedEarly;
			r.cappedLate = cur_.cappedLate;
			return beginConditions();
		}
		// inner: record the next click's window for this timing of the outer click
		res_[ci_].cond[condPos_] = cur_.baselineOk ? cur_.early + cur_.late + 1 : 0;
		if (++condPos_ < condOffsets_.size()) {
			beginInner();
			return true;
		}
		stage_ = Stage::Outer;
		return false;
	}

	bool beginConditions() {
		auto const& t = targets_[ci_];
		auto& r = res_[ci_];
		if (!cfg_.conditional || t.release || nextClick_[ci_] == npos || !r.baselineOk) return false;

		// every timing of this click that worked, from earliest to latest, with the release timing that worked
		condOffsets_.clear();
		for (int k = -r.early; k <= r.late; k++) {
			int j = 0;
			bool found = false;
			for (auto const& p : outerPass_) if (p.first == k) { j = p.second; found = true; break; }
			if (!found) return false; // should not happen (search is contiguous)
			condOffsets_.push_back({k, j});
		}
		r.cond.assign(condOffsets_.size(), 0);
		r.condNextFrame = targets_[nextClick_[ci_]].frame;
		condPos_ = 0;
		beginInner();
		return true;
	}

	// Search the next click's window with this click moved to condOffsets_[condPos_].
	void beginInner() {
		auto const& t = targets_[ci_];
		auto [k, j] = condOffsets_[condPos_];
		baseShifts_.clear();
		baseShifts_.push_back({t.idx, k});
		if (t.pairedIdx != npos) baseShifts_.push_back({t.pairedIdx, (cfg_.shiftRelease ? k : 0) + j});
		stage_ = Stage::Inner;
		sub_ = nextClick_[ci_];
		beginSubject();
	}

	StepCmd& requestLoad(LoadKind kind, StepCmd& cmd) {
		st_ = St::WaitLoad;
		loadKind_ = kind;
		cmd.loadCheckpoint = true;
		cmd.skipStep = true;
		return cmd;
	}

	StepCmd& endTrial(bool pass, StepCmd& cmd) {
		if (scheduleNext(pass)) return requestLoad(LoadKind::Trial, cmd);

		// this target is done
		++ci_;
		if (ci_ >= targets_.size()) {
			st_ = St::Finished;
			cmd.done = true;
			return cmd;
		}
		if (checkpointFrameFor(ci_) <= cpFrame_) {
			// next target is close enough to reuse the same checkpoint
			beginTarget();
			return requestLoad(LoadKind::Trial, cmd);
		}
		// go back to the checkpoint and replay forward to the next one
		return requestLoad(LoadKind::Baseline, cmd);
	}

	StepCmd& abort(StepCmd& cmd, std::string msg) {
		error_ = std::move(msg);
		st_ = St::Aborted;
		cmd.done = true;
		cmd.aborted = true;
		return cmd;
	}

	std::vector<InputEvent> events_;
	SearchSettings cfg_;
	std::vector<Target> targets_;
	std::vector<size_t> nextClick_; // for each target: index of the next click (press) target
	std::vector<ClickResult> res_;

	St st_ = St::Baseline;
	LoadKind loadKind_ = LoadKind::Trial;
	Stage stage_ = Stage::Outer;
	Phase phase_ = Phase::Zero;
	size_t ci_ = 0;    // target being analyzed
	size_t sub_ = 0;   // target whose window is being searched right now (ci_, or its next click)
	int k_ = 0;
	int earlyLimit_ = 0;
	int lateLimit_ = 0;
	bool hasPos_ = false;
	int64_t cpFrame_ = 0;
	int64_t lastFrame_ = -1;
	int64_t trialEnd_ = 0;
	size_t evIdx_ = 0;
	size_t tp_ = 0;
	Held held_{};
	Held heldAtCp_{};
	Held trialHeld_{};
	std::vector<InputEvent> trialEv_;
	std::vector<int> cands_;
	size_t candIdx_ = 0;
	Cur cur_;
	std::vector<Shift> baseShifts_;                 // fixed modifications (the outer click moved to a timing)
	std::vector<std::pair<int, int>> outerPass_;    // (offset, release tweak) of every timing that worked
	std::vector<std::pair<int, int>> condOffsets_;  // the same, ordered earliest to latest
	size_t condPos_ = 0;
	std::string error_;
};

} // namespace cw
