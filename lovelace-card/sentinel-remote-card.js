/**
 * Sentinel Remote Card
 * ---------------------
 * A modern, contemporary reinterpretation of the Vent-Axia Sentinel (B series)
 * wired remote control for MVHR units, for use as a Home Assistant Lovelace card.
 *
 * The physical remote has a 16x2 character LCD and four buttons: BOOST, DOWN,
 * SELECT and UP. This card mirrors that exact interaction model (including
 * press-and-hold fast-scroll on Up/Down) but rebuilds it as a clean glass/metal
 * panel that fits a modern dashboard, in both light and dark Home Assistant themes.
 *
 * Beyond the remote itself the card carries these surfaces, all optional:
 *   - the header vent glyph, which spins at a rate proportional to airflow_entity,
 *     with a live boost-countdown badge alongside it when a timed boost is running
 *   - an alert rail that renders nothing at all while the unit is healthy, so a
 *     quiet card is a well unit
 *   - two labelled chip groups (Climate, System) of numeric readouts, each
 *     wrapping on its own line rather than one row that clips
 *   - an airflow-mode segmented control (Normal / boost durations / Purge), which
 *     is also the only way to start and stop a purge
 *   - a "More settings" disclosure, closed by default, holding the summer-bypass
 *     switch, the two bypass temperature setpoints, and the row of maintenance
 *     actions (refresh diagnostics, sync clock, reset filter) -- everything
 *     occasional-use, out from under the remote until asked for
 *
 * Every status entity is opt-in by presence: name the entity in the config and
 * the feature appears, omit it and it disappears. The disclosure itself follows
 * the same rule one level up -- it only appears once at least one settings
 * entity or maintenance button is configured behind it.
 *
 * Two ordered lists, `chips:` and `alerts:`, additionally control *which* of the
 * configured readouts appear and in what order. Omitting either list falls back
 * to the historical default set and order, so a config written before those keys
 * existed renders exactly as it always did. That backward-compatibility contract
 * is why DEFAULT_CHIP_ORDER/DEFAULT_ALERT_ORDER are spelled out as constants
 * below rather than being derived as "everything in the catalogue".
 *
 * Install:
 *   1. Copy this file to <config>/www/sentinel-remote-card.js
 *   2. Settings -> Dashboards -> Resources -> Add Resource
 *        URL: /local/sentinel-remote-card.js?v=1   Type: JavaScript Module
 *   3. Add a card with type: custom:sentinel-remote-card (see README for options)
 *
 * Updating: copying a newer file over the old one is often not enough. Browsers
 * cache by URL, so the stale card can survive a dashboard reload and even a Home
 * Assistant restart -- typically showing up as an update that lands on one device
 * but not another, or new config keys being silently ignored. Bump the version
 * query on the resource (?v=1 -> ?v=2) to make it a different URL. Any change to
 * the value works; only changing it matters.
 */

const CARD_TAG = "sentinel-remote-card";
const EDITOR_TAG = "sentinel-remote-card-editor";

const DEFAULT_ACCENT = "#3ddc84"; // LCD phosphor green, matches the original unit
const FAST_SCROLL_DELAY_MS = 550; // matches "hold >2s" spec, felt snappier at ~0.55s repeat start
const FAST_SCROLL_REPEAT_MS = 160;

// How long "Confirm?" stays armed on the reset-filter action before it lapses
// back to its resting state. On the physical remote that operation's
// deliberateness comes from having to hold Up+Down for 5.5 seconds; a single
// tap on a dashboard has none of that, so this window is the whole guard in
// front of the one irreversible operation the component exposes. Short enough
// that a stray tap cannot arm it and then be completed minutes later by
// someone else walking past the tablet.
const CONFIRM_WINDOW_MS = 4000;

// How long a tapped airflow-mode segment stays marked pending before the card
// gives up waiting for the unit to confirm it. The select is deliberately not
// optimistic (select.py): it publishes only what the unit's own status line
// confirms, and a transition runs ~25-30s, plus up to CONTINUOUS_CONFIRM_MS
// (20s) more to confirm a continuous boost. This is that worst case with
// margin -- and it must expire rather than latch, because some transitions
// legitimately never complete: selecting Normal during a switched-live boost
// fails on purpose (see the README), and a pending mark that never cleared
// would misreport that as still-in-flight forever.
const MODE_PENDING_TIMEOUT_MS = 90000;

// Vent glyph rotation period at 0% and 100% airflow. The unit idles around 20-30%
// and boosts to ~50-60%, so the interesting range sits in the middle: a linear map
// between these two keeps trickle visibly slow and boost visibly urgent without the
// top end becoming a blur.
const SPIN_SLOWEST_S = 3.2;
const SPIN_FASTEST_S = 0.8;

// Six-spoke snowflake for the antifrost alert pill: three thin rounded bars
// through the centre, each rotated 60 deg from the last -- same rotated-copy
// technique as the header vent glyph's three ellipses, just three bars
// instead of three blades.
const SNOWFLAKE_ICON = `
  <g fill="currentColor">
    <rect x="11.25" y="2" width="1.5" height="20" rx="0.75"/>
    <rect x="11.25" y="2" width="1.5" height="20" rx="0.75" transform="rotate(60 12 12)"/>
    <rect x="11.25" y="2" width="1.5" height="20" rx="0.75" transform="rotate(120 12 12)"/>
  </g>
`;

// Single-path glyphs, kept as bare `d` strings and wrapped in <path> at render
// time. Deliberately simple shapes rather than an icon-font dependency: the
// card has no external resources at all, which is what lets it be installed by
// copying one file.
const ICON = {
  // Chips
  supplyTemp: "M12 3v10.2l-3.6-3.6L7 11l5 5 5-5-1.4-1.4-3.6 3.6V3h-2ZM4 19h16v2H4v-2Z",
  extractTemp: "M12 21V10.8l3.6 3.6L17 13l-5-5-5 5 1.4 1.4L12 10.8V21h-2 4-2ZM4 3h16v2H4V3Z",
  thermometer:
    "M12 2a3 3 0 0 0-3 3v8.6a5 5 0 1 0 6 0V5a3 3 0 0 0-3-3Zm0 2a1 1 0 0 1 1 1v9.6l.6.4a3 3 0 1 1-3.2 0l.6-.4V5a1 1 0 0 1 1-1Z",
  droplet: "M12 2s6 7.2 6 11.5a6 6 0 1 1-12 0C6 9.2 12 2 12 2Z",
  co2: "M12 2a5 5 0 0 0-5 5c0 3 5 9 5 9s5-6 5-9a5 5 0 0 0-5-5Z",
  tachometer: "M12 2a10 10 0 1 0 10 10h-2a8 8 0 1 1-8-8V2Zm1 4v7h6a6 6 0 0 0-6-6Z",
  bars: "M4 14h3v6H4v-6Zm5-4h3v10H9V10Zm5-4h3v14h-3V6Zm5-4h3v18h-3V2Z",
  funnel: "M3 4h18l-7 8v7l-4 2v-9L3 4Z",
  clock: "M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2Zm1 10.41V6h-2v7.41l5.29 5.3 1.42-1.42Z",

  // Alert-rail pills
  sun: "M12 7a5 5 0 1 0 5 5 5 5 0 0 0-5-5Zm0-5 1.8 3.1h-3.6L12 2Zm0 20-1.8-3.1h3.6L12 22ZM2 12l3.1-1.8v3.6L2 12Zm20 0-3.1 1.8v-3.6L22 12Z",
  warning: "M12 2 1 21h22L12 2Zm1 14h-2v2h2v-2Zm0-7h-2v5h2V9Z",
  // A no-entry circle: unmistakable at 12px, and the only glyph here that has
  // to read as "nothing is getting through" rather than "something is wrong".
  offline:
    "M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2Zm0 2a7.9 7.9 0 0 1 4.9 1.7L5.7 16.9A8 8 0 0 1 12 4Zm0 16a7.9 7.9 0 0 1-4.9-1.7L18.3 7.1A8 8 0 0 1 12 20Z",
  // Two curling airstreams -- purge is the unit at full tilt, so it gets the
  // most emphatic air glyph on the card.
  wind: "M4 7h9a2.5 2.5 0 1 0-2.5-2.5h-2A4.5 4.5 0 1 1 13 9H4V7Zm0 4h13a2.5 2.5 0 1 1-2.5 2.5h-2A4.5 4.5 0 1 0 17 13H4v-2Z",
  // Sun over a horizon line: a thaw, distinct from antifrost's snowflake.
  thaw: "M12 6a4 4 0 1 1-4 4 4 4 0 0 1 4-4Zm0-5 1.5 3h-3L12 1ZM4.2 4.2 6.6 5.4 5.4 6.6 4.2 4.2Zm15.6 0-1.2 2.4-1.2-1.2 2.4-1.2ZM2 12l3-1.5v3L2 12Zm20 0-3 1.5v-3l3 1.5ZM4 19h16v2H4v-2Z",
  // Droplet above a floor line: moisture being driven out of the fabric.
  dryout: "M12 2s6 7.2 6 11.5a6 6 0 1 1-12 0C6 9.2 12 2 12 2Zm-5 19h10v2H7v-2Z",
  // A rocker switch plate -- the wall switch that is holding the boost on.
  switchPlate: "M7 2h10a2 2 0 0 1 2 2v16a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2Zm2 3v6h6V5H9Z",

  // Maintenance actions
  refresh: "M12 4V1L8 5l4 4V6a6 6 0 1 1-6 6H4a8 8 0 1 0 8-8Z",
  sliders: "M4 6h10v2H4V6Zm12 0h4v2h-4V6Zm-4-3h2v8h-2V3ZM4 16h4v2H4v-2Zm6 0h10v2H10v-2Zm-4-3h2v8H6v-8Z",

  // The "More settings" disclosure toggle -- a plain chevron, reused from the
  // Down button's own glyph so it reads as "reveal what's below" rather than
  // introducing a fourth arrow style onto the card.
  chevronDown: "M7 10l5 5 5-5H7Z",
};

