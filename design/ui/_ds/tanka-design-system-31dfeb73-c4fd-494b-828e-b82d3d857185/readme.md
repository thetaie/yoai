# Tanka Design System

> The world's first AI messenger with **long-term memory** for teams.
> *"The future belongs to organizations that remember."*

Tanka is an AI-powered team messenger that turns scattered conversations across
Slack, WhatsApp, Telegram, Outlook and Gmail into structured, durable memory.
Built on a proprietary neuroscience-inspired memory architecture
(**MemUnit → MemGraph → MemOrg**), it delivers context-aware Smart Replies,
proactive reminders, and an AI Assistant in every chat — your team's *second brain*.

- **Company:** Tanka (backed by Shanda Group; Chen Institute / TCCI research lineage)
- **Category:** AI team messaging / collaboration · "memory-first productivity"
- **Positioning vs:** Slack, Microsoft Teams — but with persistent organizational memory
- **Core pillars:** AI Long-Term Memory · Smart Reply · Universal Communications Hub · AI Assistant

---

## Sources used to build this system

This system was derived from brand exports — **no product source code was provided**,
so UI-kit recreations are faithful interpretations of Tanka's public product (a
chat/messenger surface) styled with the real tokens, not reverse-engineered code.

| Source | Location | What it gave us |
|---|---|---|
| Framer color styles backup | `uploads/framer-colors-backup_2026-06-22_03-18-21.json` (59 styles) | Full color system — core, theme ramp, gray ramp, semantic |
| Framer text styles backup | `uploads/framer-textstyles-backup_2026-06-22_03-18-36.json` (16 styles) | Type scales for Inter / DM Serif Display / IBM Plex Sans |
| Logo mark | `uploads/BUz8gqFP46PLYMJVEVrplvttTg.svg` → `assets/tanka-mark.svg` | The stacked-bar speech-bubble logo |
| tanka.ai (public) | web | Product description, tone, feature names, positioning |

> ⚠️ **Font binaries were not provided.** Inter, DM Serif Display, IBM Plex Sans and
> IBM Plex Mono are all on Google Fonts and are loaded via CDN in `tokens/fonts.css`.
> For production/offline use, self-host the `.woff2` files and swap the `@import`
> for `@font-face` rules. **Please confirm these are the correct families, or send
> the real font files if Tanka uses licensed cuts.**

---

## CONTENT FUNDAMENTALS

**Voice:** confident, declarative, future-facing — but practical. Tanka talks about a
real workplace pain ("Organizational Amnesia", re-explaining things, lost context) and
answers it plainly. It is *aspirational on memory* ("knowledge immortal", "enterprise
brain that grows with your team") yet *concrete on benefit* ("saved 111 workdays",
"16.5 smart replies per user per month").

