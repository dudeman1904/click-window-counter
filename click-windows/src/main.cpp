#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/MDPopup.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <array>
#include <memory>

#include "Replay.hpp"
#include "Searcher.hpp"
#include "Store.hpp"

using namespace geode::prelude;

namespace {

enum class Mode { Idle, Analyzing, Playback };

struct Session {
	Mode mode = Mode::Idle;
	std::unique_ptr<cw::Searcher> searcher;
	CheckpointObject* checkpoint = nullptr; // manually retained
	bool pendingLoad = false;
	bool finishRequested = false;
	float savedTimeScale = 1.f;
	int updateCounter = 0;

	cw::LoadedMacro macro;
	bool hasMacro = false;
	cw::StoredResults store; // analysis results + which markers are switched off

	// top-left conditions text
	std::vector<size_t> condIdx; // indices into store.results of clicks that have a next-click table
	int shownCond = -2;          // which one is on screen (-1 none, -2 force refresh)

	// tally panel (so the conditions text can sit below it if both are in the same corner)
	float statsPanelH = 0.f;
	bool statsRight = false;

	// playback cursor
	size_t pbIdx = 0;
	std::array<bool, 8> pbHeld{};
};

Session& S() {
	static Session s;
	return s;
}

constexpr auto kMarkersID = "click-window-markers"_spr;
constexpr auto kLabelID = "click-window-progress"_spr;
constexpr auto kCondID = "click-window-conditions"_spr;
constexpr auto kStatsID = "click-window-stats"_spr;

bool cornerIsRight(char const* settingKey) {
	return Mod::get()->getSettingValue<std::string>(settingKey) == "Top right";
}

void setCheckpoint(CheckpointObject* cp) {
	auto& s = S();
	if (cp) cp->retain();
	if (s.checkpoint) s.checkpoint->release();
	s.checkpoint = cp;
}

cw::SearchSettings readSettings() {
	auto* m = Mod::get();
	cw::SearchSettings c;
	c.maxOffset = m->getSettingValue<int>("max-offset");
	c.tailFrames = m->getSettingValue<int>("tail-frames");
	c.maxHorizon = m->getSettingValue<int>("max-horizon");
	c.shiftRelease = m->getSettingValue<bool>("shift-release");
	c.onlyJump = m->getSettingValue<bool>("only-jump");
	c.conditional = m->getSettingValue<bool>("conditional-windows");
	c.freeRelease = m->getSettingValue<bool>("free-release");
	c.freeReleaseRange = m->getSettingValue<int>("free-release-range");
	auto what = m->getSettingValue<std::string>("analyze");
	c.analyzeClicks = what != "Releases";
	c.analyzeReleases = what != "Clicks";
	return c;
}

std::string levelKey(PlayLayer* pl) {
	int id = pl->m_level->m_levelID.value();
	if (id != 0) return std::to_string(id);
	std::string name = pl->m_level->m_levelName;
	for (auto& ch : name) {
		if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
	}
	return "local-" + name;
}

std::filesystem::path resultsPath(PlayLayer* pl) {
	return Mod::get()->getSaveDir() / fmt::format("windows-{}.json", levelKey(pl));
}

void saveStore(PlayLayer* pl) {
	auto written = file::writeString(resultsPath(pl), cw::resultsToJson(S().store));
	if (written.isErr()) log::warn("Couldn't save results: {}", written.unwrapErr());
}

// ---------- markers ----------

ccColor3B colorFor(int window) {
	auto c = cw::windowColor(window);
	return {c.r, c.g, c.b};
}

// A thick coloured ring (like the reference screenshot) with the window number beside it.
// Clicks: number on the left. Releases: number on the right. Sizes come from the settings.
CCNode* makeMarker(cw::ClickResult const& r) {
	auto col = colorFor(r.window());
	auto marker = CCNode::create();
	marker->setPosition({r.x, r.y});

	float size = static_cast<float>(Mod::get()->getSettingValue<double>(r.release ? "release-ring-size" : "click-ring-size"));
	auto g = cw::ringGeometry(size);

	auto ring = CCDrawNode::create();
	// dark outline underneath, then the coloured ring on top
	ring->drawCircle({0.f, 0.f}, g.radius, ccc4f(0.f, 0.f, 0.f, 0.f), g.outline, ccc4f(0.f, 0.f, 0.f, 0.85f), 40);
	ring->drawCircle({0.f, 0.f}, g.radius, ccc4f(0.f, 0.f, 0.f, 0.f), g.border,
		ccc4f(col.r / 255.f, col.g / 255.f, col.b / 255.f, 1.f), 40);
	marker->addChild(ring);

	auto text = fmt::format("{}{}", r.window(), r.capped() ? "+" : "");
	auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
	label->setScale(g.textScale);
	label->setColor(col);
	if (r.release) {
		label->setAnchorPoint({0.f, 0.5f});
		label->setPosition({g.textOffset, g.textDropY});
	} else {
		label->setAnchorPoint({1.f, 0.5f});
		label->setPosition({-g.textOffset, g.textDropY});
	}
	marker->addChild(label);
	return marker;
}

// ---------- top-left conditions text ----------

void rebuildCondIndex() {
	auto& s = S();
	s.condIdx.clear();
	for (size_t i = 0; i < s.store.results.size(); i++) {
		auto const& r = s.store.results[i];
		if (!r.release && r.baselineOk && !r.cond.empty()) s.condIdx.push_back(i);
	}
	s.shownCond = -2;
}

void setCondLabel(PlayLayer* pl, std::string const& text) {
	auto* parent = static_cast<CCNode*>(pl->m_uiLayer);
	auto* label = static_cast<CCLabelBMFont*>(parent->getChildByID(kCondID));
	if (text.empty()) {
		if (label) label->setVisible(false);
		return;
	}
	if (!label) {
		label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
		label->setID(kCondID);
		label->setOpacity(230);
		parent->addChild(label, 9998);
	}
	// corner can be changed in the settings at any time, so (re)apply it every time the text is set
	bool right = cornerIsRight("conditions-corner");
	auto win = CCDirector::get()->getWinSize();
	float top = win.height - 8.f;
	auto& s = S();
	if (s.statsPanelH > 0.f && right == s.statsRight) top -= s.statsPanelH + 8.f; // sit below the tally panel
	label->setAnchorPoint({right ? 1.f : 0.f, 1.f});
	label->setAlignment(right ? kCCTextAlignmentRight : kCCTextAlignmentLeft);
	label->setPosition({right ? win.width - 8.f : 8.f, top});
	label->setString(text.c_str());
	label->setScale(static_cast<float>(Mod::get()->getSettingValue<double>("condition-text-size")));
	label->setVisible(true);
}

// Shows the table of the upcoming click (from 40 frames before it until 25 frames after it).
void updateConditionLabel(PlayLayer* pl, int64_t frame) {
	auto& s = S();
	if (s.mode == Mode::Analyzing || !Mod::get()->getSettingValue<bool>("show-conditions")) {
		if (s.shownCond != -1) {
			setCondLabel(pl, "");
			s.shownCond = -1;
		}
		return;
	}
	auto const& rs = s.store.results;
	int idx = -1;
	auto it = std::lower_bound(s.condIdx.begin(), s.condIdx.end(), frame - 25,
		[&](size_t i, int64_t f) { return rs[i].frame < f; });
	if (it != s.condIdx.end() && rs[*it].frame - 40 <= frame) idx = static_cast<int>(*it);
	if (idx == s.shownCond) return;
	s.shownCond = idx;
	// a click whose ring is switched off doesn't show its text either
	setCondLabel(pl, idx >= 0 && s.store.visible(rs[idx]) ? cw::conditionText(rs[idx]) : std::string());
}

// ---------- window tally panel ----------

// One row per window bucket, biggest on top, "label: count" in that bucket's marker colour.
void rebuildStatsPanel(PlayLayer* pl) {
	auto& s = S();
	auto* parent = static_cast<CCNode*>(pl->m_uiLayer);
	if (auto* old = parent->getChildByID(kStatsID)) old->removeFromParent();
	s.statsPanelH = 0.f;
	if (s.mode == Mode::Analyzing || s.store.results.empty() || !Mod::get()->getSettingValue<bool>("show-stats")) return;

	auto counts = cw::tally(s.store);
	float scale = static_cast<float>(Mod::get()->getSettingValue<double>("stats-size"));
	float rowH = 30.f * scale;

	auto root = CCNode::create();
	root->setID(kStatsID);

	std::vector<std::pair<CCLabelBMFont*, CCLabelBMFont*>> rows;
	float maxName = 0.f, maxNum = 0.f;
	for (int row = 0; row < cw::kBucketCount; row++) {
		int bucket = cw::kBucketCount - 1 - row;
		auto c = cw::bucketColor(bucket);
		ccColor3B col = {c.r, c.g, c.b};

		auto name = CCLabelBMFont::create((std::string(cw::kBucketLabels[bucket]) + ":").c_str(), "bigFont.fnt");
		auto num = CCLabelBMFont::create(std::to_string(counts[bucket]).c_str(), "bigFont.fnt");
		for (auto* l : {name, num}) {
			l->setScale(scale);
			l->setColor(col);
			l->setAnchorPoint({0.f, .5f});
			root->addChild(l);
		}
		float y = -(static_cast<float>(row) + .5f) * rowH;
		name->setPosition({0.f, y});
		num->setPositionY(y);
		maxName = std::max(maxName, name->getContentSize().width * scale);
		maxNum = std::max(maxNum, num->getContentSize().width * scale);
		rows.push_back({name, num});
	}
	float numX = maxName + 10.f * scale; // numbers line up in one column after the widest label
	for (auto& r : rows) r.second->setPositionX(numX);
	float totalW = numX + maxNum;

	bool right = cornerIsRight("stats-corner");
	auto win = CCDirector::get()->getWinSize();
	root->setPosition({right ? win.width - 8.f - totalW : 8.f, win.height - 8.f});
	s.statsPanelH = rowH * static_cast<float>(cw::kBucketCount);
	s.statsRight = right;
	parent->addChild(root, 9997);
}

void addMarkers(PlayLayer* pl) {
	rebuildCondIndex();
	rebuildStatsPanel(pl);
	if (auto old = pl->m_objectLayer->getChildByID(kMarkersID)) old->removeFromParent();
	if (!Mod::get()->getSettingValue<bool>("show-markers")) return;
	auto& store = S().store;
	if (store.results.empty()) return;

	auto root = CCNode::create();
	root->setID(kMarkersID);
	for (auto const& r : store.results) {
		if (!store.visible(r)) continue;
		root->addChild(makeMarker(r));
	}
	pl->m_objectLayer->addChild(root, 2000);
}

void refreshMarkers() {
	if (auto* pl = PlayLayer::get()) addMarkers(pl);
}

void persistAndRefresh() {
	if (auto* pl = PlayLayer::get()) {
		saveStore(pl);
		addMarkers(pl);
	}
}

void loadStoredMarkers(PlayLayer* pl) {
	auto path = resultsPath(pl);
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return;
	auto text = file::readString(path);
	if (text.isErr()) return;
	cw::StoredResults loaded;
	if (!cw::resultsFromJson(text.unwrap(), loaded)) return;
	S().store = std::move(loaded);
	addMarkers(pl);
	log::info("Loaded {} analyzed inputs for this level", S().store.results.size());
}

// ---------- analysis control ----------

void setProgressLabel(PlayLayer* pl, std::string const& text) {
	auto* parent = static_cast<CCNode*>(pl->m_uiLayer);
	auto* label = static_cast<CCLabelBMFont*>(parent->getChildByID(kLabelID));
	if (!label) {
		label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
		label->setID(kLabelID);
		label->setScale(0.45f);
		auto win = CCDirector::get()->getWinSize();
		label->setPosition({win.width / 2.f, win.height - 18.f});
		parent->addChild(label, 9999);
	}
	label->setString(text.c_str());
}

void removeProgressLabel(PlayLayer* pl) {
	if (!pl) return;
	if (auto* l = static_cast<CCNode*>(pl->m_uiLayer)->getChildByID(kLabelID)) l->removeFromParent();
}

// Leaves any mode and restores everything we touched. Safe to call at any time.
void endSession() {
	auto& s = S();
	if (s.mode == Mode::Analyzing) {
		CCDirector::get()->getScheduler()->setTimeScale(s.savedTimeScale);
	}
	s.mode = Mode::Idle;
	s.pendingLoad = false;
	s.finishRequested = false;
	s.searcher.reset();
	setCheckpoint(nullptr);
}

void finishAnalysis() {
	auto& s = S();
	auto* pl = PlayLayer::get();
	auto searcher = std::move(s.searcher);
	endSession();
	if (!pl || !searcher) return;
	removeProgressLabel(pl);

	if (searcher->aborted()) {
		FLAlertLayer::create(
			"Analysis stopped",
			searcher->error() +
				"\n\nMake sure the macro is for this level, the game runs at 240 TPS, and the level was started from the beginning.",
			"OK"
		)->show();
		pl->resetLevelFromStart();
		return;
	}

	// keep the user's global hide switches, but start with every individual marker visible
	s.store.results = searcher->results();

	saveStore(pl);

	for (auto const& r : s.store.results) {
		log::info("{} @{}: baselineOk={} early={} late={} window={}{}", r.release ? "release" : "click", r.frame,
			r.baselineOk, r.early, r.late, r.window(), r.capped() ? "+" : "");
	}

	pl->resetLevelFromStart();
	addMarkers(pl);
	MDPopup::create("Frame Windows",
		cw::summaryMarkdown(s.store.results, s.macro.sourceFramerate, s.store.maxOffset), "OK")->show();
}

void startAnalysis(cw::LoadedMacro macro, PauseLayer* pause) {
	auto* pl = PlayLayer::get();
	if (!pl) return;
	auto& s = S();
	endSession();

	auto cfg = readSettings();
	s.macro = std::move(macro);
	s.hasMacro = true;
	s.store.maxOffset = cfg.maxOffset;
	s.searcher = std::make_unique<cw::Searcher>(s.macro.events, cfg);
	if (s.searcher->targetCount() == 0) {
		s.searcher.reset();
		FLAlertLayer::create("Nothing to analyze", "This macro has no inputs of the kind selected in the settings.", "OK")->show();
		return;
	}
	log::info("Analyzing {} inputs ({} raw inputs, source {} Hz)", s.searcher->targetCount(), s.macro.events.size(),
		s.macro.sourceFramerate);

	if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
	pl->resetLevelFromStart();

	s.mode = Mode::Analyzing;
	s.updateCounter = 0;
	rebuildStatsPanel(pl); // removes the panel while analyzing
	auto* sched = CCDirector::get()->getScheduler();
	s.savedTimeScale = sched->getTimeScale();
	sched->setTimeScale(static_cast<float>(Mod::get()->getSettingValue<double>("analysis-speed")));

	pause->onResume(nullptr);
	setProgressLabel(pl, "Analyzing...");
}

void syncPlaybackCursor(int64_t frame) {
	auto& s = S();
	auto const& ev = s.macro.events;
	s.pbHeld.fill(false);
	s.pbIdx = 0;
	while (s.pbIdx < ev.size() && ev[s.pbIdx].frame < frame) {
		auto const& e = ev[s.pbIdx++];
		s.pbHeld[(e.button & 3) * 2 + (e.p2 ? 1 : 0)] = e.down;
	}
}

// Replays the macro from the start of the level; the markers stay visible while it plays.
void beginPlayback(PauseLayer* pause) {
	auto* pl = PlayLayer::get();
	if (!pl) return;
	auto& s = S();
	endSession();
	s.mode = Mode::Playback;
	pl->resetLevelFromStart(); // the resetLevel hook syncs the playback cursor
	addMarkers(pl);
	pause->onResume(nullptr);
}

// Opens the file picker, then either analyzes or plays the chosen macro.
void pickMacroAndRun(PauseLayer* pause, bool analyze) {
	async::spawn(
		file::pick(file::PickMode::OpenFile, file::FilePickOptions {
			.filters = { file::FilePickOptions::Filter {
				.description = "GDR2 macro",
				.files = { "*.gdr2" },
			}}
		}),
		[self = Ref(pause), analyze](file::PickResult result) {
			if (result.isErr()) {
				FLAlertLayer::create("Unable to select file", result.unwrapErr(), "OK")->show();
				return;
			}
			auto picked = std::move(result).unwrap();
			if (!picked) return; // cancelled

			cw::LoadedMacro macro;
			std::string err;
			if (!cw::loadMacro(*picked, macro, err)) {
				FLAlertLayer::create("Macro error", err, "OK")->show();
				return;
			}
			if (analyze) {
				startAnalysis(std::move(macro), self);
			} else {
				auto& s = S();
				s.macro = std::move(macro);
				s.hasMacro = true;
				beginPlayback(self);
			}
		}
	);
}

// ---------- markers popup: switch individual circles on/off ----------

class MarkersPopup : public Popup {
protected:
	bool init() {
		if (!Popup::init(340.f, 260.f)) return false;
		this->setTitle("Frame Window Markers");

		auto& store = S().store;

		// global switches
		auto clicksToggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(MarkersPopup::onToggleClicks), .6f);
		clicksToggle->toggle(!store.hideClicks);
		m_buttonMenu->addChildAtPosition(clicksToggle, Anchor::Top, ccp(-120.f, -50.f));
		auto clicksLabel = CCLabelBMFont::create("All clicks", "bigFont.fnt");
		clicksLabel->setScale(.4f);
		m_buttonMenu->addChildAtPosition(clicksLabel, Anchor::Top, ccp(-102.f, -50.f), ccp(0.f, .5f));