// Numeric readouts, keyed by the short id used in the `chips:` config list.
// `key` is the config key naming the entity; the chip is skipped when that key
// is absent, so listing an id costs nothing until its entity is configured too.
//
// `stale: true` marks a reading that comes off the ~15 minute diagnostics
// scrape rather than the live status frames, which adds "updated hh:mm" to the
// tooltip from diagnostics_updated_entity. Everything sourced from a diagnostic
// page carries it -- including humidity, which predates the flag and was
// previously (wrongly) presented as live.
//
// `group` places a qualifying chip into one of two labelled rows under the
// chip area (`_renderChips`) -- "climate" for what the air is doing, "system"
// for what the hardware is doing. `chips:`/DEFAULT_CHIP_ORDER still control
// membership and order exactly as before; grouping only changes where a
// qualifying chip lands, never whether it qualifies. `boost_remaining` has no
// group: it is live, not diagnostic, and rides in the header next to the
// airflow badge instead (see `_renderChips`'s boost-badge handling).
const CHIP_CATALOGUE = {
  supply_temp: {
    key: "supply_temp_entity",
    icon: ICON.supplyTemp,
    label: "Supply air (to house)",
    fallbackUnit: "°C",
    stale: true,
    group: "climate",
  },
  extract_temp: {
    key: "extract_temp_entity",
    icon: ICON.extractTemp,
    label: "Extract air (from house)",
    fallbackUnit: "°C",
    stale: true,
    group: "climate",
  },
  // Labelled "unit sensor" on purpose. The MVHR's menu has a screen called
  // "Indoor Temp" which is the summer-bypass *setpoint*, not a reading; naming
  // this chip just "Indoor temp" would collide with it on the same dashboard.
  indoor_temp: {
    key: "indoor_temp_entity",
    icon: ICON.thermometer,
    label: "Indoor air (unit sensor)",
    fallbackUnit: "°C",
    stale: true,
    group: "climate",
  },
  humidity: {
    key: "humidity_entity",
    icon: ICON.droplet,
    label: "Humidity",
    fallbackUnit: "%",
    stale: true,
    group: "climate",
  },
  humidity_avg: {
    key: "humidity_avg_entity",
    icon: ICON.droplet,
    label: "Humidity (5 min average)",
    fallbackUnit: "%",
    stale: true,
    group: "climate",
  },
  co2: { key: "co2_entity", icon: ICON.co2, label: "CO2", fallbackUnit: "ppm", group: "climate" },
  // RPM is the one readout that distinguishes "commanded 30%" from "actually
  // turning"; drive percentage rising at constant RPM is the early signal of a
  // blocked filter or duct.
  supply_rpm: {
    key: "supply_rpm_entity",
    icon: ICON.tachometer,
    label: "Supply fan speed",
    fallbackUnit: "rpm",
    stale: true,
    group: "system",
  },
  extract_rpm: {
    key: "extract_rpm_entity",
    icon: ICON.tachometer,
    label: "Extract fan speed",
    fallbackUnit: "rpm",
    stale: true,
    group: "system",
  },
  supply_pwm: {
    key: "supply_pwm_entity",
    icon: ICON.bars,
    label: "Supply motor drive",
    fallbackUnit: "%",
    stale: true,
    group: "system",
  },
  extract_pwm: {
    key: "extract_pwm_entity",
    icon: ICON.bars,
    label: "Extract motor drive",
    fallbackUnit: "%",
    stale: true,
    group: "system",
  },
  // Shares filter_entity with the alert rail, which uses the same figure for
  // its "Filter due - 312 h" detail. Listing this id promotes it to a chip of
  // its own for people who want the countdown visible all the time.
  filter_hours: {
    key: "filter_entity",
    icon: ICON.funnel,
    label: "Filter life remaining",
    fallbackUnit: "h",
    stale: true,
    group: "system",
  },
  boost_remaining: {
    key: "boost_remaining_entity",
    icon: ICON.clock,
    label: "Boost ends in",
    fallbackUnit: "min",
    hideWhenZero: true,
  },
};

// The set and order the card shipped with before `chips:` existed. Omitting
// `chips:` must reproduce this exactly -- see the header comment.
const DEFAULT_CHIP_ORDER = ["supply_temp", "extract_temp", "humidity", "co2", "boost_remaining"];

// Alert-rail pills, keyed by the short id used in the `alerts:` config list.
//
// `severity` picks the pill tint: `info` (accent) for the unit protecting
// itself or the house automatically, `warn` (amber) for something the user
// must eventually act on, `alarm` (red) for a fault or a dead link. Bypass and
// antifrost are deliberately `info` rather than amber -- an open bypass in
// summer is correct behaviour and must not read as a fault.
//
// `invert` means the pill shows while the entity is *off*, which is what
// "offline" needs. An unavailable entity counts as off, so a whole ESPHome
// device dropping off the network raises it too.
//
// `resolve` replaces both the activity test and the label for the two pills
// that are not a plain on/off read.
const ALERT_CATALOGUE = {
  offline: {
    key: "link_entity",
    invert: true,
    severity: "alarm",
    icon: ICON.offline,
    label: "MVHR offline",
    title: "No frames arriving from the MVHR — the unit, the dongle or the link is down",
  },
  bypass: {
    key: "bypass_entity",
    severity: "info",
    icon: ICON.sun,
    label: "Bypass",
    title: "Summer bypass open",
  },
  antifrost: { key: "antifrost_entity", severity: "info", icon: SNOWFLAKE_ICON, rawIcon: true, resolve: "antifrost" },
  defrost: {
    key: "defrost_entity",
    severity: "info",
    icon: ICON.thaw,
    label: "Defrost",
    title: "Defrost cycle running",
  },
  dryout: {
    key: "dryout_entity",
    severity: "info",
    icon: ICON.dryout,
    label: "Dryout",
    title: "Dryout mode — the unit is drying the building fabric",
  },
  // Status line2's alpha annunciator (column 15, rendered '*' — see
  // status.h's has_sensor_boost_annunciator()): the internal humidity
  // sensor (or a proportional 0-10V sensor, not fitted on this unit) has
  // raised airflow on its own. `info`, not `warn`/`alarm`, for the same
  // reason bypass/dryout are: the unit responding correctly to measured
  // humidity, not a fault — and it clears itself once humidity drops.
  // Reuses ICON.dryout rather than a new glyph: both pills are the unit
  // moving air in response to moisture, just triggered from opposite
  // directions (dryout pushes moisture out deliberately; this is the unit
  // reacting to moisture already in the air).
  humidity_boost: {
    key: "humidity_boost_entity",
    severity: "info",
    icon: ICON.dryout,
    label: "Humidity boost",
    title: "The humidity sensor has raised airflow in proportion to measured humidity. Clears itself once humidity drops.",
  },
  purge: {
    key: "purge_entity",
    severity: "info",
    icon: ICON.wind,
    label: "Purge",
    title: "Purge running — full airflow",
  },
  // The most valuable pill on the rail: a wall switched live (commonly taken
  // off a bathroom light) holds the unit's own boost input directly, and
  // nothing but that switch releasing will clear it. Without this the failure
  // reads as the card being broken. Comes off diagnostic page 05, so it is
  // stale by construction -- the tooltip says so, because it explains a boost
  // after the fact rather than reporting one live.
  switched_live: {
    key: "switched_live_entity",
    severity: "info",
    icon: ICON.switchPlate,
    label: "Switched-live boost",
    title:
      "A wired wall switch is holding the boost on. It cannot be cancelled from Home Assistant — only the switch releasing will clear it. Read from the ~15 minute diagnostics scrape, so this may lag.",
  },
  filter: { key: "filter_due_entity", severity: "warn", icon: ICON.warning, resolve: "filter" },
  supply_fault: {
    key: "supply_fault_entity",
    severity: "alarm",
    icon: ICON.warning,
    label: "T1 sensor fault",
    title: "Supply air temperature sensor (T1) fault",
  },
  extract_fault: {
    key: "extract_fault_entity",
    severity: "alarm",
    icon: ICON.warning,
    label: "T2 sensor fault",
    title: "Extract air temperature sensor (T2) fault",
  },
  rail_fault: {
    key: "rail_fault_entity",
    severity: "alarm",
    icon: ICON.warning,
    label: "24 V rail fault",
    title: "24 V rail fault — check fuse FS1",
  },
};

// Unlike DEFAULT_CHIP_ORDER this is the full catalogue rather than a historical
// subset: an alert only renders when its entity is both configured *and*
// asserted, so listing every id by default costs nothing for a config that
// names none of the new ones.
//
// The ordering is "faults first, then everything else" -- but note that
// bypass/antifrost/filter appear in that relative order deliberately, because
// it is the order the card rendered them in before `alerts:` existed. Sorting
// the actionable amber filter pill above the informational bypass one would be
// marginally better triage and was tried; it also silently reshuffled a rail
// that existing dashboards already display, which is not a trade worth making
// for a rail that rarely shows more than one pill at a time. New pills slot in
// around that fixed trio rather than through it.
const DEFAULT_ALERT_ORDER = [
  "offline",
  "supply_fault",
  "extract_fault",
  "rail_fault",
  "bypass",
  "antifrost",
  "filter",
  "defrost",
  "dryout",
  "humidity_boost",
  "purge",
  "switched_live",
];

// Compact labels for the airflow-mode segments. The segment list itself is
// always read from the select entity's own `options` attribute -- never
// hardcoded -- because that order is load-bearing on the component side
// (select.py maps it index-for-index onto AirflowTarget). This map only
// shortens what is displayed, and passes anything it does not recognise
// straight through, so a future option appears as itself rather than vanishing.
const MODE_LABELS = {
  Normal: "Normal",
  "Boost 30 min": "30m",
  "Boost 60 min": "60m",
  "Boost Continuous": "Cont",
  Purge: "Purge",
};

