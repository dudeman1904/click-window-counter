#include "../src/Replay.hpp"
#include "../src/Store.hpp"
#include <cassert>
#include <iostream>
int main() {
	// gdr2 round trip with the real library: write a 60 Hz replay, load, convert to 240 Hz
	gdr::Replay<> rep("TestBot", 1);
	rep.framerate = 60.0;
	rep.levelInfo.name = "Test";
	rep.author = "me";
	rep.inputs.push_back(gdr::Input<>(10, 1, false, true));
	rep.inputs.push_back(gdr::Input<>(12, 1, false, false));
	rep.inputs.push_back(gdr::Input<>(30, 1, true, true));
	rep.inputs.push_back(gdr::Input<>(33, 1, true, false));
	auto path = std::filesystem::path("/tmp/t.gdr2");
	auto w = rep.exportData(path);
	assert(w.isOk());
	cw::LoadedMacro m; std::string err;
	assert(cw::loadMacro(path, m, err));
	assert(m.events.size() == 4 && m.sourceFramerate == 60.0);
	assert(m.events[0].frame == 40 && m.events[1].frame == 48 && m.events[2].p2 && m.events[2].frame == 120);
	std::cout << "gdr2 ok: " << m.events.size() << " events, frames " << m.events[0].frame << "," << m.events[1].frame << "\n";
	assert(!cw::loadMacro("/tmp/nonexistent.gdr2", m, err)); std::cout << "missing file err: " << err << "\n";
	{ std::ofstream f("/tmp/bad.gdr2", std::ios::binary); f << "garbage not a replay"; }
	bool ok = cw::loadMacro("/tmp/bad.gdr2", m, err); std::cout << "garbage -> ok=" << ok << " err=" << err << "\n";

	// store round trip (with hide flags + per-input hidden)
	cw::StoredResults st;
	st.maxOffset = 12; st.hideReleases = true;
	st.results.resize(3);
	auto& rs = st.results;
	rs[0].frame = 100; rs[0].baselineOk = true; rs[0].hasPos = true; rs[0].x = 1234.5f; rs[0].y = 90; rs[0].early = 1; rs[0].late = 2;
	rs[1].frame = 200; rs[1].baselineOk = true; rs[1].hasPos = true; rs[1].early = 12; rs[1].late = 12; rs[1].cappedEarly = true; rs[1].cappedLate = true; rs[1].hidden = true;
	rs[2].frame = 110; rs[2].release = true; rs[2].baselineOk = true; rs[2].hasPos = true; rs[2].early = 2; rs[2].late = 3;
	auto js = cw::resultsToJson(st);
	cw::StoredResults back;
	assert(cw::resultsFromJson(js, back) && back.results.size() == 3);
	assert(back.results[0].window() == 4 && back.results[0].x == 1234.5f);
	assert(back.results[1].window() == 25 && back.results[1].capped() && back.results[1].hidden);
	assert(back.results[2].release && back.results[2].window() == 6);
	assert(back.hideReleases && !back.hideClicks && back.maxOffset == 12);
	// visibility rules: hidden individually, or whole kind hidden
	assert(back.visible(back.results[0]));
	assert(!back.visible(back.results[1]));   // hidden marker
	assert(!back.visible(back.results[2]));   // releases switched off globally
	back.hideReleases = false;
	assert(back.visible(back.results[2]));
	back.hideClicks = true;
	assert(!back.visible(back.results[0]));
	cw::StoredResults junk;
	assert(!cw::resultsFromJson("nope", junk));
	// old files without the hide fields still load
	assert(cw::resultsFromJson(R"({"clicks":[{"frame":5,"x":1,"y":2,"early":0,"late":0}]})", junk) && junk.results.size() == 1 && !junk.results[0].hidden);

	// colour scale matches the reference chart
	auto eq = [](cw::Rgb a, cw::Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
	assert(eq(cw::windowColor(4), {255, 255, 255}));          // 4 is white
	assert(eq(cw::windowColor(1), cw::windowColor(0)));        // 1 (and below) red
	assert(!eq(cw::windowColor(1), cw::windowColor(2)));
	assert(eq(cw::windowColor(5), cw::windowColor(6)) && !eq(cw::windowColor(4), cw::windowColor(5)));   // 5-6 green
	assert(eq(cw::windowColor(7), cw::windowColor(8)) && !eq(cw::windowColor(6), cw::windowColor(7)));   // 7-8 light blue
	assert(eq(cw::windowColor(9), cw::windowColor(12)) && !eq(cw::windowColor(8), cw::windowColor(9)));  // 9-12 blue (the "11" in the screenshot)
	assert(eq(cw::windowColor(13), cw::windowColor(14)) && !eq(cw::windowColor(12), cw::windowColor(13)));   // 13-14 purple
	assert(eq(cw::windowColor(15), cw::windowColor(40)) && !eq(cw::windowColor(14), cw::windowColor(15)));   // 15+ pink
	assert(cw::windowColor(11).b > cw::windowColor(11).r);     // 11 is blue
	// buckets: 1 | 2 | 3 | 4 | 5-6 | 7-8 | 9-12 | 13-14 | 15+
	{
		int expect[][2] = {{0,0},{1,0},{2,1},{3,2},{4,3},{5,4},{6,4},{7,5},{8,5},{9,6},{12,6},{13,7},{14,7},{15,8},{16,8},{25,8},{99,8}};
		for (auto const& e : expect) assert(cw::windowBucket(e[0]) == e[1]);
		assert(std::string(cw::kBucketLabels[0]) == "1" && std::string(cw::kBucketLabels[8]) == "15+" && std::string(cw::kBucketLabels[6]) == "9-12");
		// every bucket has its own colour
		for (int i = 0; i < cw::kBucketCount; i++)
			for (int j = i + 1; j < cw::kBucketCount; j++) assert(!eq(cw::bucketColor(i), cw::bucketColor(j)));
	}

	// tally panel counts (matches the reference: "9-12: 1, 7-8: 1, 5-6: 6, 4: 1, 3: 1, 2: 0, 1: 0")
	{
		cw::StoredResults t;
		int windows[] = {10, 8, 5, 5, 6, 5, 6, 6, 4, 3};
		for (int w : windows) {
			cw::ClickResult r; r.baselineOk = true; r.hasPos = true; r.early = 0; r.late = w - 1; t.results.push_back(r);
		}
		auto c = cw::tally(t);
		assert(c[6] == 1 && c[5] == 1 && c[4] == 6 && c[3] == 1 && c[2] == 1 && c[1] == 0 && c[0] == 0 && c[7] == 0 && c[8] == 0);
		t.results[0].hidden = true;                 // switching a ring off removes it from the tally
		assert(cw::tally(t)[6] == 0);
		cw::ClickResult rel; rel.baselineOk = true; rel.hasPos = true; rel.release = true; rel.late = 14; t.results.push_back(rel);
		assert(cw::tally(t)[8] == 1);
		t.hideReleases = true;                      // hiding all releases removes them too
		assert(cw::tally(t)[8] == 0);
		cw::ClickResult bad; bad.baselineOk = false; bad.hasPos = true; t.results.push_back(bad);   // unreproduced inputs are never counted
		int total = 0; for (int n : cw::tally(t)) total += n;
		assert(total == 9);
		std::cout << "tally ok\n";
	}
	std::cout << "colour scale ok\n";

	// conditional text + saving the tables
	{
		cw::ClickResult r;
		r.baselineOk = true; r.hasPos = true; r.frame = 100; r.early = 2; r.late = 3;
		r.cond = {3, 3, 3, 3, 1, 3};
		r.condNextFrame = 200;
		auto t = cw::conditionText(r);
		std::cout << t << "\n";
		assert(t == "click window 6 (you: frame 3)\nif on frame 1-4, 6: next click is 3\nif on frame 5: next click is 1");
		r.cond = {4, 4, 4};
		assert(cw::conditionText(r) == "click window 3 (you: frame 3)\nnext click is 4 whatever frame");
		r.cond = {3, 0, 3};
		assert(cw::conditionText(r).find("if on frame 2: next click is impossible") != std::string::npos);
		r.cond = {5, 6, 7, 8, 9, 10, 11, 12};   // many different values -> compact table
		auto c = cw::conditionText(r);
		assert(c.find("frame: next click\n1:5   2:6   3:7   4:8   5:9   6:10\n7:11   8:12") != std::string::npos);
		r.cond.clear();
		assert(cw::conditionText(r).empty());
		assert(cw::formatPositions({1, 2, 3, 4, 6}) == "1-4, 6" && cw::formatPositions({2, 3}) == "2, 3" && cw::formatPositions({5}) == "5");

		cw::StoredResults st;
		st.results.push_back(r);
		st.results[0].cond = {3, 3, 3, 3, 1, 3};
		auto back = cw::StoredResults{};
		assert(cw::resultsFromJson(cw::resultsToJson(st), back));
		assert(back.results[0].cond == st.results[0].cond && back.results[0].condNextFrame == 200);
		std::cout << "condition text ok\n";
	}

	// ring geometry: default click = 20px radius, default release (0.65) = the old 13px, everything scales together
	{
		auto c = cw::ringGeometry(1.0f);
		assert(c.radius == 20.f && c.border == 4.f && c.textScale > 0.449f && c.textScale < 0.451f);
		auto r = cw::ringGeometry(0.65f);
		assert(r.radius > 12.99f && r.radius < 13.01f);
		auto big = cw::ringGeometry(2.0f);
		assert(big.radius == 40.f && big.border == 8.f && big.textOffset > c.textOffset * 1.99f && big.textOffset < c.textOffset * 2.01f);
		auto tiny = cw::ringGeometry(0.3f);
		assert(tiny.border >= 1.5f && tiny.outline > tiny.border);     // never vanishes, outline stays visible
		assert(cw::ringGeometry(-5.f).radius > 0.f && cw::ringGeometry(1000.f).radius <= 120.f);  // clamped
		std::cout << "ring geometry ok\n";
	}

	std::cout << cw::summaryMarkdown(st.results, 60.0, 12) << "\nSTORE OK\n";
}
