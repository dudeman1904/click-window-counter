#pragma once

// Saving / loading analysis results as JSON, the window colour scale, and the summary text.
// Only depends on matjson.

#include <matjson.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Searcher.hpp"

namespace cw {

struct Rgb { uint8_t r, g, b; };

// Window-size buckets, used for both the marker colours and the tally panel:
//  1 | 2 | 3 | 4 | 5-6 | 7-8 | 9-12 | 13-14 | 15+
constexpr int kBucketCount = 9;
constexpr char const* kBucketLabels[kBucketCount] = {"1", "2", "3", "4", "5-6", "7-8", "9-12", "13-14", "15+"};

inline int windowBucket(int w) {
	if (w <= 1) return 0;
	if (w == 2) return 1;
	if (w == 3) return 2;
	if (w == 4) return 3;
	if (w <= 6) return 4;
	if (w <= 8) return 5;
	if (w <= 12) return 6;
	if (w <= 14) return 7;
	return 8;
}

// Colours (sampled from the reference chart):
// 1 red | 2 orange | 3 yellow | 4 white | 5-6 green | 7-8 light blue | 9-12 blue | 13-14 purple | 15+ pink
inline Rgb bucketColor(int bucket) {
	static constexpr Rgb table[kBucketCount] = {
		{245, 100, 75},   // 1
		{252, 182, 75},   // 2
		{245, 245, 112},  // 3
		{255, 255, 255},  // 4
		{135, 232, 135},  // 5-6
		{112, 195, 252},  // 7-8
		{125, 125, 250},  // 9-12
		{185, 135, 252},  // 13-14
		{245, 135, 245},  // 15+
	};
	return table[std::clamp(bucket, 0, kBucketCount - 1)];
}

inline Rgb windowColor(int w) { return bucketColor(windowBucket(w)); }

// Ring geometry for a given size multiplier (1.0 = a click ring at the default size).
// Everything scales together so the ring, outline, number and number offset stay proportional.
struct RingGeometry {
	float radius;       // ring radius
	float border;       // coloured ring thickness
	float outline;      // dark outline thickness (drawn underneath)
	float textScale;    // number size
	float textOffset;   // distance of the number from the ring centre (to its left or right)
	float textDropY;    // number sits slightly below the ring's centre line
};

inline RingGeometry ringGeometry(float size) {
	size = std::clamp(size, 0.1f, 6.f);
	RingGeometry g;
	g.radius = 20.f * size;
	g.border = std::max(1.5f, 4.f * size);
	g.outline = g.border + 2.5f;
	g.textScale = 0.45f * size;
	g.textOffset = g.radius + 8.f * size;
	g.textDropY = -4.f * size;
	return g;
}

// "1-4, 6" style list of window positions.
inline std::string formatPositions(std::vector<int> const& v) {
	std::string out;
	size_t i = 0;
	while (i < v.size()) {
		size_t j = i;
		while (j + 1 < v.size() && v[j + 1] == v[j] + 1) j++;
		if (!out.empty()) out += ", ";
		out += std::to_string(v[i]);
		if (j > i) out += (j == i + 1 ? ", " : "-") + std::to_string(v[j]);
		i = j + 1;
	}
	return out;
}

// Small text for the top-left of the screen: how the timing of this click changes the next click's window.
// Frames are positions inside this click's own window (1 = earliest working frame).
//   click window 6 (you: frame 3)
//   if on frame 1-4, 6: next click is 3
//   if on frame 5: next click is 1
inline std::string conditionText(ClickResult const& r) {
	if (r.cond.empty()) return "";
	int n = static_cast<int>(r.cond.size());
	std::ostringstream out;
	out << "click window " << n << " (you: frame " << (r.early + 1) << ")\n";

	struct Group { int value; std::vector<int> pos; };
	std::vector<Group> groups;
	for (int p = 1; p <= n; p++) {
		int v = r.cond[p - 1];
		auto it = std::find_if(groups.begin(), groups.end(), [&](Group const& g) { return g.value == v; });
		if (it == groups.end()) groups.push_back({v, {p}});
		else it->pos.push_back(p);
	}
	auto valueText = [](int v) { return v == 0 ? std::string("impossible") : std::to_string(v); };

	if (groups.size() == 1) {
		out << "next click is " << valueText(groups[0].value) << " whatever frame";
	} else if (groups.size() <= 5) {
		for (size_t i = 0; i < groups.size(); i++) {
			if (i) out << "\n";
			out << "if on frame " << formatPositions(groups[i].pos) << ": next click is " << valueText(groups[i].value);
		}
	} else {
		// too many different values to group: frame:next pairs, six per line
		out << "frame: next click";
		for (int p = 1; p <= n; p++) {
			out << ((p - 1) % 6 == 0 ? "\n" : "   ") << p << ":" << r.cond[p - 1];
		}
	}
	return out.str();
}

struct StoredResults {
	std::vector<ClickResult> results;
	int maxOffset = 12;
	bool hideClicks = false;    // global switch: hide every click marker
	bool hideReleases = false;  // global switch: hide every release marker

