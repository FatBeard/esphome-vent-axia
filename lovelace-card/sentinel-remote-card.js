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
 * Install:
 *   1. Copy this file to <config>/www/sentinel-remote-card.js
 *   2. Settings -> Dashboards -> Resources -> Add Resource
 *        URL: /local/sentinel-remote-card.js   Type: JavaScript Module
 *   3. Add a card with type: custom:sentinel-remote-card (see README for options)
 */

const CARD_TAG = "sentinel-remote-card";
const EDITOR_TAG = "sentinel-remote-card-editor";

const DEFAULT_ACCENT = "#3ddc84"; // LCD phosphor green, matches the original unit
const FAST_SCROLL_DELAY_MS = 550; // matches "hold >2s" spec, felt snappier at ~0.55s repeat start
const FAST_SCROLL_REPEAT_MS = 160;

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
          </div>

          <div class="bezel">
            <div class="lcd">
              <div class="lcd-line" id="l1">&nbsp;</div>
              <div class="lcd-line" id="l2">&nbsp;</div>
              <div class="lcd-scanline"></div>
            </div>
          </div>

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
      l1: root.querySelector("#l1"),
      l2: root.querySelector("#l2"),
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

    // Spin the vent glyph gently when a fan-speed/running entity says the unit is active
    const running =
      c.running_entity && hass.states[c.running_entity]
        ? this._isRunningValue(hass.states[c.running_entity])
        : boostActive;
    this._els.vent.classList.toggle("spinning", !!running);

    this._renderChips(hass, c);
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

  _renderChips(hass, c) {
    const defs = [
      { key: "humidity_entity", icon: "M12 2s6 7.2 6 11.5a6 6 0 1 1-12 0C6 9.2 12 2 12 2Z", label: "Humidity", fallbackUnit: "%" },
      { key: "filter_entity", icon: "M4 4h16v4H4zm0 6h16v4H4zm0 6h10v4H4z", label: "Filter life", fallbackUnit: "" },
      { key: "co2_entity", icon: "M12 2a5 5 0 0 0-5 5c0 3 5 9 5 9s5-6 5-9a5 5 0 0 0-5-5Z", label: "CO2", fallbackUnit: "ppm" },
      {
        key: "boost_remaining_entity",
        icon: "M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2Zm1 10.41V6h-2v7.41l5.29 5.3 1.42-1.42Z",
        label: "Boost ends in",
        fallbackUnit: "min",
        hideWhenZero: true,
      },
    ];
    const chips = [];
    for (const d of defs) {
      const entity = c[d.key];
      if (!entity) continue;
      const st = hass.states[entity];
      if (!st || st.state === "unavailable" || st.state === "unknown") continue;
      if (d.hideWhenZero && !(Number(st.state) > 0)) continue;
      // Units are read straight from the entity when Home Assistant knows them
      // (e.g. ESPHome's unit_of_measurement), so this works whether your filter
      // sensor reports hours, days, or percent-remaining.
      const unit = (st.attributes && st.attributes.unit_of_measurement) || d.fallbackUnit || "";
      let warn = false;
      if (d.key === "filter_entity") {
        const defaultThreshold = unit === "h" ? 336 : unit === "d" ? 14 : 14;
        const threshold = c.filter_warning_threshold ?? defaultThreshold;
        warn = Number(st.state) <= threshold;
      }
      chips.push(`
        <div class="chip ${warn ? "warn" : ""}" title="${d.label}">
          <svg viewBox="0 0 24 24" width="12" height="12"><path fill="currentColor" d="${d.icon}"/></svg>
          <span>${st.state}${unit ? " " + unit : ""}</span>
        </div>
      `);
    }
    this._els.chips.innerHTML = chips.join("");
    this._els.chips.style.display = chips.length ? "" : "none";
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
      .vent-icon {
        color: var(--secondary-text-color);
        display: flex;
        opacity: .6;
      }
      .vent-icon.spinning svg { animation: spin 2.4s linear infinite; }
      .vent-icon.spinning { color: var(--accent); opacity: 1; }
      @keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }

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

      .chips {
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
      .chip.warn { color: #b45309; background: color-mix(in srgb, #f59e0b 18%, transparent); }

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
        <i>Edit in YAML</i> and see the README for the full option list
        (line1_entity / line2_entity or display_entity, boost_button, down_button,
        select_button, up_button, plus optional humidity_entity, filter_entity,
        boost_active_entity, running_entity, accent_color, theme).</p>
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