// Maintenance actions, each backing a sequence on the component side. These are
// the card's answer to the physical remote's held key combos: rather than try
// to reproduce a 5.5s two-key hold over a fire-and-forget service call, each
// one presses the button that starts the sequence which already performs that
// hold, with its own screen checks and self-verification around it.
const ACTION_BUTTONS = [
  {
    key: "refresh_diagnostics_button",
    icon: ICON.refresh,
    label: "Refresh",
    title: "Re-scan the unit's diagnostic pages (~15 min of staleness cleared in one pass)",
  },
  {
    key: "refresh_settings_button",
    icon: ICON.sliders,
    label: "Settings",
    title: "Re-read the summer bypass settings from the unit",
  },
  { key: "sync_clock_button", icon: ICON.clock, label: "Clock", title: "Set the unit's clock from Home Assistant" },
  {
    key: "reset_filter_button",
    icon: ICON.funnel,
    label: "Filter",
    confirm: true,
    title: "Restart the filter service countdown. Irreversible — press only after actually changing the filters.",
  },
];

// The two bypass temperature setpoints, rendered as tap-to-step rows in the
// settings panel. `key` names the entity as usual; min/max/step/unit are read
// from the number entity's own attributes when present (ESPHome publishes
// them from number.py's declared bounds) and these are only the fallback for
// an entity that, for whatever reason, doesn't carry them.
const SETTINGS_NUMBERS = [
  {
    key: "bypass_indoor_temp_entity",
    label: "Indoor target",
    sub: "Summer bypass indoor setpoint",
    fallbackMin: 16,
    fallbackMax: 40,
    fallbackStep: 1,
    fallbackUnit: "°C",
  },
  {
    key: "bypass_outdoor_temp_entity",
    label: "Outdoor cut-off",
    sub: "Summer bypass outdoor cut-off",
    fallbackMin: 5,
    fallbackMax: 20,
    fallbackStep: 1,
    fallbackUnit: "°C",
  },
];

class SentinelRemoteCard extends HTMLElement {
  static getStubConfig() {
    return {
      type: `custom:${CARD_TAG}`,
      title: "Hallway MVHR",
      line1_entity: "sensor.mvhr_display_line1",
      line2_entity: "sensor.mvhr_display_line2",
      boost_button: "button.mvhr_boost",
      down_button: "button.mvhr_down",
      select_button: "button.mvhr_select",
      up_button: "button.mvhr_up",
    };
  }

  static getConfigElement() {
    return document.createElement(EDITOR_TAG);
  }