- **Person:** second person to the user ("your team's second brain", "Tanka remembers
  for you"); third person for the product ("Tanka learns…", "Tanka surfaces…").
  Tanka is an active agent — verbs are *remembers, learns, surfaces, retrieves, evolves*.
- **Casing:** Sentence case everywhere — headings, buttons, nav. Product feature names
  are Title Case proper nouns: **Smart Reply**, **AI Assistant**, **Memory**,
  **Communications Hub**, **MemGraph**. Never ALL-CAPS for emphasis.
- **Sentence length:** short, punchy marketing lines often as fragments
  ("No more repeating prompts." / "Tanka remembers."). Body copy is plain and scannable.
- **Numbers:** lead with proof. Use concrete metrics (35,000+ smart replies, 30% of time
  saved, 111 workdays) rather than vague claims.
- **Emoji:** used *sparingly* in social/launch copy (💬 IM + 🤖 AI + 🧠 memory) but
  **NOT** in the product UI. Keep product surfaces emoji-free; the 🧠 "memory/brain"
  motif is the one recurring symbol, and it should be expressed with an icon, not emoji.
- **Tagline patterns:** "The world's first messenger with AI long-term memory."
  "Say goodbye to Organizational Amnesia." "Your team's second brain."
- **Avoid:** hype without a number, jargon dumps, exclamation overload in-product,
  calling the AI a "chatbot" (it's an *assistant* / *memory*).

**Example copy**
- Hero: *"The AI messenger that never forgets."*
- Sub: *"Tanka turns every conversation into memory — so your team stops re-explaining and starts moving."*
- Button: *"Get started free"* · *"Connect Slack"* · *"Ask Tanka"*
- Empty state: *"No memories yet. As your team chats, Tanka starts remembering what matters."*

---

## VISUAL FOUNDATIONS

**Overall vibe:** clean, bright, optimistic SaaS — a light-first messenger that feels
fast and trustworthy. Lots of white space, crisp 1px borders, soft rounded corners.
The energy comes from one electric blue accent plus the green→cyan→blue brand "flag."

**Color**
- **Light is the default theme**; a full dark theme exists (`[data-theme="dark"]`).
- One dominant accent: **primary `#0096F8`** (a bright, slightly cool blue). Used for
  primary buttons, links, active states, the AI assistant, focus rings.
- **Brand flag gradient** (from the logo): green `#00DF5D` → cyan `#00B9E3` →
  blue `#2D7AFF` → deep blue `#0050DC`. Reserved for brand moments — logo, hero
  accents, the AI/Memory glow, illustrations. Do **not** flood UI chrome with it.
- Neutrals are near-true-gray with a faint cool cast. Text is near-black `#040608`
  on `#FAFCFE` page / pure-white cards.
- Semantic: danger/warning red `#D64846`, success green. Status colors are muted, not neon.

**Type**
- **Inter** runs the product UI (chat, dashboard, settings) — tight tracking on
  headings (-0.02em), generous 1.5 line-height on body.
- **DM Serif Display** is the marketing display face — elegant, high-contrast serif for
  big hero headlines only. Never for UI or long body.
- **IBM Plex Sans** is marketing long-form body (blog, docs) at a relaxed 1.69 line-height.
- **IBM Plex Mono** for code, memory IDs, timestamps, metadata.

**Spacing & layout:** 4px base grid. Comfortable but not airy density in-app; generous
on marketing. App layout is a fixed left sidebar (`--sidebar-width: 280px`) + fluid
conversation column + optional right memory/context panel.

**Corner radii:** soft and friendly. Buttons/inputs ~8–12px, cards ~16px, modals ~20px,
avatars and pills fully round. Chat bubbles use `--radius-bubble` (18px with one
4px tucked corner toward the sender) — the signature shape echoing the logo's tail.

**Cards:** white surface, 1px `--border` (#DEE2E6), 16px radius, soft `--shadow-sm`/`md`.
No heavy borders, no colored left-accent bars. Elevation comes from soft, low-opacity
shadows — never hard drop shadows. The primary CTA and AI assistant may carry a subtle
blue glow (`--shadow-primary`).

**Borders & dividers:** hairline 1px. Dividers are very low-opacity gray
(`--divider`), not solid lines.

**Backgrounds:** predominantly flat — `--surface-page` (#FAFCFE) and white cards.
No busy textures or photographic hero backgrounds in-product. Marketing may use a
soft single-hue wash or a faint brand-flag gradient glow behind the hero. Avoid
purple/violet gradients (off-brand) and heavy mesh gradients.

**Transparency & blur:** used lightly — sticky headers and overlays get a translucent
surface with `backdrop-filter: blur()`; the alpha theme tints (`--theme-a*`) provide
subtle hover/active washes. Modals use a dim scrim (~`rgba(0,0,1,.4)`).

**Animation:** quick and confident, never bouncy-cartoonish. Standard easing
`cubic-bezier(0.2,0,0,1)`, durations 120–320ms. Fades + small translate/scale for
entrances; messages slide up + fade in. The one expressive motion is the AI/"thinking"
state and a gentle shimmer on the memory glow. Respect `prefers-reduced-motion`.

**Hover states:** subtle background wash (`--bg-hover`) on neutral controls; primary
buttons darken slightly. **Press states:** deeper wash (`--bg-pressed`) — no scale-down
on buttons (a small scale on icon buttons is OK). Focus is always a 3px primary ring.

---

## ICONOGRAPHY

No proprietary icon set or icon-font was present in the provided exports. Tanka's
product is a clean line-icon messenger, so this system standardizes on **Lucide**
(MIT, CDN-available) — 1.5–2px stroke, rounded caps/joins, 24px grid — which matches
the bright, friendly line aesthetic. **This is a substitution; flag if Tanka uses a
specific licensed set and we'll swap it.**

- **Library:** [Lucide](https://lucide.dev) via CDN (`lucide@latest`). Stroke `1.75`,
  `currentColor`, 20–24px in UI, 16px inline.
- **Logo / brand mark:** `assets/tanka-mark.svg` — stacked green/cyan/blue bars forming a
  speech bubble with a tail. Use as-is; do not recolor the bars.
- **The memory motif:** brain / sparkle / layered-cards iconography signals AI & Memory
  features. Pair the `Sparkles` (or `BrainCircuit`) Lucide glyph with the primary blue
  or the brand gradient for AI Assistant / Smart Reply entry points.
- **Emoji:** not used in product UI. Permitted only in launch/social copy.
- **Unicode glyphs:** not used as functional icons — always real SVG icons.

---

## Index / manifest

**Root**
- `styles.css` — global entry (imports only). Consumers link this.
- `readme.md` — this file.
- `SKILL.md` — Agent-Skill wrapper.
- `tokens/` — `fonts.css`, `colors.css`, `typography.css`, `spacing.css`.
- `assets/` — `tanka-mark.svg` (logo mark). Wordmark = mark + "Tanka" set in Inter SemiBold (see Brand card).

**Foundation cards** (`guidelines/`) — Design System tab specimens for color, type, spacing, brand.

**Components** (`components/`) — reusable React primitives (see each `*.prompt.md`):
- `core/` — Button, IconButton, Badge, Tag, Avatar
- `forms/` — Input, Textarea, Select, Switch, Checkbox
- `feedback/` — Card, Tooltip, Toast, Spinner
- `chat/` — MessageBubble, SmartReply, MemoryChip

**UI kit** (`ui_kits/messenger/`) — interactive recreation of the Tanka messenger:
sidebar, conversation, composer, Smart Reply, Memory panel, AI Assistant.

Run `check_design_system` to see the live component namespace and card inventory.