	bool visible(ClickResult const& r) const {
		if (!r.baselineOk || !r.hasPos || r.hidden) return false;
		return !(r.release ? hideReleases : hideClicks);
	}
};

// How many inputs fall in each window bucket. Only inputs whose marker is currently visible are counted,
// so switching rings off in the Markers list also changes the tally.
inline std::array<int, kBucketCount> tally(StoredResults const& store) {
	std::array<int, kBucketCount> counts{};
	for (auto const& r : store.results) {
		if (!store.visible(r)) continue;
		counts[windowBucket(r.window())]++;
	}
	return counts;
}

inline std::string resultsToJson(StoredResults const& store) {
	std::vector<matjson::Value> arr;
	for (auto const& r : store.results) {
		if (!r.baselineOk || !r.hasPos) continue;
		matjson::Value o;
		o["frame"] = r.frame;
		o["x"] = r.x;
		o["y"] = r.y;
		o["early"] = r.early;
		o["late"] = r.late;
		o["window"] = r.window();
		o["capped"] = r.capped();
		o["p2"] = r.p2;
		o["release"] = r.release;
		o["hidden"] = r.hidden;
		if (!r.cond.empty()) {
			std::vector<matjson::Value> cv;
			for (int c : r.cond) cv.push_back(matjson::Value(c));
			o["cond"] = std::move(cv);
			o["next-frame"] = r.condNextFrame;
		}
		arr.push_back(std::move(o));
	}
	matjson::Value root;
	root["hz"] = 240;
	root["max-offset"] = store.maxOffset;
	root["hide-clicks"] = store.hideClicks;
	root["hide-releases"] = store.hideReleases;
	root["clicks"] = std::move(arr);
	return root.dump();
}

inline bool resultsFromJson(std::string const& text, StoredResults& out) {
	auto parsed = matjson::Value::parse(text);
	if (parsed.isErr()) return false;
	auto root = std::move(parsed).unwrap();
	auto arr = root["clicks"].asArray();
	if (arr.isErr()) return false;
	out = StoredResults{};
	out.maxOffset = static_cast<int>(root["max-offset"].asInt().unwrapOr(12));
	out.hideClicks = root["hide-clicks"].asBool().unwrapOr(false);
	out.hideReleases = root["hide-releases"].asBool().unwrapOr(false);
	for (auto const& o : arr.unwrap()) {
		ClickResult r;
		r.frame = o["frame"].asInt().unwrapOr(0);
		r.x = static_cast<float>(o["x"].asDouble().unwrapOr(0.0));
		r.y = static_cast<float>(o["y"].asDouble().unwrapOr(0.0));
		r.early = static_cast<int>(o["early"].asInt().unwrapOr(0));
		r.late = static_cast<int>(o["late"].asInt().unwrapOr(0));
		r.cappedLate = o["capped"].asBool().unwrapOr(false);
		r.p2 = o["p2"].asBool().unwrapOr(false);
		r.release = o["release"].asBool().unwrapOr(false);
		r.hidden = o["hidden"].asBool().unwrapOr(false);
		r.condNextFrame = o["next-frame"].asInt().unwrapOr(0);
		if (auto cv = o["cond"].asArray(); cv.isOk()) {
			for (auto const& c : cv.unwrap()) r.cond.push_back(static_cast<int>(c.asInt().unwrapOr(0)));
		}
		r.baselineOk = true;
		r.hasPos = true;
		out.results.push_back(r);
	}
	return true;
}

inline std::string summaryMarkdown(std::vector<ClickResult> const& rs, double srcHz, int maxOffset) {
	std::ostringstream out;
	size_t okClicks = 0, okReleases = 0, bad = 0;
	std::map<int, size_t> clickHist, releaseHist;
	for (auto const& r : rs) {
		if (!r.baselineOk) { bad++; continue; }
		if (r.release) { okReleases++; releaseHist[r.window()]++; }
		else { okClicks++; clickHist[r.window()]++; }
	}
	out << "**Clicks analyzed:** " << okClicks << "  \n";
	out << "**Releases analyzed:** " << okReleases << "  \n";
	if (bad) out << "**Inputs that didn't reproduce:** " << bad << "  \n";
	out << "**Resolution:** 240 Hz (1 frame = 4.17 ms)";
	if (srcHz != 240.0) out << "  \n*Macro was recorded at " << srcHz << " Hz and converted.*";
	out << "\n\n";

	auto histogram = [&](char const* title, std::map<int, size_t> const& hist) {
		if (hist.empty()) return;
		out << "## " << title << "\n";
		for (auto const& [w, n] : hist) {
			bool cap = w >= 2 * maxOffset + 1;
			out << "- **" << w << (cap ? "+" : "") << "** frame" << (w == 1 ? "" : "s") << ": " << n << (n == 1 ? " input" : " inputs") << "\n";
		}
		out << "\n";
	};
	histogram("Click windows", clickHist);
	histogram("Release windows", releaseHist);

	std::vector<ClickResult const*> tight;
	for (auto const& r : rs) if (r.baselineOk) tight.push_back(&r);
	std::stable_sort(tight.begin(), tight.end(), [](auto a, auto b) { return a->window() < b->window(); });
	if (!tight.empty()) {
		out << "## Tightest inputs\n";
		size_t n = std::min<size_t>(tight.size(), 15);
		for (size_t i = 0; i < n; i++) {
			auto const& r = *tight[i];
			out << "- " << (r.release ? "release" : "click") << " at frame **" << r.frame << "**: window **" << r.window()
				<< (r.capped() ? "+" : "") << "** (" << r.early << " early / " << r.late << " late)\n";
		}
	}
	out << "\n*Window = frames you can move this input earlier or later and still survive to the next section. "
		   "Releases are tested with the click fixed. Capped values (+) hit the max offset setting.*\n\n"
		   "**To watch it:** pause, press **Play Macro**. Use **Markers** in the pause menu to switch circles off.\n";
	return out.str();
}

} // namespace cw