		auto releasesToggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(MarkersPopup::onToggleReleases), .6f);
		releasesToggle->toggle(!store.hideReleases);
		m_buttonMenu->addChildAtPosition(releasesToggle, Anchor::Top, ccp(25.f, -50.f));
		auto releasesLabel = CCLabelBMFont::create("All releases", "bigFont.fnt");
		releasesLabel->setScale(.4f);
		m_buttonMenu->addChildAtPosition(releasesLabel, Anchor::Top, ccp(43.f, -50.f), ccp(0.f, .5f));

		// per-input list
		CCSize listSize { 300.f, 150.f };
		auto container = CCNode::create();
		container->setContentSize(listSize);

		auto bg = NineSlice::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
		bg->setScale(.5f);
		bg->setColor({0, 0, 0});
		bg->setOpacity(75);
		bg->setContentSize(listSize * 2);
		bg->setPosition(listSize / 2);
		container->addChild(bg);

		auto scroll = ScrollLayer::create(listSize);
		scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
		for (size_t i = 0; i < store.results.size(); i++) {
			auto const& r = store.results[i];
			auto row = CCMenu::create();
			row->setContentSize({ listSize.width, 26.f });

			auto toggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(MarkersPopup::onToggleInput), .6f);
			toggle->setTag(static_cast<int>(i));
			toggle->toggle(!r.hidden);
			row->addChildAtPosition(toggle, Anchor::Left, ccp(20.f, 0.f));