  setConfig(config) {
    if (!config) throw new Error("Invalid configuration");
    if (!config.line1_entity && !config.display_entity) {
      throw new Error(
        "sentinel-remote-card: set either 'display_entity' (single multi-line sensor) or 'line1_entity' / 'line2_entity'."
      );
    }
    this._config = {
      display_separator: "\n",
      theme: "auto",
      accent_color: DEFAULT_ACCENT,
      ...config,
    };
    this._holdTimer = null;
    this._holdInterval = null;
    // Which mode segment has been tapped but not yet confirmed by the unit, and
    // when. Cleared on confirmation or after MODE_PENDING_TIMEOUT_MS -- see that
    // constant for why it must be able to lapse.
    this._pendingMode = null;
    this._pendingModeAt = 0;
    // Which action button is armed for its second, confirming tap.
    this._armedAction = null;
    this._armTimer = null;
    // Signatures of the last-rendered mode row and action row. Both contain
    // live buttons, and rewriting their innerHTML out from under a pointer
    // swallows the click, so they are only re-rendered when something they
    // display has actually changed.
    this._modeSig = null;
    this._actionSig = null;
    // Whether the settings/maintenance disclosure is open. Local UI state,
    // not derived from hass -- see the click handler in `_build()`.
    this._settingsOpen = false;
    if (!this._built) this._build();
    this._applyStaticConfig();
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._built) return;
    this._render();
  }

  getCardSize() {
    return 5;
  }

  connectedCallback() {
    if (this._built) this._render();
  }

  disconnectedCallback() {
    this._clearHold();
    this._disarmAction();
  }

  // ---------------------------------------------------------------- build --

  _build() {
    const root = this.attachShadow({ mode: "open" });
    root.innerHTML = `
      <style>${this._css()}</style>
      <ha-card>
        <div class="panel" part="panel">
          <div class="header">
            <div class="title"></div>
            <div class="airflow">
              <div class="vent-icon" title="Unit status">
                <svg viewBox="0 0 24 24" width="16" height="16">
                  <g fill="currentColor">
                    <ellipse cx="12" cy="6.8" rx="2.5" ry="4.4" transform="rotate(0 12 12)"/>
                    <ellipse cx="12" cy="6.8" rx="2.5" ry="4.4" transform="rotate(120 12 12)"/>
                    <ellipse cx="12" cy="6.8" rx="2.5" ry="4.4" transform="rotate(240 12 12)"/>
                  </g>
                  <circle cx="12" cy="12" r="1.7" fill="var(--card-background-color, #fff)"/>
                </svg>
              </div>
              <span class="airflow-pct" id="airflowPct" title="Live airflow"></span>
              <span class="boost-badge" id="boostBadge"></span>
            </div>
          </div>

          <div class="bezel">
            <div class="lcd">
              <div class="lcd-line" id="l1">&nbsp;</div>
              <div class="lcd-line" id="l2">&nbsp;</div>
              <div class="lcd-scanline"></div>
            </div>
            <div class="busy-bar" id="busyBar"><span></span></div>
          </div>

          <div class="alerts" id="alerts"></div>

          <div class="chips" id="chips">
            <div class="chip-group" id="chipsClimateGroup">
              <div class="chip-group-label">Climate</div>
              <div class="chip-group-row" id="chipsClimateRow"></div>
            </div>
            <div class="chip-group" id="chipsSystemGroup">
              <div class="chip-group-label">System</div>
              <div class="chip-group-row" id="chipsSystemRow"></div>
            </div>
          </div>

          <div class="mode-row" id="modeRow"></div>

          <div class="buttons">
            <button class="btn btn-boost" data-key="boost" title="Boost">
              <svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M13 2 4 14h6l-1 8 9-12h-6l1-8Z"/></svg>
              <span>Boost</span>
            </button>
            <div class="nav-row">
              <button class="btn btn-nav" data-key="down" title="Down">
                <svg viewBox="0 0 24 24" width="20" height="20"><path fill="currentColor" d="M7 10l5 5 5-5H7Z"/></svg>
              </button>
              <button class="btn btn-select" data-key="select" title="Select">
                <svg viewBox="0 0 24 24" width="18" height="18"><path fill="currentColor" d="M9 16.2 4.8 12l-1.4 1.4L9 19 21 7l-1.4-1.4L9 16.2Z"/></svg>
              </button>
              <button class="btn btn-nav" data-key="up" title="Up">
                <svg viewBox="0 0 24 24" width="20" height="20"><path fill="currentColor" d="M7 14l5-5 5 5H7Z"/></svg>
              </button>
            </div>
          </div>

          <button class="disclosure-toggle" id="moreToggle" title="Show settings and maintenance actions" aria-expanded="false">
            <span>More settings</span>
            <svg viewBox="0 0 24 24" width="14" height="14"><path fill="currentColor" d="${ICON.chevronDown}"/></svg>
          </button>
          <div class="disclosure-body" id="moreBody">
            <div>
              <div class="disclosure-inner">
                <div class="settings" id="settingsPanel"></div>
                <div class="actions" id="actions"></div>
              </div>
            </div>
          </div>
        </div>
      </ha-card>
    `;

    this._els = {
      panel: root.querySelector(".panel"),
      title: root.querySelector(".title"),
      vent: root.querySelector(".vent-icon"),
      airflowPct: root.querySelector("#airflowPct"),
      boostBadge: root.querySelector("#boostBadge"),
      l1: root.querySelector("#l1"),
      l2: root.querySelector("#l2"),
      busyBar: root.querySelector("#busyBar"),
      alerts: root.querySelector("#alerts"),
      chips: root.querySelector("#chips"),
      chipsClimateGroup: root.querySelector("#chipsClimateGroup"),
      chipsClimateRow: root.querySelector("#chipsClimateRow"),
      chipsSystemGroup: root.querySelector("#chipsSystemGroup"),
      chipsSystemRow: root.querySelector("#chipsSystemRow"),
      modeRow: root.querySelector("#modeRow"),
      moreToggle: root.querySelector("#moreToggle"),
      moreBody: root.querySelector("#moreBody"),
      settingsPanel: root.querySelector("#settingsPanel"),
      actions: root.querySelector("#actions"),
      boost: root.querySelector('[data-key="boost"]'),
      down: root.querySelector('[data-key="down"]'),
      select: root.querySelector('[data-key="select"]'),
      up: root.querySelector('[data-key="up"]'),
    };

    // Delegated, so the handlers survive the innerHTML rewrites both rows do.
    this._els.modeRow.addEventListener("click", (ev) => {
      const btn = ev.target.closest("[data-option]");
      if (btn && !btn.disabled) this._onModeSelect(btn.dataset.option);
    });
    this._els.actions.addEventListener("click", (ev) => {
      const btn = ev.target.closest("[data-action]");
      if (btn && !btn.disabled) this._onAction(btn.dataset.action);
    });
    this._els.settingsPanel.addEventListener("click", (ev) => {
      const toggleBtn = ev.target.closest("[data-toggle]");
      if (toggleBtn && !toggleBtn.disabled) return this._onSettingsToggle(toggleBtn.dataset.toggle);
      const stepBtn = ev.target.closest("[data-step]");
      if (stepBtn && !stepBtn.disabled) this._onSettingsStep(stepBtn.dataset.step, Number(stepBtn.dataset.dir));
    });
    // Purely local UI state -- which entities are behind it is a render
    // concern (see `_renderMore`), but open/closed is not hass-derived, so
    // toggling it goes straight to the DOM rather than through `_render()`.
    this._els.moreToggle.addEventListener("click", () => this._toggleMore());

    for (const key of ["boost", "down", "select", "up"]) {
      const el = this._els[key];
      el.addEventListener("pointerdown", (ev) => this._onPress(key, ev));
      el.addEventListener("pointerup", () => this._clearHold());
      el.addEventListener("pointerleave", () => this._clearHold());
      el.addEventListener("pointercancel", () => this._clearHold());
      el.addEventListener("contextmenu", (ev) => ev.preventDefault());
    }

    if ("ResizeObserver" in window) {
      this._resizeObserver = new ResizeObserver(() => this._fitLcd());
      this._resizeObserver.observe(this._els.panel);
    }

    this._built = true;
  }

  _applyStaticConfig() {
    const c = this._config;
    this._els.title.textContent = c.title || "";
    this._els.title.style.display = c.title ? "" : "none";
    this._els.panel.style.setProperty("--accent", c.accent_color || DEFAULT_ACCENT);
    this._els.panel.classList.toggle("force-light", c.theme === "light");
    this._els.panel.classList.toggle("force-dark", c.theme === "dark");
  }

  // -------------------------------------------------------------- render --

  _render() {
    if (!this._hass || !this._config) return;
    const c = this._config;
    const hass = this._hass;

    const [line1, line2] = this._displayLines(hass, c);
    this._setLcdLine(this._els.l1, line1);
    this._setLcdLine(this._els.l2, line2);
    this._fitLcd();

    // `busy` is true while a keypad tap or a whole sequence is in flight. An
    // airflow-mode change runs ~25-30s, so without this the card looks hung
    // for half a minute after a tap; this is exactly what the component
    // publishes that sensor for.
    const busy = !!(c.busy_entity && hass.states[c.busy_entity] && this._isOn(hass.states[c.busy_entity]));
    // Link down means no frames are arriving from the MVHR. Every sequence and
    // key press is refused in that state anyway (the Runner will not start one
    // while the link is down), so presenting the controls as live would be a
    // lie. An unavailable entity counts as down, which also covers the whole
    // ESPHome node dropping off the network.
    const online = !c.link_entity || this._isOn(hass.states[c.link_entity]);
    this._els.panel.classList.toggle("offline", !online);
    this._els.busyBar.classList.toggle("on", busy);

    for (const key of ["boost", "down", "select", "up"]) {
      const entity = c[`${key}_button`];
      // The raw keys stay live while merely busy -- a refused tap is logged,
      // not harmful, and these are the documented escape hatch for when
      // something has gone wrong. Only a dead link takes them away.
      const disabled = !entity || this._isUnavailable(hass, entity) || !online;
      this._els[key].disabled = disabled;
      this._els[key].classList.toggle("disabled", disabled);
    }

    // Optional boost-active highlight
    const boostActive =
      c.boost_active_entity && hass.states[c.boost_active_entity]
        ? this._isOn(hass.states[c.boost_active_entity])
        : false;
    this._els.boost.classList.toggle("active", boostActive);
    this._els.panel.classList.toggle("boost-active", boostActive);

    this._renderAirflow(hass, c, boostActive);
    this._renderAlerts(hass, c);
    this._renderChips(hass, c);
    this._renderModeRow(hass, c, busy || !online);
    this._renderMore(hass, c, busy || !online);
  }

  // Spin the vent glyph at a rate that tracks real airflow. `airflow_entity` is
  // the only continuously-updating flow figure the component publishes (it comes
  // off the status-line decode every frame); the RPM and PWM sensors refresh only
  // on the ~15 minute diagnostics scrape and would animate a stale number.
  //
  // With no airflow_entity configured this falls back to the original behaviour
  // exactly: a fixed-rate spin driven by running_entity, or by boost when that is
  // absent too. Existing card configs therefore behave as they always did.
  _renderAirflow(hass, c, boostActive) {
    const st = c.airflow_entity ? hass.states[c.airflow_entity] : null;
    const usable = st && st.state !== "unavailable" && st.state !== "unknown";
    const pct = usable ? Number(st.state) : NaN;

    if (!usable || Number.isNaN(pct)) {
      this._els.airflowPct.textContent = "";
      this._els.airflowPct.style.display = "none";
      this._els.vent.style.removeProperty("--spin-duration");
      const running =
        c.running_entity && hass.states[c.running_entity]
          ? this._isRunningValue(hass.states[c.running_entity])
          : boostActive;
      this._els.vent.classList.toggle("spinning", !!running);
      return;
    }

    const clamped = Math.max(0, Math.min(100, pct));
    const duration = SPIN_SLOWEST_S - (clamped / 100) * (SPIN_SLOWEST_S - SPIN_FASTEST_S);
    this._els.vent.style.setProperty("--spin-duration", duration.toFixed(2) + "s");
    this._els.vent.classList.toggle("spinning", clamped > 0);

    const unit = (st.attributes && st.attributes.unit_of_measurement) || "%";
    this._els.airflowPct.textContent = `${st.state}${unit ? " " + unit : ""}`;
    this._els.airflowPct.style.display = "";
  }

  // The alert rail is silent by design: it renders nothing at all while the unit
  // is healthy, so anything appearing here is worth a second look. Bypass and
  // antifrost are both tinted with the accent rather than amber -- both are the
  // unit protecting itself/the house automatically, not a fault to act on; amber
  // is reserved for filter, the one alert that actually needs the user to do
  // something, and red for the faults and a dead link.
  //
  // Order comes from `alerts:` when configured, otherwise DEFAULT_ALERT_ORDER,
  // which runs most urgent first so a genuine fault is never pushed off the end
  // of the rail by a routine bypass opening.
  _renderAlerts(hass, c) {
    const pills = [];

    for (const id of this._orderedIds(c.alerts, DEFAULT_ALERT_ORDER, ALERT_CATALOGUE, "alerts")) {
      const def = ALERT_CATALOGUE[id];
      const entity = c[def.key];

      let detail;
      if (def.resolve === "antifrost") {
        detail = this._antifrostAlert(hass, c);
      } else if (def.resolve === "filter") {
        detail = this._filterAlert(hass, c);
      } else {
        if (!entity) continue;
        const st = hass.states[entity];
        // For an inverted pill a missing or unavailable state counts as
        // asserted, which is what makes `offline` fire when the whole ESPHome
        // node drops off the network rather than only when link_up decodes as
        // off. The `if (!entity) continue` above is what stops it firing on a
        // config that simply never named link_entity.
        const asserted = def.invert ? !this._isOn(st) : this._isOn(st);
        detail = asserted ? { label: def.label, title: def.title } : null;
      }
      if (!detail) continue;

      pills.push(this._pill(def.severity, def.rawIcon ? def.icon : this._iconPath(def.icon), detail.title, detail.label));
    }

    this._els.alerts.innerHTML = pills.join("");
    this._els.alerts.style.display = pills.length ? "" : "none";
  }

  // Resolves a configured id list against a catalogue, falling back to the
  // supplied default order when the key is absent (which is the whole
  // backward-compatibility contract -- see the header comment).
  //
  // An unrecognised id is skipped with a one-off console warning rather than
  // throwing: a typo in one list entry should cost that one readout, not blank
  // the entire card on a wall tablet nobody is standing in front of.
  _orderedIds(configured, fallback, catalogue, what) {
    if (!Array.isArray(configured)) return fallback;
    const out = [];
    for (const id of configured) {
      if (catalogue[id]) {
        out.push(id);
        continue;
      }
      this._warnOnce(`${what}: unknown id "${id}" — known ids are ${Object.keys(catalogue).join(", ")}`);
    }
    return out;
  }

  _warnOnce(message) {
    this._warned = this._warned || new Set();
    if (this._warned.has(message)) return;
    this._warned.add(message);
    console.warn(`${CARD_TAG}: ${message}`);
  }

  _iconPath(d) {
    return `<path fill="currentColor" d="${d}"/>`;
  }

  // Diagnostic page 24's antifrost mode (PLAN.md §4 in the component repo) is
  // already spelled out by the component -- e.g. "Airflow 85% / 115%" -- so the
  // mode entity, when configured, replaces the generic label with that detail
  // rather than the card inventing its own wording. antifrost_mode_entity is
  // optional: without it the pill still appears from antifrost_entity alone,
  // just with a plain "Frost protection" label.
  _antifrostAlert(hass, c) {
    const activeSt = c.antifrost_entity ? hass.states[c.antifrost_entity] : null;
    if (!activeSt || !this._isOn(activeSt)) return null;

    const modeSt = c.antifrost_mode_entity ? hass.states[c.antifrost_mode_entity] : null;
    const mode = modeSt && modeSt.state !== "unavailable" && modeSt.state !== "unknown" ? modeSt.state : "";

    return {
      label: mode ? `Frost protection · ${mode}` : "Frost protection",
      title: mode ? `Frost protection active — ${mode}` : "Frost protection active",
    };
  }

  // Two independent sources agree on "change the filter": the status line's
  // "Check Filter" message and the page-23 hours counter reaching zero, both of
  // which the component folds into filter_due_entity. filter_entity is kept as a
  // fallback for configs without the binary sensor, and supplies the hours detail
  // when both are present.
  _filterAlert(hass, c) {
    const hoursSt = c.filter_entity ? hass.states[c.filter_entity] : null;
    const hoursUsable = hoursSt && hoursSt.state !== "unavailable" && hoursSt.state !== "unknown";
    const unit = (hoursUsable && hoursSt.attributes && hoursSt.attributes.unit_of_measurement) || "";

    const dueSt = c.filter_due_entity ? hass.states[c.filter_due_entity] : null;
    let due = dueSt ? this._isOn(dueSt) : false;

    if (!due && hoursUsable) {
      // Unit-aware default: the sensor reports hours on this hardware, but the
      // card is also used with day- and percent-remaining filter sensors.
      const defaultThreshold = unit === "h" ? 336 : 14;
      const threshold = c.filter_warning_threshold ?? defaultThreshold;
      due = Number(hoursSt.state) <= threshold;
    }
    if (!due) return null;

    const detail = hoursUsable ? `${hoursSt.state}${unit ? " " + unit : ""}` : "";
    return {
      label: detail ? `Filter due · ${detail}` : "Filter due",
      title: detail ? `Filter change due — ${detail} remaining` : "Filter change due",
    };
  }

  // `iconSvg` is raw inner-SVG markup rather than a bare path `d` string, so a
  // pill can use something richer than a single path -- SNOWFLAKE_ICON's three
  // rotated bars, for instance -- without _pill() itself needing to know that.
  _pill(kind, iconSvg, title, label) {
    return `
      <div class="chip ${kind}" title="${this._esc(title)}">
        <svg viewBox="0 0 24 24" width="12" height="12">${iconSvg}</svg>
        <span>${this._esc(label)}</span>
      </div>
    `;
  }

  _displayLines(hass, c) {
    if (c.display_entity) {
      const st = hass.states[c.display_entity];
      const raw = st ? st.state : "";
      const sep = c.display_separator ?? "\n";
      const parts = (raw || "").split(sep);
      return [parts[0] || "", parts[1] || ""];
    }
    const s1 = c.line1_entity ? hass.states[c.line1_entity] : null;
    const s2 = c.line2_entity ? hass.states[c.line2_entity] : null;
    const val = (s) => (s && s.state !== "unavailable" && s.state !== "unknown" ? s.state : "");
    return [val(s1), val(s2)];
  }

  _setLcdLine(el, text) {
    el.textContent = text && text.length ? text : "\u00a0";
  }

  // Shrink the LCD font (in lockstep on both lines) until neither line
  // overflows its available width, so text is never clipped regardless
  // of how wide the card ends up on a given dashboard.
  _fitLcd() {
    const els = [this._els.l1, this._els.l2];
    if (!els[0] || !els[0].isConnected) return;
    els.forEach((el) => (el.style.fontSize = ""));
    const maxSize = parseFloat(getComputedStyle(els[0]).fontSize);
    const minSize = 12;
    let size = maxSize;
    const overflowing = () => els.some((el) => el.scrollWidth > el.clientWidth + 0.5);
    let guard = 0;
    while (overflowing() && size > minSize && guard < 30) {
      size -= 1;
      els.forEach((el) => (el.style.fontSize = size + "px"));
      guard++;
    }
  }

  // The airflow-mode segmented control, and the only route to purge on this
  // card. It drives the `airflow_mode` select, which is an absolute set-point:
  // "Boost 60 min" means put the unit into a 60 minute boost from wherever it
  // currently is, not "add 60 minutes". That is the whole reason the component
  // models boost as a select rather than as key presses -- the unit's Main key
  // is a cumulative counter with no usable timeout, so only an absolute target
  // is expressible.
  //
  // Segments are read from the entity's own `options` attribute rather than a
  // hardcoded list. That order is load-bearing on the component side (it is
  // mapped index-for-index onto AirflowTarget), so reading it at runtime is
  // what keeps the card from silently drifting out of step with the firmware.
  _renderModeRow(hass, c, locked) {
    const el = this._els.modeRow;
    const entity = c.airflow_mode_entity;
    const st = entity ? hass.states[entity] : null;
    const options = st && Array.isArray(st.attributes && st.attributes.options) ? st.attributes.options : [];

    if (!options.length) {
      if (this._modeSig !== "off") {
        el.innerHTML = "";
        el.style.display = "none";
        this._modeSig = "off";
      }
      return;
    }

    const current = st.state;
    // Drop the pending mark once the unit confirms the target -- or once it
    // reports anything else that is not what we asked for and the window has
    // run out. The timeout matters: a transition that legitimately never lands
    // (selecting Normal during a switched-live boost) must stop claiming to be
    // in flight rather than latching forever.
    if (this._pendingMode) {
      if (current === this._pendingMode || Date.now() - this._pendingModeAt > MODE_PENDING_TIMEOUT_MS) {
        this._pendingMode = null;
      }
    }

    const sig = `${options.join("")}|${current}|${this._pendingMode || ""}|${locked ? 1 : 0}`;
    if (sig === this._modeSig) return;
    this._modeSig = sig;

    const segments = options.map((opt) => {
      const label = MODE_LABELS[opt] || opt;
      const classes = ["seg"];
      if (opt === current) classes.push("active");
      if (opt === this._pendingMode && opt !== current) classes.push("pending");
      return `
        <button class="${classes.join(" ")}" data-option="${this._esc(opt)}" title="${this._esc(opt)}"${
        locked ? " disabled" : ""
      }>${this._esc(label)}</button>
      `;
    });

    el.innerHTML = segments.join("");
    el.style.display = "";
  }

  _onModeSelect(option) {
    if (!this._hass || !this._config.airflow_mode_entity) return;
    // Marked pending immediately, because the select is not optimistic: nothing
    // will change in `state` until the unit's own status line confirms it, up
    // to half a minute later. Without this the tap looks ignored.
    this._pendingMode = option;
    this._pendingModeAt = Date.now();
    this._modeSig = null;
    this._hass.callService("select", "select_option", {
      entity_id: this._config.airflow_mode_entity,
      option,
    });
    this._render();
  }

  // The "More settings" disclosure: everything occasional-use (the summer
  // bypass switch, the two bypass temperature setpoints, the maintenance
  // action buttons) lives behind it, closed by default, so the everyday card
  // is just the remote and the readouts. The toggle itself only appears when
  // there's something behind it to show -- same opt-in-by-presence rule as
  // every other surface on the card, just applied to the drawer as a whole.
  _renderMore(hass, c, locked) {
    const hasSettings = this._renderSettings(hass, c, locked);
    this._renderActions(hass, c, locked);
    const hasActions = !!(this._els.actions.style.display !== "none" && this._els.actions.innerHTML);

    const show = hasSettings || hasActions;
    this._els.moreToggle.style.display = show ? "" : "none";
    this._els.moreBody.style.display = show ? "" : "none";
    if (!show) return;

    // Open/closed itself is local UI state toggled directly by `_toggleMore()`
    // -- this only keeps the DOM in sync with it across a re-render (e.g. the
    // toggle staying open while a stepper tap re-renders the panel).
    this._els.moreBody.classList.toggle("open", this._settingsOpen);
    this._els.moreToggle.classList.toggle("open", this._settingsOpen);
  }

  _toggleMore() {
    this._settingsOpen = !this._settingsOpen;
    const open = this._settingsOpen;
    this._els.moreBody.classList.toggle("open", open);
    this._els.moreToggle.classList.toggle("open", open);
    this._els.moreToggle.setAttribute("aria-expanded", open ? "true" : "false");
    this._els.moreToggle.title = open
      ? "Hide settings and maintenance actions"
      : "Show settings and maintenance actions";
    this._els.moreToggle.querySelector("span").textContent = open ? "Hide settings" : "More settings";
  }

  // The settings panel: the summer-bypass switch and the two bypass
  // temperature setpoints, each independently opt-in by presence exactly like
  // every chip and alert. Unlike the alert/chip catalogues these aren't
  // read-only, so unlike a chip's flat text this row also has to reflect
  // whether the underlying entity currently allows a tap (unavailable, or the
  // card is locked). Returns whether it rendered anything, so `_renderMore`
  // knows whether the disclosure has a reason to exist.
  _renderSettings(hass, c, locked) {
    const el = this._els.settingsPanel;
    const rows = [];

    if (c.summer_mode_entity) {
      const st = hass.states[c.summer_mode_entity];
      const unavailable = this._isUnavailable(hass, c.summer_mode_entity);
      const on = st ? this._isOn(st) : false;
      rows.push(`
        <div class="settings-row">
          <div>
            <div class="settings-label">Summer bypass</div>
            <div class="settings-sub">Menu 2 · Enable Bypass</div>
          </div>
          <button
            class="switch${on ? " on" : ""}"
            data-toggle="summer_mode_entity"
            title="${on ? "On" : "Off"} — tap to ${on ? "disable" : "enable"} the summer bypass"
            ${locked || unavailable ? "disabled" : ""}
          ></button>
        </div>
      `);
    }

    for (const def of SETTINGS_NUMBERS) {
      const entity = c[def.key];
      if (!entity) continue;
      const st = hass.states[entity];
      const unavailable = !st || st.state === "unavailable" || st.state === "unknown";
      const attrs = (st && st.attributes) || {};
      const min = attrs.min ?? def.fallbackMin;
      const max = attrs.max ?? def.fallbackMax;
      const step = attrs.step ?? def.fallbackStep;
      const unit = attrs.unit_of_measurement || def.fallbackUnit;
      const value = unavailable ? null : Number(st.state);
      const atMin = value !== null && value <= min;
      const atMax = value !== null && value >= max;
      rows.push(`
        <div class="settings-row">
          <div>
            <div class="settings-label">${this._esc(def.label)}</div>
            <div class="settings-sub">${this._esc(def.sub)}</div>
          </div>
          <div class="stepper">
            <button
              data-step="${def.key}" data-dir="-1"
              title="Decrease ${this._esc(def.label.toLowerCase())} by ${step}${unit}"
              ${locked || unavailable || atMin ? "disabled" : ""}
            >&minus;</button>
            <span class="stepper-val">${unavailable ? "—" : this._esc(value) + unit}</span>
            <button
              data-step="${def.key}" data-dir="1"
              title="Increase ${this._esc(def.label.toLowerCase())} by ${step}${unit}"
              ${locked || unavailable || atMax ? "disabled" : ""}
            >+</button>
          </div>
        </div>
      `);
    }

    el.innerHTML = rows.join("");
    el.style.display = rows.length ? "" : "none";
    return rows.length > 0;
  }

  _onSettingsToggle(key) {
    const entity = this._config[key];
    if (!entity || !this._hass) return;
    this._hass.callService("switch", "toggle", { entity_id: entity });
  }

  // Not optimistic, deliberately: like airflow_mode (select.py), these
  // numbers reflect only what the unit itself confirms, not a locally
  // predicted value, so the stepper's displayed value comes straight from
  // `hass.states` on the next render rather than being nudged client-side.
  _onSettingsStep(key, dir) {
    const entity = this._config[key];
    if (!entity || !this._hass) return;
    const st = this._hass.states[entity];
    if (!st) return;
    const attrs = st.attributes || {};
    const min = attrs.min ?? -Infinity;
    const max = attrs.max ?? Infinity;
    const step = attrs.step ?? 1;
    const next = Math.min(max, Math.max(min, Number(st.state) + dir * step));
    this._hass.callService("number", "set_value", { entity_id: entity, value: next });
  }

  // Maintenance actions. Each is a plain button press on the component side,
  // where a sequence performs whatever held key combination the operation
  // actually needs -- see the README's "Button combos" section for why the card
  // presses these rather than trying to hold keys itself.
  _renderActions(hass, c, locked) {
    const defs = ACTION_BUTTONS.filter((d) => c[d.key]);
    const el = this._els.actions;

    if (!defs.length) {
      if (this._actionSig !== "off") {
        el.innerHTML = "";
        el.style.display = "none";
        this._actionSig = "off";
      }
      return;
    }

    const sig = `${defs.map((d) => d.key).join("")}|${this._armedAction || ""}|${locked ? 1 : 0}`;
    if (sig === this._actionSig) return;
    this._actionSig = sig;

    el.innerHTML = defs
      .map((d) => {
        const armed = this._armedAction === d.key;
        const unavailable = this._isUnavailable(hass, c[d.key]);
        const disabled = locked || unavailable;
        return `
          <button class="action${armed ? " armed" : ""}" data-action="${d.key}" title="${this._esc(d.title)}"${
          disabled ? " disabled" : ""
        }>
            <svg viewBox="0 0 24 24" width="14" height="14">${this._iconPath(d.icon)}</svg>
            <span>${this._esc(armed ? "Confirm?" : d.label)}</span>
          </button>
        `;
      })
      .join("");
    el.style.display = "";
  }

  // Two-step for anything flagged `confirm`. The first tap only arms the
  // button; nothing is sent. This stands in for the deliberateness the physical
  // remote gets from requiring a 5.5 second two-key hold before it will restart
  // the filter countdown -- an operation with no way back from software.
  _onAction(key) {
    const def = ACTION_BUTTONS.find((d) => d.key === key);
    if (!def || !this._hass) return;
    const entity = this._config[key];
    if (!entity) return;

    if (def.confirm && this._armedAction !== key) {
      this._disarmAction();
      this._armedAction = key;
      this._actionSig = null;
      this._armTimer = setTimeout(() => {
        this._armedAction = null;
        this._armTimer = null;
        this._actionSig = null;
        this._render();
      }, CONFIRM_WINDOW_MS);
      this._render();
      return;
    }

    this._disarmAction();
    this._hass.callService("button", "press", { entity_id: entity });
    this._render();
  }

  _disarmAction() {
    if (this._armTimer) clearTimeout(this._armTimer);
    this._armTimer = null;
    this._armedAction = null;
    this._actionSig = null;
  }

  _isUnavailable(hass, entityId) {
    if (!entityId) return true;
    const st = hass.states[entityId];
    return !st || st.state === "unavailable";
  }

  _isOn(state) {
    return state && (state.state === "on" || state.state === "true" || Number(state.state) > 0);
  }

  _isRunningValue(state) {
    if (!state) return false;
    if (state.state === "on") return true;
    const n = Number(state.state);
    return !Number.isNaN(n) && n > 0;
  }

  // Numeric readouts, in the order given by `chips:` (or DEFAULT_CHIP_ORDER
  // when that key is absent). Filter life is not in the default set -- the
  // useful signal is "change it", which the alert rail carries, rather than a
  // count of hours nobody reads -- but it is available as the `filter_hours`
  // id for anyone who wants the countdown on screen permanently.
  //
  // Listing an id is necessary but not sufficient: the chip still needs its
  // entity configured and reporting, so the original opt-in-by-presence rule
  // survives underneath the grouping. Each qualifying chip's CHIP_CATALOGUE
  // `group` then sends it into the Climate or System row (each of which wraps
  // on its own rather than the whole card clipping one long line), except
  // `boost_remaining`, which has no group and goes to the header badge next
  // to the airflow percentage -- it is live, not a diagnostics-page reading,
  // so it doesn't belong in either group.
  _renderChips(hass, c) {
    // Anything read from a diagnostic page comes off the ~15 minute scrape, not
    // the live status frames, so it can legitimately be a quarter of an hour
    // old. Rather than clutter the chip, say when it was last refreshed in the
    // tooltip.
    const scrapeSt = c.diagnostics_updated_entity ? hass.states[c.diagnostics_updated_entity] : null;
    const scrapedAt =
      scrapeSt && scrapeSt.state !== "unavailable" && scrapeSt.state !== "unknown" ? scrapeSt.state : "";

    const climateChips = [];
    const systemChips = [];
    let boostBadgeHtml = "";

    for (const id of this._orderedIds(c.chips, DEFAULT_CHIP_ORDER, CHIP_CATALOGUE, "chips")) {
      const d = CHIP_CATALOGUE[id];
      const entity = c[d.key];
      if (!entity) continue;
      const st = hass.states[entity];
      if (!st || st.state === "unavailable" || st.state === "unknown") continue;
      if (d.hideWhenZero && !(Number(st.state) > 0)) continue;
      // Units are read straight from the entity when Home Assistant knows them
      // (e.g. ESPHome's unit_of_measurement), so this works whether your sensor
      // reports Celsius, Fahrenheit, minutes or percent.
      const unit = (st.attributes && st.attributes.unit_of_measurement) || d.fallbackUnit || "";
      const title = d.stale && scrapedAt ? `${d.label} — updated ${scrapedAt}` : d.label;
      const valueText = `${this._esc(st.state)}${unit ? " " + this._esc(unit) : ""}`;

      if (!d.group) {
        boostBadgeHtml = `
          <span class="boost-badge" title="${this._esc(title)}">
            <svg viewBox="0 0 24 24" width="12" height="12">${this._iconPath(d.icon)}</svg>${valueText}
          </span>
        `;
        continue;
      }

      const bucket = d.group === "climate" ? climateChips : systemChips;
      bucket.push(`
        <div class="chip" title="${this._esc(title)}">
          <svg viewBox="0 0 24 24" width="12" height="12">${this._iconPath(d.icon)}</svg>
          <span>${valueText}</span>
        </div>
      `);
    }

    this._els.boostBadge.innerHTML = boostBadgeHtml;
    this._els.boostBadge.style.display = boostBadgeHtml ? "" : "none";

    this._els.chipsClimateRow.innerHTML = climateChips.join("");
    this._els.chipsClimateGroup.style.display = climateChips.length ? "" : "none";
    this._els.chipsSystemRow.innerHTML = systemChips.join("");
    this._els.chipsSystemGroup.style.display = systemChips.length ? "" : "none";
    this._els.chips.style.display = climateChips.length || systemChips.length ? "" : "none";
  }

  // Chips and alerts are assembled as HTML strings, so anything sourced from an
  // entity state, unit or attribute has to be escaped on the way in.
  _esc(s) {
    return String(s ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  // ------------------------------------------------------------- actions --

  _onPress(key, ev) {
    if (ev) ev.preventDefault();
    const entity = this._config[`${key}_button`];
    if (!entity || this._isUnavailable(this._hass, entity)) return;

    this._pulse(this._els[key]);
    this._callButtonPress(entity);

    // Fast-scroll on hold, matching the physical remote's >2s hold behaviour
    if (key === "up" || key === "down") {
      this._clearHold();
      this._holdTimer = setTimeout(() => {
        this._holdInterval = setInterval(() => {
          this._pulse(this._els[key], true);
          this._callButtonPress(entity);
        }, FAST_SCROLL_REPEAT_MS);
      }, FAST_SCROLL_DELAY_MS);
    }
  }

  _clearHold() {
    if (this._holdTimer) clearTimeout(this._holdTimer);
    if (this._holdInterval) clearInterval(this._holdInterval);
    this._holdTimer = null;
    this._holdInterval = null;
  }

  _pulse(el, quick) {
    el.classList.remove("pressed");
    // force reflow so the animation restarts on rapid repeats
    void el.offsetWidth;
    el.classList.add("pressed");
    setTimeout(() => el.classList.remove("pressed"), quick ? 90 : 140);
  }

  _callButtonPress(entityId) {
    if (!this._hass) return;
    this._hass.callService("button", "press", { entity_id: entityId });
  }

  // ------------------------------------------------------------------ css --

  _css() {
    return `
      :host { display: block; }
      ha-card { padding: 0; overflow: hidden; background: transparent; box-shadow: none; }

      .panel {
        --accent: ${DEFAULT_ACCENT};
        position: relative;
        border-radius: 22px;
        padding: 18px 18px 20px;
        background: linear-gradient(165deg, var(--card-background-color, #fff) 0%, color-mix(in srgb, var(--card-background-color, #fff) 88%, #888) 100%);
        border: 1px solid var(--divider-color, rgba(0,0,0,.08));
        box-shadow: 0 1px 2px rgba(0,0,0,.06), 0 8px 24px -12px rgba(0,0,0,.25);
        color: var(--primary-text-color);
        font-family: var(--paper-font-body1_-_font-family, "Inter", "Roboto", sans-serif);
        transition: box-shadow .25s ease;
      }
      .panel.boost-active {
        box-shadow: 0 1px 2px rgba(0,0,0,.06), 0 0 0 2px color-mix(in srgb, var(--accent) 55%, transparent), 0 10px 28px -12px rgba(0,0,0,.3);
      }

      .header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 10px;
        padding: 0 2px;
      }
      .title {
        font-size: 13px;
        font-weight: 600;
        letter-spacing: .02em;
        color: var(--secondary-text-color);
        text-transform: uppercase;
      }
      .airflow {
        display: flex;
        align-items: center;
        gap: 6px;
      }
      .airflow-pct {
        font-size: 12px;
        font-weight: 600;
        font-variant-numeric: tabular-nums;
        color: var(--secondary-text-color);
      }
      /* Live, not diagnostic -- boost_remaining rides here instead of in the
         System group below, because it comes off the status line every tick
         rather than the ~15 minute scrape everything in that group shares. */
      .boost-badge {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        margin-left: 4px;
        padding-left: 6px;
        border-left: 1px solid var(--divider-color, rgba(0,0,0,.12));
        font-size: 11px;
        font-weight: 600;
        font-variant-numeric: tabular-nums;
        color: var(--accent);
      }
      .vent-icon {
        color: var(--secondary-text-color);
        display: flex;
        opacity: .6;
      }
      /* Duration is set inline from the airflow percentage; the literal here is
         only the fallback for configs with no airflow_entity. */
      .vent-icon.spinning svg { animation: spin var(--spin-duration, 2.4s) linear infinite; }
      .vent-icon.spinning { color: var(--accent); opacity: 1; }
      .vent-icon.spinning + .airflow-pct { color: var(--accent); }
      @keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
      @media (prefers-reduced-motion: reduce) {
        .vent-icon.spinning svg { animation: none; }
      }

      .bezel {
        border-radius: 16px;
        padding: 10px;
        background: linear-gradient(165deg, #2b2f2c, #141614);
        box-shadow: inset 0 2px 5px rgba(0,0,0,.6), inset 0 -1px 0 rgba(255,255,255,.04);
      }
      .lcd {
        position: relative;
        border-radius: 10px;
        background: radial-gradient(120% 160% at 15% 0%, #0f2018 0%, #060a08 70%);
        padding: 12px 14px;
        overflow: hidden;
      }
      .lcd-line {
        font-family: "VT323", "Share Tech Mono", "Consolas", monospace;
        font-size: 24px;
        line-height: 1.28;
        letter-spacing: .03em;
        color: var(--accent);
        text-shadow: 0 0 6px color-mix(in srgb, var(--accent) 70%, transparent), 0 0 14px color-mix(in srgb, var(--accent) 35%, transparent);
        white-space: pre;
        overflow: hidden;
        text-overflow: clip;
      }
      .lcd-scanline {
        position: absolute;
        inset: 0;
        pointer-events: none;
        background: repeating-linear-gradient(to bottom, rgba(255,255,255,.035) 0px, rgba(255,255,255,.035) 1px, transparent 1px, transparent 3px);
        mix-blend-mode: overlay;
      }

      /* The alert rail shares the chip pill styling so the two rows read as one
         family; it is hidden outright (not just emptied) when nothing is wrong,
         so a healthy card carries no stray gap above the chips.

         The rail keeps wrapping: alerts are exceptional and rare, and a pill
         like "Frost protection - Airflow 85% / 115%" is carrying detail that
         would be lost to an ellipsis. */
      .alerts {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
        margin-top: 10px;
      }
      /* Two labelled groups -- Climate, System -- each wrapping on its own
         line rather than one row that clips at the card edge. A config with a
         dozen chips enabled grows taller, never narrower than its content:
         the old single-row-with-ellipsis had no good answer for that, this
         does. */
      .chips {
        display: flex;
        flex-direction: column;
        gap: 12px;
        margin-top: 10px;
      }
      .chip-group-label {
        font-size: 10px;
        font-weight: 700;
        letter-spacing: .08em;
        text-transform: uppercase;
        color: var(--secondary-text-color);
        opacity: .68;
        margin: 0 0 6px 2px;
      }
      .chip-group-row {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
      }
      .chip span {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }
      .chip {
        min-width: 0;
        display: inline-flex;
        align-items: center;
        gap: 5px;
        font-size: 11.5px;
        font-weight: 600;
        padding: 4px 8px 4px 6px;
        border-radius: 999px;
        background: var(--secondary-background-color, rgba(0,0,0,.05));
        color: var(--secondary-text-color);
      }
      /* Mixed against --primary-text-color rather than a fixed hex so the amber
         darkens on light themes and lightens on dark ones. The old fixed #b45309
         was near-invisible on a dark card, which matters now that this pill is
         the only filter-change indicator on the card. */
      .chip.warn {
        color: color-mix(in srgb, #f59e0b 62%, var(--primary-text-color, #000));
        background: color-mix(in srgb, #f59e0b 18%, transparent);
      }
      /* Bypass is accent-tinted rather than amber: an open bypass in summer is
         the unit behaving correctly, so it must not read as a fault. */
      .chip.info {
        color: color-mix(in srgb, var(--accent) 70%, var(--primary-text-color));
        background: color-mix(in srgb, var(--accent) 16%, transparent);
      }
      /* Red, above amber, for the things that are actually broken: a sensor or
         24V rail fault, or a dead link. Mixed against --primary-text-color for
         the same light/dark legibility reason as .chip.warn above. */
      .chip.alarm {
        color: color-mix(in srgb, #ef4444 62%, var(--primary-text-color, #000));
        background: color-mix(in srgb, #ef4444 18%, transparent);
      }

      /* A sequence can run for ~25-30s. This sits under the LCD and is the only
         thing on the card that says "still working" during it -- without it a
         mode change looks like a tap that did nothing.

         Space is reserved permanently (never display:none) and only visibility
         toggles via .on -- appearing/disappearing by display would insert or
         remove its margin+height from the flow and shove every row below it
         (alerts/chips/mode-row/buttons/actions) down and back up on every tap. */
      .busy-bar {
        margin-top: 8px;
        height: 2px;
        border-radius: 2px;
        overflow: hidden;
        background: color-mix(in srgb, var(--accent) 18%, transparent);
        visibility: hidden;
      }
      .busy-bar.on {
        visibility: visible;
      }
      .busy-bar span {
        display: block;
        height: 100%;
        width: 40%;
        border-radius: 2px;
        background: var(--accent);
        animation: busy-slide 1.4s ease-in-out infinite;
      }
      .busy-bar:not(.on) span {
        animation-play-state: paused;
      }
      @keyframes busy-slide {
        0%   { transform: translateX(-100%); }
        100% { transform: translateX(350%); }
      }
      @media (prefers-reduced-motion: reduce) {
        .busy-bar span { animation: none; width: 100%; opacity: .6; }
      }

      /* The airflow-mode segmented control. Equal columns regardless of label
         length, so the row reads as one control rather than five buttons. */
      .mode-row {
        margin-top: 10px;
        display: grid;
        grid-auto-flow: column;
        grid-auto-columns: 1fr;
        gap: 4px;
        padding: 3px;
        border-radius: 12px;
        background: var(--secondary-background-color, rgba(0,0,0,.05));
      }
      .seg {
        appearance: none;
        border: none;
        cursor: pointer;
        padding: 7px 4px;
        border-radius: 9px;
        font-size: 11.5px;
        font-weight: 600;
        font-family: inherit;
        letter-spacing: .01em;
        color: var(--secondary-text-color);
        background: transparent;
        transition: background .15s ease, color .15s ease;
        -webkit-tap-highlight-color: transparent;
        touch-action: manipulation;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .seg:hover:not(:disabled):not(.active) { background: color-mix(in srgb, var(--accent) 10%, transparent); }
      .seg.active {
        color: #fff;
        background: linear-gradient(165deg, color-mix(in srgb, var(--accent) 92%, #fff 8%), color-mix(in srgb, var(--accent) 70%, #000 15%));
      }
      /* Tapped but not yet confirmed by the unit. Deliberately an outline rather
         than a fill: the select is not optimistic, so until the unit's own
         status line agrees, this is a request and not a state. */
      .seg.pending {
        color: var(--accent);
        border: 1px dashed color-mix(in srgb, var(--accent) 60%, transparent);
        padding: 6px 3px;
        animation: seg-pulse 1.6s ease-in-out infinite;
      }
      @keyframes seg-pulse {
        0%, 100% { opacity: 1; }
        50% { opacity: .55; }
      }
      @media (prefers-reduced-motion: reduce) {
        .seg.pending { animation: none; }
      }
      .seg:disabled { opacity: .4; pointer-events: none; }

      .buttons { margin-top: 16px; display: flex; flex-direction: column; gap: 10px; }

      /* Settings and maintenance actions are occasional housekeeping, not the
         everyday reason to open the card, so both live behind this disclosure
         below the remote rather than sitting open every time -- closed by
         default (_toggleMore()). The toggle only appears when there's
         something behind it (_renderMore), same opt-in-by-presence rule as
         everything else on the card. */
      .disclosure-toggle {
        margin-top: 12px;
        width: 100%;
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 6px;
        padding: 8px;
        border-radius: 12px;
        border: 1px dashed var(--divider-color, rgba(0,0,0,.14));
        background: transparent;
        color: var(--secondary-text-color);
        font-family: inherit;
        font-size: 11.5px;
        font-weight: 600;
        cursor: pointer;
        -webkit-tap-highlight-color: transparent;
        touch-action: manipulation;
      }
      .disclosure-toggle:hover { background: var(--secondary-background-color, rgba(0,0,0,.05)); color: var(--primary-text-color); }
      .disclosure-toggle svg { transition: transform .2s ease; }
      .disclosure-toggle.open svg { transform: rotate(180deg); }
      /* Animated via grid-template-rows (0fr -> 1fr) rather than max-height:
         the track sizes to the real content height either way, so there's no
         magic-number cap for a growing settings panel to outgrow. */
      .disclosure-body {
        display: grid;
        grid-template-rows: 0fr;
        transition: grid-template-rows .25s ease;
      }
      .disclosure-body.open { grid-template-rows: 1fr; }
      .disclosure-body > div { overflow: hidden; }
      .disclosure-inner { padding-top: 12px; display: flex; flex-direction: column; gap: 12px; }
      @media (prefers-reduced-motion: reduce) {
        .disclosure-body { transition: none; }
      }

      .settings {
        padding: 10px 10px 11px;
        border-radius: 14px;
        background: var(--secondary-background-color, rgba(0,0,0,.05));
        display: flex;
        flex-direction: column;
        gap: 11px;
      }
      .settings-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
      .settings-label { font-size: 12px; font-weight: 600; color: var(--primary-text-color); }
      .settings-sub { font-size: 10.5px; color: var(--secondary-text-color); margin-top: 1px; }

      .switch {
        appearance: none;
        border: none;
        cursor: pointer;
        width: 36px;
        height: 21px;
        border-radius: 999px;
        background: var(--divider-color, rgba(0,0,0,.16));
        position: relative;
        flex-shrink: 0;
        transition: background .15s ease;
        -webkit-tap-highlight-color: transparent;
        touch-action: manipulation;
      }
      .switch::after {
        content: "";
        position: absolute;
        top: 2px;
        left: 2px;
        width: 17px;
        height: 17px;
        border-radius: 50%;
        background: #fff;
        box-shadow: 0 1px 2px rgba(0,0,0,.3);
        transition: left .15s ease;
      }
      .switch.on { background: var(--accent); }
      .switch.on::after { left: 17px; }
      .switch:disabled { opacity: .4; pointer-events: none; }

      .stepper { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }
      .stepper button {
        appearance: none;
        cursor: pointer;
        width: 24px;
        height: 24px;
        border-radius: 8px;
        border: 1px solid var(--divider-color, rgba(0,0,0,.12));
        background: var(--card-background-color, #fff);
        color: var(--primary-text-color);
        font-size: 14px;
        font-weight: 700;
        line-height: 1;
        display: flex;
        align-items: center;
        justify-content: center;
        -webkit-tap-highlight-color: transparent;
        touch-action: manipulation;
      }
      .stepper button:disabled { opacity: .35; pointer-events: none; }
      .stepper-val {
        min-width: 42px;
        text-align: center;
        font-size: 12.5px;
        font-weight: 700;
        font-variant-numeric: tabular-nums;
        color: var(--primary-text-color);
      }

      /* Maintenance actions share the disclosure with settings -- both are
         occasional housekeeping, and the remote above is what the card is for. */
      .actions {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
      }
      .action {
        appearance: none;
        cursor: pointer;
        display: inline-flex;
        align-items: center;
        gap: 5px;
        font-family: inherit;
        font-size: 11.5px;
        font-weight: 600;
        padding: 5px 10px 5px 8px;
        border-radius: 999px;
        color: var(--secondary-text-color);
        background: transparent;
        border: 1px solid var(--divider-color, rgba(0,0,0,.12));
        transition: background .15s ease, color .15s ease, border-color .15s ease;
        -webkit-tap-highlight-color: transparent;
        touch-action: manipulation;
      }
      .action:hover:not(:disabled) { background: var(--secondary-background-color, rgba(0,0,0,.05)); }
      .action:disabled { opacity: .35; pointer-events: none; }
      /* Armed for its confirming second tap. Amber and unmistakable, because
         the only action that uses it cannot be undone. */
      .action.armed {
        color: color-mix(in srgb, #f59e0b 62%, var(--primary-text-color, #000));
        background: color-mix(in srgb, #f59e0b 18%, transparent);
        border-color: color-mix(in srgb, #f59e0b 45%, transparent);
      }

      .btn {
        appearance: none;
        border: none;
        cursor: pointer;
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 8px;
        color: var(--primary-text-color);
        background: var(--secondary-background-color, #f2f2f2);
        border: 1px solid var(--divider-color, rgba(0,0,0,.06));
        transition: transform .08s ease, box-shadow .08s ease, background .15s ease;
        -webkit-tap-highlight-color: transparent;
        user-select: none;
        touch-action: manipulation;
      }
      .btn:active, .btn.pressed {
        transform: scale(.94);
      }
      .btn.disabled { opacity: .35; pointer-events: none; }

      .btn-boost {
        width: 100%;
        height: 46px;
        border-radius: 14px;
        font-size: 13px;
        font-weight: 700;
        letter-spacing: .08em;
        text-transform: uppercase;
        color: #fff;
        background: linear-gradient(165deg, color-mix(in srgb, var(--accent) 92%, #fff 8%), color-mix(in srgb, var(--accent) 70%, #000 15%));
        box-shadow: 0 4px 10px -4px color-mix(in srgb, var(--accent) 70%, transparent);
      }
      .btn-boost.active {
        box-shadow: 0 0 0 3px color-mix(in srgb, var(--accent) 35%, transparent), 0 4px 14px -4px color-mix(in srgb, var(--accent) 80%, transparent);
        animation: boost-pulse 1.6s ease-in-out infinite;
      }
      @keyframes boost-pulse {
        0%, 100% { filter: brightness(1); }
        50% { filter: brightness(1.15); }
      }
      .btn-boost.pressed { transform: scale(.97); }

      .nav-row { display: grid; grid-template-columns: 1fr 1.15fr 1fr; gap: 10px; }
      .btn-nav {
        height: 52px;
        border-radius: 14px;
      }
      .btn-select {
        height: 52px;
        border-radius: 14px;
        background: linear-gradient(165deg, var(--card-background-color,#fff), var(--secondary-background-color,#eee));
        color: var(--accent);
        border: 1.5px solid color-mix(in srgb, var(--accent) 45%, var(--divider-color, #ddd));
      }
      .btn svg { display: block; }

      /* A dead link is otherwise indistinguishable from an idle unit. Draining
         the colour out of the panel says "this is not live" faster than any pill
         can, and the pill then says why.

         Applied to the children rather than to .panel itself, and skipping the
         alert rail: a CSS filter on an ancestor cannot be cancelled by a
         descendant, so greying the whole panel would take the red "MVHR
         offline" pill down with it -- the one thing on the card that still
         needs to be legible. */
      .panel.offline > *:not(.alerts) {
        filter: grayscale(.85);
        opacity: .78;
      }

      .panel.force-dark { color-scheme: dark; }
      .panel.force-light { color-scheme: light; }
    `;
  }
}

class SentinelRemoteCardEditor extends HTMLElement {
  setConfig(config) {
    this._config = config;
    this._render();
  }
  set hass(hass) {
    this._hass = hass;
  }
  _render() {
    if (!this._config) return;
    this.innerHTML = `
      <div style="padding:12px;font-size:13px;line-height:1.5;">
        <p><b>sentinel-remote-card</b> uses a YAML-only configuration. Switch this card to
        <i>Edit in YAML</i> and see the README for the full option list.</p>
        <p><b>Required:</b> line1_entity / line2_entity (or display_entity), boost_button,
        down_button, select_button, up_button.</p>
        <p><b>Controls:</b> airflow_mode_entity (the Normal / boost / Purge segmented
        row — this is where purge lives), refresh_diagnostics_button,
        refresh_settings_button, sync_clock_button, reset_filter_button.</p>
        <p><b>Chips (Climate / System groups):</b> supply_temp_entity, extract_temp_entity,
        indoor_temp_entity, humidity_entity, humidity_avg_entity, co2_entity,
        supply_rpm_entity, extract_rpm_entity, supply_pwm_entity, extract_pwm_entity,
        filter_entity, diagnostics_updated_entity. boost_remaining_entity rides in the
        header next to the airflow badge instead of a group.</p>
        <p><b>Alerts:</b> link_entity, bypass_entity, antifrost_entity,
        antifrost_mode_entity, defrost_entity, dryout_entity, humidity_boost_entity,
        purge_entity, switched_live_entity, filter_due_entity, filter_warning_threshold,
        supply_fault_entity, extract_fault_entity, rail_fault_entity.</p>
        <p><b>Settings (behind the "More settings" disclosure):</b> summer_mode_entity,
        bypass_indoor_temp_entity, bypass_outdoor_temp_entity.</p>
        <p><b>Other status:</b> airflow_entity (spins the header vent glyph in
        proportion to flow), busy_entity, boost_active_entity, running_entity.</p>
        <p><b>Layout:</b> chips and alerts take ordered id lists that control which
        readouts appear and in what order.</p>
        <p><b>Appearance:</b> title, accent_color, theme.</p>
        <p>Each status entity is opt-in: name it and the chip or icon appears, omit
        the line and it is hidden. Settings and maintenance actions share one
        disclosure that only appears once something is configured behind it.</p>
      </div>
    `;
  }
}

if (!customElements.get(CARD_TAG)) {
  customElements.define(CARD_TAG, SentinelRemoteCard);
}
if (!customElements.get(EDITOR_TAG)) {
  customElements.define(EDITOR_TAG, SentinelRemoteCardEditor);
}

window.customCards = window.customCards || [];
window.customCards.push({
  type: CARD_TAG,
  name: "Sentinel Remote Card",
  description: "A modern take on the Vent-Axia Sentinel wired remote for MVHR control.",
  preview: false,
});
