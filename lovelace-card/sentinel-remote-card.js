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
 * Beyond the remote itself the card carries three status surfaces, all optional:
 *   - the header vent glyph, which spins at a rate proportional to airflow_entity
 *   - an alert rail that renders nothing at all until summer bypass opens or the
 *     filter needs changing, so a healthy unit shows a quiet card
 *   - a chip row of numeric readouts (supply/extract air temperature, and so on)
 *
 * Every status entity is opt-in by presence: name the entity in the config and
 * the feature appears, omit it and it disappears. There are no separate show/hide
 * flags -- deleting the config line is the off switch.
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
              <span class="airflow-pct" id="airflowPct"></span>
            </div>
          </div>

          <div class="bezel">
            <div class="lcd">
              <div class="lcd-line" id="l1">&nbsp;</div>
              <div class="lcd-line" id="l2">&nbsp;</div>
              <div class="lcd-scanline"></div>
            </div>
          </div>

          <div class="alerts" id="alerts"></div>
          <div class="chips" id="chips"></div>

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
        </div>
      </ha-card>
    `;

    this._els = {
      panel: root.querySelector(".panel"),
      title: root.querySelector(".title"),
      vent: root.querySelector(".vent-icon"),
      airflowPct: root.querySelector("#airflowPct"),
      l1: root.querySelector("#l1"),
      l2: root.querySelector("#l2"),
      alerts: root.querySelector("#alerts"),
      chips: root.querySelector("#chips"),
      boost: root.querySelector('[data-key="boost"]'),
      down: root.querySelector('[data-key="down"]'),
      select: root.querySelector('[data-key="select"]'),
      up: root.querySelector('[data-key="up"]'),
    };

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

    const anyUnavailable = this._buttonUnavailable(hass, c);
    for (const key of ["boost", "down", "select", "up"]) {
      const entity = c[`${key}_button`];
      const disabled = !entity || this._isUnavailable(hass, entity);
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
  // something.
  _renderAlerts(hass, c) {
    const pills = [];

    const bypass = c.bypass_entity ? hass.states[c.bypass_entity] : null;
    if (bypass && this._isOn(bypass)) {
      pills.push(
        this._pill(
          "info",
          `<path fill="currentColor" d="M12 7a5 5 0 1 0 5 5 5 5 0 0 0-5-5Zm0-5 1.8 3.1h-3.6L12 2Zm0 20-1.8-3.1h3.6L12 22ZM2 12l3.1-1.8v3.6L2 12Zm20 0-3.1 1.8v-3.6L22 12Z"/>`,
          "Summer bypass open",
          "Bypass"
        )
      );
    }

    const antifrost = this._antifrostAlert(hass, c);
    if (antifrost) {
      pills.push(this._pill("info", SNOWFLAKE_ICON, antifrost.title, antifrost.label));
    }

    const filter = this._filterAlert(hass, c);
    if (filter) {
      pills.push(
        this._pill(
          "warn",
          `<path fill="currentColor" d="M12 2 1 21h22L12 2Zm1 14h-2v2h2v-2Zm0-7h-2v5h2V9Z"/>`,
          filter.title,
          filter.label
        )
      );
    }

    this._els.alerts.innerHTML = pills.join("");
    this._els.alerts.style.display = pills.length ? "" : "none";
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

  _buttonUnavailable(hass, c) {
    return ["boost", "down", "select", "up"].every((k) => this._isUnavailable(hass, c[`${k}_button`]));
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

  // Numeric readouts. Filter life is deliberately absent -- it lives on the alert
  // rail now, because the useful signal is "change it" rather than a count of
  // hours nobody reads. Everything here is opt-in by presence of the config key.
  _renderChips(hass, c) {
    const defs = [
      {
        key: "supply_temp_entity",
        icon: "M12 3v10.2l-3.6-3.6L7 11l5 5 5-5-1.4-1.4-3.6 3.6V3h-2ZM4 19h16v2H4v-2Z",
        label: "Supply air (to house)",
        fallbackUnit: "°C",
        stale: true,
      },
      {
        key: "extract_temp_entity",
        icon: "M12 21V10.8l3.6 3.6L17 13l-5-5-5 5 1.4 1.4L12 10.8V21h-2 4-2ZM4 3h16v2H4V3Z",
        label: "Extract air (from house)",
        fallbackUnit: "°C",
        stale: true,
      },
      { key: "humidity_entity", icon: "M12 2s6 7.2 6 11.5a6 6 0 1 1-12 0C6 9.2 12 2 12 2Z", label: "Humidity", fallbackUnit: "%" },
      { key: "co2_entity", icon: "M12 2a5 5 0 0 0-5 5c0 3 5 9 5 9s5-6 5-9a5 5 0 0 0-5-5Z", label: "CO2", fallbackUnit: "ppm" },
      {
        key: "boost_remaining_entity",
        icon: "M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2Zm1 10.41V6h-2v7.41l5.29 5.3 1.42-1.42Z",
        label: "Boost ends in",
        fallbackUnit: "min",
        hideWhenZero: true,
      },
    ];

    // The air temperatures come off the ~15 minute diagnostics scrape, not the
    // live status frames, so they can legitimately be a quarter of an hour old.
    // Rather than clutter the chip, say when it was last refreshed in the tooltip.
    const scrapeSt = c.diagnostics_updated_entity ? hass.states[c.diagnostics_updated_entity] : null;
    const scrapedAt =
      scrapeSt && scrapeSt.state !== "unavailable" && scrapeSt.state !== "unknown" ? scrapeSt.state : "";

    const chips = [];
    for (const d of defs) {
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
      chips.push(`
        <div class="chip" title="${this._esc(title)}">
          <svg viewBox="0 0 24 24" width="12" height="12"><path fill="currentColor" d="${d.icon}"/></svg>
          <span>${this._esc(st.state)}${unit ? " " + this._esc(unit) : ""}</span>
        </div>
      `);
    }
    this._els.chips.innerHTML = chips.join("");
    this._els.chips.style.display = chips.length ? "" : "none";
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
         so a healthy card carries no stray gap above the chips. */
      .alerts, .chips {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
        margin-top: 10px;
      }
      .chip {
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

      .buttons { margin-top: 16px; display: flex; flex-direction: column; gap: 10px; }

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
        <p><b>Optional status:</b> airflow_entity (spins the header vent glyph in
        proportion to flow), bypass_entity, antifrost_entity and filter_due_entity
        (the alert rail), antifrost_mode_entity (detail text for the antifrost
        pill), supply_temp_entity, extract_temp_entity, humidity_entity, co2_entity,
        boost_remaining_entity, filter_entity, filter_warning_threshold,
        diagnostics_updated_entity, boost_active_entity, running_entity.</p>
        <p><b>Appearance:</b> title, accent_color, theme.</p>
        <p>Each status entity is opt-in: name it and the chip or icon appears, omit
        the line and it is hidden.</p>
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