			auto text = fmt::format("{} @ {}  -  {}{}", r.release ? "Release" : "Click", r.frame, r.window(), r.capped() ? "+" : "");
			auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
			label->setScale(.4f);
			label->setColor(colorFor(r.window()));
			row->addChildAtPosition(label, Anchor::Left, ccp(42.f, 0.f), ccp(0.f, .5f));

			scroll->m_contentLayer->addChild(row);
		}
		scroll->m_contentLayer->updateLayout();
		scroll->scrollToTop();
		container->addChild(scroll);

		m_mainLayer->addChildAtPosition(container, Anchor::Bottom, ccp(0.f, 18.f), ccp(.5f, 0.f));
		return true;
	}

	// The toggler flips itself; we flip our own copy of the state so both always agree.
	void onToggleInput(CCObject* sender) {
		size_t i = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
		auto& rs = S().store.results;
		if (i >= rs.size()) return;
		rs[i].hidden = !rs[i].hidden;
		persistAndRefresh();
	}

	void onToggleClicks(CCObject*) {
		S().store.hideClicks = !S().store.hideClicks;
		persistAndRefresh();
	}

	void onToggleReleases(CCObject*) {
		S().store.hideReleases = !S().store.hideReleases;
		persistAndRefresh();
	}

public:
	static MarkersPopup* create() {
		auto ret = new MarkersPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

} // namespace

// redraw the rings as soon as a size setting is changed
$on_mod(Loaded) {
	listenForSettingChanges<double>("click-ring-size", [](double) { refreshMarkers(); });
	listenForSettingChanges<double>("release-ring-size", [](double) { refreshMarkers(); });
	listenForSettingChanges<bool>("show-markers", [](bool) { refreshMarkers(); });
	listenForSettingChanges<bool>("show-stats", [](bool) { refreshMarkers(); });
	listenForSettingChanges<double>("stats-size", [](double) { refreshMarkers(); });
	listenForSettingChanges<std::string>("stats-corner", [](std::string) { refreshMarkers(); });
	listenForSettingChanges<double>("condition-text-size", [](double) { S().shownCond = -2; });
	listenForSettingChanges<bool>("show-conditions", [](bool) { S().shownCond = -2; });
	listenForSettingChanges<std::string>("conditions-corner", [](std::string) { S().shownCond = -2; });
}

// ---------- hooks ----------

class $modify(CWBaseLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto* pl = PlayLayer::get();
		auto& s = S();
		bool ours = pl && static_cast<GJBaseGameLayer*>(pl) == this;
		if (!ours || s.mode == Mode::Idle || isHalfTick) {
			return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		}

		int64_t frame = m_gameState.m_currentProgress;

		if (s.mode == Mode::Analyzing) {
			// no stepping while waiting for a checkpoint load or after finishing
			if (s.pendingLoad || s.finishRequested || !s.searcher) return;

			auto cmd = s.searcher->beforeStep(frame, m_player1->getPositionX(), m_player1->getPositionY());
			if (cmd.saveCheckpoint) setCheckpoint(pl->createCheckpoint());
			if (cmd.loadCheckpoint) s.pendingLoad = true;
			if (cmd.done) s.finishRequested = true;
			if (cmd.skipStep || cmd.done) return;

			for (auto const& e : cmd.inputs) this->handleButton(e.down, e.button, !e.p2);
		}
		else if (s.mode == Mode::Playback) {
			auto const& ev = s.macro.events;
			while (s.pbIdx < ev.size() && ev[s.pbIdx].frame <= frame) {
				auto const& e = ev[s.pbIdx++];
				int k = (e.button & 3) * 2 + (e.p2 ? 1 : 0);
				if (s.pbHeld[k] == e.down) continue;
				s.pbHeld[k] = e.down;
				this->handleButton(e.down, e.button, !e.p2);
			}
		}

		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	void update(float dt) {
		auto* pl = PlayLayer::get();
		auto& s = S();
		bool analyzing = pl && static_cast<GJBaseGameLayer*>(pl) == this && s.mode == Mode::Analyzing && s.searcher;

		if (analyzing) {
			if (s.finishRequested) {
				// tear down on the next main-thread tick instead of inside the update loop
				if (!s.pendingLoad) {
					s.pendingLoad = true; // also blocks stepping and prevents queueing twice
					queueInMainThread([] { finishAnalysis(); });
				}
			}
			else if (s.pendingLoad && s.checkpoint) {
				pl->loadFromCheckpoint(s.checkpoint);
				auto cpFrame = s.searcher->checkpointFrame();
				if (static_cast<int64_t>(m_gameState.m_currentProgress) != cpFrame) {
					log::debug("Frame counter after checkpoint load was {}, forcing {}", m_gameState.m_currentProgress, cpFrame);
					m_gameState.m_currentProgress = static_cast<unsigned>(cpFrame);
				}
				s.pendingLoad = false;
				s.searcher->onLoaded(cpFrame);
			}
		}

		GJBaseGameLayer::update(dt);

		if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
			updateConditionLabel(pl, static_cast<int64_t>(m_gameState.m_currentProgress));
		}

		if (analyzing && s.searcher && !s.finishRequested && (++s.updateCounter % 15) == 0) {
			auto text = fmt::format("Analyzing input {}/{}", s.searcher->currentTarget() + 1, s.searcher->targetCount());
			if (s.searcher->inConditions()) {
				text += fmt::format(" - next-click conditions {}/{}", s.searcher->conditionPos(), s.searcher->conditionCount());
			}
			setProgressLabel(pl, text);
		}
	}
};

class $modify(CWPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		loadStoredMarkers(this);
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		auto& s = S();
		if (s.mode == Mode::Analyzing && s.searcher && static_cast<PlayLayer*>(this) == PlayLayer::get()) {
			if (s.pendingLoad || s.finishRequested) return;
			// swallow the death: the search decides what happens next
			auto cmd = s.searcher->onDeath(m_gameState.m_currentProgress);
			if (cmd.loadCheckpoint) s.pendingLoad = true;
			if (cmd.done) s.finishRequested = true;
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void levelComplete() {
		auto& s = S();
		if (s.mode == Mode::Analyzing && s.searcher && static_cast<PlayLayer*>(this) == PlayLayer::get()) {
			if (s.pendingLoad || s.finishRequested) return;
			auto cmd = s.searcher->onLevelComplete(m_gameState.m_currentProgress);
			if (cmd.loadCheckpoint) s.pendingLoad = true;
			if (cmd.done) s.finishRequested = true;
			return;
		}
		PlayLayer::levelComplete();
	}

	void resetLevel() {
		auto& s = S();
		if (s.mode == Mode::Analyzing) {
			// the player restarted manually: cancel the analysis
			endSession();
			removeProgressLabel(this);
			Notification::create("Click window analysis cancelled", NotificationIcon::Warning)->show();
		}
		PlayLayer::resetLevel();
		s.shownCond = -2; // re-evaluate the conditions text for the new attempt
		if (s.mode == Mode::Playback) syncPlaybackCursor(m_gameState.m_currentProgress);
	}

	void onQuit() {
		endSession(); // also ends playback
		PlayLayer::onQuit();
	}
};

class $modify(CWPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto win = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({0.f, 0.f});
		menu->setID("click-windows-menu"_spr);

		auto addButton = [&](char const* text, SEL_MenuHandler handler, float y) {
			auto spr = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_01.png", .6f);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, handler);
			btn->setPosition({win.width - 75.f, y});
			menu->addChild(btn);
		};
		addButton("Frame Windows", menu_selector(CWPauseLayer::onAnalyze), 95.f);
		addButton(S().mode == Mode::Playback ? "Stop Macro" : "Play Macro", menu_selector(CWPauseLayer::onPlayMacro), 60.f);
		addButton("Markers", menu_selector(CWPauseLayer::onMarkers), 25.f);

		this->addChild(menu, 100);
	}

	void onAnalyze(CCObject*) {
		pickMacroAndRun(this, true);
	}

	void onPlayMacro(CCObject*) {
		auto& s = S();
		if (!PlayLayer::get()) return;

		if (s.mode == Mode::Playback) {
			s.mode = Mode::Idle;
			Notification::create("Macro playback stopped", NotificationIcon::Success)->show();
			this->onResume(nullptr);
			return;
		}
		if (!s.hasMacro) {
			// nothing loaded this session (e.g. after restarting the game): pick the macro, then play it
			pickMacroAndRun(this, false);
			return;
		}
		beginPlayback(this);
	}

	void onMarkers(CCObject*) {
		if (S().store.results.empty()) {
			FLAlertLayer::create("No markers yet", "Use \"Frame Windows\" to analyze a macro for this level first.", "OK")->show();
			return;
		}
		MarkersPopup::create()->show();
	}
};
