/* @ds-bundle: {"format":3,"namespace":"TankaDesignSystem_31dfeb","components":[{"name":"MemoryChip","sourcePath":"components/chat/MemoryChip.jsx"},{"name":"MessageBubble","sourcePath":"components/chat/MessageBubble.jsx"},{"name":"SmartReply","sourcePath":"components/chat/SmartReply.jsx"},{"name":"Avatar","sourcePath":"components/core/Avatar.jsx"},{"name":"Badge","sourcePath":"components/core/Badge.jsx"},{"name":"Button","sourcePath":"components/core/Button.jsx"},{"name":"IconButton","sourcePath":"components/core/IconButton.jsx"},{"name":"Card","sourcePath":"components/feedback/Card.jsx"},{"name":"Spinner","sourcePath":"components/feedback/Spinner.jsx"},{"name":"Input","sourcePath":"components/forms/Input.jsx"},{"name":"Switch","sourcePath":"components/forms/Switch.jsx"}],"sourceHashes":{"components/chat/MemoryChip.jsx":"3ee345886e08","components/chat/MessageBubble.jsx":"3997d4b2a296","components/chat/SmartReply.jsx":"ae4660788128","components/core/Avatar.jsx":"5a5d887126eb","components/core/Badge.jsx":"dfcb82ddedfd","components/core/Button.jsx":"cd8bbc5bfe1f","components/core/IconButton.jsx":"eca772970ee0","components/feedback/Card.jsx":"5842422ea21e","components/feedback/Spinner.jsx":"b986a9ce6a9e","components/forms/Input.jsx":"2b15337d96af","components/forms/Switch.jsx":"4c3490d549c8","ui_kits/messenger/ChatHeader.jsx":"292aab162b10","ui_kits/messenger/Composer.jsx":"eae92dd12198","ui_kits/messenger/Icons.jsx":"3b4cb214039f","ui_kits/messenger/MemoryPanel.jsx":"56db025e7ecb","ui_kits/messenger/Sidebar.jsx":"c7e9d25ddee6","ui_kits/messenger/data.js":"52f98a3cf2ef"},"inlinedExternals":[],"unexposedExports":[]} */

(() => {

const __ds_ns = (window.TankaDesignSystem_31dfeb = window.TankaDesignSystem_31dfeb || {});

const __ds_scope = {};

(__ds_ns.__errors = __ds_ns.__errors || []);

// components/chat/MemoryChip.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Memory chip — a recalled fact/decision surfaced from Tanka's memory graph.
 * Brand-gradient left rail + brain glyph; used in the Memory panel and inline.
 */
function MemoryChip({
  label = 'Memory',
  children,
  time = null,
  style = {},
  ...props
}) {
  return /*#__PURE__*/React.createElement("div", _extends({
    style: {
      display: 'flex',
      gap: 10,
      padding: '10px 12px',
      alignItems: 'flex-start',
      background: 'var(--surface-card)',
      border: '1px solid var(--border)',
      borderRadius: 'var(--radius-md)',
      boxShadow: 'var(--shadow-xs)',
      position: 'relative',
      overflow: 'hidden',
      ...style
    }
  }, props), /*#__PURE__*/React.createElement("span", {
    style: {
      position: 'absolute',
      left: 0,
      top: 0,
      bottom: 0,
      width: 3,
      background: 'var(--brand-gradient)'
    }
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      flex: 'none',
      width: 26,
      height: 26,
      borderRadius: 'var(--radius-sm)',
      background: 'var(--theme-a200)',
      color: 'var(--primary)'
    }
  }, /*#__PURE__*/React.createElement("svg", {
    width: "15",
    height: "15",
    viewBox: "0 0 24 24",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round",
    strokeLinejoin: "round"
  }, /*#__PURE__*/React.createElement("path", {
    d: "M12 5a3 3 0 0 0-3 3 3 3 0 0 0-1 5.8V16a2 2 0 0 0 4 0M12 5a3 3 0 0 1 3 3 3 3 0 0 1 1 5.8V16a2 2 0 0 1-4 0M12 5V19"
  }))), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      minWidth: 0
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      justifyContent: 'space-between',
      gap: 8
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: 'var(--font-ui)',
      fontSize: 11,
      fontWeight: 700,
      letterSpacing: '0.04em',
      textTransform: 'uppercase',
      color: 'var(--primary)'
    }
  }, label), time && /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: 'var(--font-mono)',
      fontSize: 11,
      color: 'var(--text-tertiary)'
    }
  }, time)), /*#__PURE__*/React.createElement("div", {
    style: {
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--ui-callout-size)',
      color: 'var(--text-primary)',
      lineHeight: 1.45,
      marginTop: 2
    }
  }, children)));
}
Object.assign(__ds_scope, { MemoryChip });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/chat/MemoryChip.jsx", error: String((e && e.message) || e) }); }

// components/chat/MessageBubble.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Chat message bubble. `me` (sent) uses primary fill on the right; otherwise a
 * neutral surface on the left. Signature tucked corner via --radius-bubble.
 */
function MessageBubble({
  me = false,
  author = null,
  time = null,
  children,
  style = {},
  ...props
}) {
  const radius = me ? '18px 18px 4px 18px' : '18px 18px 18px 4px';
  return /*#__PURE__*/React.createElement("div", _extends({
    style: {
      display: 'flex',
      flexDirection: 'column',
      alignItems: me ? 'flex-end' : 'flex-start',
      gap: 3,
      maxWidth: '72%',
      ...style
    }
  }, props), author && !me && /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: 'var(--font-ui)',
      fontSize: 12,
      fontWeight: 600,
      color: 'var(--text-secondary)',
      padding: '0 4px'
    }
  }, author), /*#__PURE__*/React.createElement("div", {
    style: {
      background: me ? 'var(--primary)' : 'var(--surface-card)',
      color: me ? '#fff' : 'var(--text-primary)',
      border: me ? '1px solid transparent' : '1px solid var(--border)',
      borderRadius: radius,
      padding: '9px 13px',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--ui-body-size)',
      lineHeight: 1.45,
      boxShadow: me ? 'none' : 'var(--shadow-xs)',
      wordBreak: 'break-word'
    }
  }, children), time && /*#__PURE__*/React.createElement("span", {
    style: {
      fontFamily: 'var(--font-ui)',
      fontSize: 11,
      color: 'var(--text-tertiary)',
      padding: '0 4px'
    }
  }, time));
}
Object.assign(__ds_scope, { MessageBubble });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/chat/MessageBubble.jsx", error: String((e && e.message) || e) }); }

// components/chat/SmartReply.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Smart Reply chip — Tanka's AI-suggested response. Outlined with a subtle
 * primary tint and a leading sparkle; click to use the suggestion.
 */
function SmartReply({
  children,
  onClick,
  style = {},
  ...props
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    onClick: onClick,
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 7,
      padding: '8px 14px',
      background: 'var(--theme-a100)',
      border: '1px solid var(--theme-a300)',
      color: 'var(--primary)',
      borderRadius: 'var(--radius-pill)',
      cursor: 'pointer',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--ui-callout-size)',
      fontWeight: 600,
      transition: 'background var(--duration-fast) var(--ease-standard)',
      ...style
    },
    onMouseEnter: e => {
      e.currentTarget.style.background = 'var(--theme-a200)';
    },
    onMouseLeave: e => {
      e.currentTarget.style.background = 'var(--theme-a100)';
    }
  }, props), /*#__PURE__*/React.createElement("svg", {
    width: "14",
    height: "14",
    viewBox: "0 0 24 24",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: "2",
    strokeLinecap: "round",
    strokeLinejoin: "round",
    style: {
      flex: 'none'
    }
  }, /*#__PURE__*/React.createElement("path", {
    d: "M12 3l1.9 4.8L19 9.5l-4.1 2.7L16 17l-4-2.8L8 17l1.1-4.8L5 9.5l5.1-1.7z"
  })), children);
}
Object.assign(__ds_scope, { SmartReply });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/chat/SmartReply.jsx", error: String((e && e.message) || e) }); }

// components/core/Avatar.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const PALETTE = ['#0096F8', '#00B9E3', '#00DF5D', '#2D7AFF', '#0050DC', '#D6A100', '#D64846'];
function hashColor(str = '') {
  let h = 0;
  for (let i = 0; i < str.length; i++) h = h * 31 + str.charCodeAt(i) >>> 0;
  return PALETTE[h % PALETTE.length];
}
function initials(name = '') {
  const parts = name.trim().split(/\s+/);
  return ((parts[0]?.[0] || '') + (parts[1]?.[0] || '')).toUpperCase() || '?';
}

/** User / group avatar. Renders image, else colored initials. Optional presence dot. */
function Avatar({
  name = '',
  src = null,
  size = 40,
  presence = null,
  square = false,
  style = {},
  ...props
}) {
  const bg = hashColor(name);
  const dot = {
    online: 'var(--success)',
    away: 'var(--warning)',
    busy: 'var(--danger)',
    offline: 'var(--gray-300)'
  };
  return /*#__PURE__*/React.createElement("span", _extends({
    style: {
      position: 'relative',
      display: 'inline-flex',
      flex: 'none',
      width: size,
      height: size,
      ...style
    }
  }, props), /*#__PURE__*/React.createElement("span", {
    style: {
      width: '100%',
      height: '100%',
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      borderRadius: square ? 'var(--radius-md)' : 'var(--radius-circle)',
      overflow: 'hidden',
      background: src ? 'var(--surface-sunken)' : bg,
      color: '#fff',
      fontFamily: 'var(--font-ui)',
      fontWeight: 'var(--weight-semibold)',
      fontSize: size * 0.4,
      letterSpacing: '0.01em',
      userSelect: 'none'
    }
  }, src ? /*#__PURE__*/React.createElement("img", {
    src: src,
    alt: name,
    style: {
      width: '100%',
      height: '100%',
      objectFit: 'cover'
    }
  }) : initials(name)), presence && /*#__PURE__*/React.createElement("span", {
    style: {
      position: 'absolute',
      right: -1,
      bottom: -1,
      width: Math.max(8, size * 0.28),
      height: Math.max(8, size * 0.28),
      borderRadius: '50%',
      background: dot[presence] || dot.offline,
      border: '2px solid var(--surface-card)'
    }
  }));
}
Object.assign(__ds_scope, { Avatar });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Avatar.jsx", error: String((e && e.message) || e) }); }

// components/core/Badge.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/** Status / count badge. Tones map to semantic colors; `flag` uses the brand gradient. */
function Badge({
  tone = 'neutral',
  size = 'md',
  dot = false,
  style = {},
  children,
  ...props
}) {
  const tones = {
    neutral: {
      background: 'var(--surface-sunken)',
      color: 'var(--text-secondary)'
    },
    primary: {
      background: 'var(--theme-a200)',
      color: 'var(--primary)'
    },
    success: {
      background: 'rgba(0,178,84,0.14)',
      color: 'var(--success)'
    },
    warning: {
      background: 'rgba(214,161,0,0.16)',
      color: '#9a7400'
    },
    danger: {
      background: 'rgba(214,72,70,0.14)',
      color: 'var(--danger)'
    },
    flag: {
      background: 'var(--brand-gradient)',
      color: '#fff'
    }
  };
  const t = tones[tone] || tones.neutral;
  const h = size === 'sm' ? 18 : 22;
  return /*#__PURE__*/React.createElement("span", _extends({
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 5,
      height: h,
      padding: dot ? `0 8px 0 6px` : '0 9px',
      fontFamily: 'var(--font-ui)',
      fontWeight: 'var(--weight-semibold)',
      fontSize: size === 'sm' ? 11 : 12,
      letterSpacing: '0.01em',
      borderRadius: 'var(--radius-pill)',
      whiteSpace: 'nowrap',
      ...t,
      ...style
    }
  }, props), dot && /*#__PURE__*/React.createElement("span", {
    style: {
      width: 6,
      height: 6,
      borderRadius: '50%',
      background: 'currentColor'
    }
  }), children);
}
Object.assign(__ds_scope, { Badge });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Badge.jsx", error: String((e && e.message) || e) }); }

// components/core/Button.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/**
 * Tanka primary action button. Variants map to the brand button tokens;
 * primary uses the electric blue, with a subtle glow on the key CTA.
 */
function Button({
  variant = 'primary',
  size = 'md',
  fullWidth = false,
  disabled = false,
  leadingIcon = null,
  trailingIcon = null,
  glow = false,
  style = {},
  children,
  ...props
}) {
  const sizes = {
    sm: {
      height: 32,
      padding: '0 12px',
      font: 'var(--ui-callout-size)',
      radius: 'var(--radius-sm)',
      gap: 6
    },
    md: {
      height: 40,
      padding: '0 16px',
      font: 'var(--ui-body-size)',
      radius: 'var(--radius-md)',
      gap: 8
    },
    lg: {
      height: 48,
      padding: '0 22px',
      font: 'var(--ui-body-size)',
      radius: 'var(--radius-md)',
      gap: 8
    }
  };
  const s = sizes[size] || sizes.md;
  const variants = {
    primary: {
      background: 'var(--primary)',
      color: '#fff',
      border: '1px solid transparent',
      boxShadow: glow ? 'var(--shadow-primary)' : 'none'
    },
    secondary: {
      background: 'var(--button-bg-secondary)',
      color: 'var(--text-primary)',
      border: '1px solid var(--border)'
    },
    ghost: {
      background: 'transparent',
      color: 'var(--text-primary)',
      border: '1px solid transparent'
    },
    inverse: {
      background: 'var(--button-bg-inverse)',
      color: 'var(--text-inverse)',
      border: '1px solid transparent'
    },
    danger: {
      background: 'var(--danger)',
      color: '#fff',
      border: '1px solid transparent'
    }
  };
  const v = variants[variant] || variants.primary;
  return /*#__PURE__*/React.createElement("button", _extends({
    disabled: disabled,
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      justifyContent: 'center',
      gap: s.gap,
      height: s.height,
      padding: s.padding,
      width: fullWidth ? '100%' : 'auto',
      fontFamily: 'var(--font-ui)',
      fontWeight: 'var(--weight-semibold)',
      fontSize: s.font,
      lineHeight: 1,
      letterSpacing: '-0.01em',
      borderRadius: s.radius,
      cursor: disabled ? 'not-allowed' : 'pointer',
      opacity: disabled ? 0.5 : 1,
      whiteSpace: 'nowrap',
      transition: 'filter var(--duration-fast) var(--ease-standard), background var(--duration-fast) var(--ease-standard), box-shadow var(--duration-normal) var(--ease-standard)',
      ...v,
      ...style
    },
    onMouseDown: e => {
      if (!disabled) e.currentTarget.style.filter = 'brightness(0.94)';
    },
    onMouseUp: e => {
      e.currentTarget.style.filter = 'none';
    },
    onMouseLeave: e => {
      e.currentTarget.style.filter = 'none';
    }
  }, props), leadingIcon, children, trailingIcon);
}
Object.assign(__ds_scope, { Button });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Button.jsx", error: String((e && e.message) || e) }); }

// components/core/IconButton.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/** Square icon-only button. Pass a Lucide (or any) icon as children. */
function IconButton({
  size = 'md',
  variant = 'ghost',
  round = false,
  disabled = false,
  style = {},
  children,
  ...props
}) {
  const sizes = {
    sm: 30,
    md: 36,
    lg: 44
  };
  const dim = sizes[size] || sizes.md;
  const variants = {
    ghost: {
      background: 'transparent',
      color: 'var(--text-secondary)'
    },
    secondary: {
      background: 'var(--button-bg-secondary)',
      color: 'var(--text-primary)'
    },
    primary: {
      background: 'var(--primary)',
      color: '#fff'
    }
  };
  const v = variants[variant] || variants.ghost;
  return /*#__PURE__*/React.createElement("button", _extends({
    disabled: disabled,
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      justifyContent: 'center',
      width: dim,
      height: dim,
      padding: 0,
      borderRadius: round ? 'var(--radius-pill)' : 'var(--radius-md)',
      border: '1px solid transparent',
      cursor: disabled ? 'not-allowed' : 'pointer',
      opacity: disabled ? 0.5 : 1,
      transition: 'background var(--duration-fast) var(--ease-standard), transform var(--duration-fast) var(--ease-standard)',
      ...v,
      ...style
    },
    onMouseEnter: e => {
      if (!disabled && variant === 'ghost') e.currentTarget.style.background = 'var(--bg-hover)';
    },
    onMouseLeave: e => {
      if (variant === 'ghost') e.currentTarget.style.background = 'transparent';
    },
    onMouseDown: e => {
      if (!disabled) e.currentTarget.style.transform = 'scale(0.92)';
    },
    onMouseUp: e => {
      e.currentTarget.style.transform = 'scale(1)';
    }
  }, props), children);
}
Object.assign(__ds_scope, { IconButton });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/IconButton.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Card.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/** Surface container. Soft border + low shadow; `interactive` adds hover lift. */
function Card({
  padding = 'var(--space-5)',
  interactive = false,
  elevation = 'sm',
  style = {},
  children,
  ...props
}) {
  const shadows = {
    none: 'none',
    sm: 'var(--shadow-sm)',
    md: 'var(--shadow-md)',
    lg: 'var(--shadow-lg)'
  };
  return /*#__PURE__*/React.createElement("div", _extends({
    style: {
      background: 'var(--surface-card)',
      border: '1px solid var(--border)',
      borderRadius: 'var(--radius-lg)',
      boxShadow: shadows[elevation] || shadows.sm,
      padding,
      transition: 'box-shadow var(--duration-normal) var(--ease-standard), transform var(--duration-normal) var(--ease-standard)',
      cursor: interactive ? 'pointer' : 'default',
      ...style
    },
    onMouseEnter: e => {
      if (interactive) {
        e.currentTarget.style.boxShadow = 'var(--shadow-md)';
        e.currentTarget.style.transform = 'translateY(-2px)';
      }
    },
    onMouseLeave: e => {
      if (interactive) {
        e.currentTarget.style.boxShadow = shadows[elevation] || shadows.sm;
        e.currentTarget.style.transform = 'none';
      }
    }
  }, props), children);
}
Object.assign(__ds_scope, { Card });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Card.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Spinner.jsx
try { (() => {
/** Spinner. Indeterminate ring in primary, or brand-flag gradient. */
function Spinner({
  size = 20,
  stroke = 2.5,
  flag = false,
  style = {}
}) {
  const id = React.useId();
  return /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'inline-flex',
      width: size,
      height: size,
      ...style
    }
  }, /*#__PURE__*/React.createElement("svg", {
    width: size,
    height: size,
    viewBox: "0 0 24 24",
    style: {
      animation: 'tanka-spin 0.8s linear infinite'
    }
  }, flag && /*#__PURE__*/React.createElement("defs", null, /*#__PURE__*/React.createElement("linearGradient", {
    id: `g${id}`,
    x1: "0",
    y1: "0",
    x2: "1",
    y2: "1"
  }, /*#__PURE__*/React.createElement("stop", {
    offset: "0%",
    stopColor: "#00DF5D"
  }), /*#__PURE__*/React.createElement("stop", {
    offset: "50%",
    stopColor: "#00B9E3"
  }), /*#__PURE__*/React.createElement("stop", {
    offset: "100%",
    stopColor: "#2D7AFF"
  }))), /*#__PURE__*/React.createElement("circle", {
    cx: "12",
    cy: "12",
    r: "9",
    fill: "none",
    stroke: "var(--divider)",
    strokeWidth: stroke
  }), /*#__PURE__*/React.createElement("path", {
    d: "M12 3 a9 9 0 0 1 9 9",
    fill: "none",
    strokeLinecap: "round",
    stroke: flag ? `url(#g${id})` : 'var(--primary)',
    strokeWidth: stroke
  })), /*#__PURE__*/React.createElement("style", null, `@keyframes tanka-spin{to{transform:rotate(360deg)}}`));
}
Object.assign(__ds_scope, { Spinner });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Spinner.jsx", error: String((e && e.message) || e) }); }

// components/forms/Input.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const {
  useState
} = React;
/** Text field. Supports leading/trailing adornments and error state. */
function Input({
  size = 'md',
  leading = null,
  trailing = null,
  error = false,
  disabled = false,
  style = {},
  ...props
}) {
  const [focused, setFocused] = useState(false);
  const heights = {
    sm: 34,
    md: 40,
    lg: 46
  };
  const h = heights[size] || heights.md;
  const borderColor = error ? 'var(--danger)' : focused ? 'var(--primary)' : 'var(--border)';
  return /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 8,
      height: h,
      padding: '0 12px',
      background: disabled ? 'var(--field-bg-pressed)' : 'var(--field-bg)',
      border: `1px solid ${borderColor}`,
      borderRadius: 'var(--radius-md)',
      boxShadow: focused && !error ? 'var(--focus-ring)' : 'none',
      transition: 'border-color var(--duration-fast) var(--ease-standard), box-shadow var(--duration-fast) var(--ease-standard)',
      opacity: disabled ? 0.6 : 1,
      ...style
    }
  }, leading && /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      color: 'var(--text-tertiary)',
      flex: 'none'
    }
  }, leading), /*#__PURE__*/React.createElement("input", _extends({
    disabled: disabled,
    onFocus: e => {
      setFocused(true);
      props.onFocus?.(e);
    },
    onBlur: e => {
      setFocused(false);
      props.onBlur?.(e);
    },
    style: {
      flex: 1,
      minWidth: 0,
      border: 'none',
      outline: 'none',
      background: 'transparent',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--ui-body-size)',
      color: 'var(--field-text)'
    }
  }, props)), trailing && /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      color: 'var(--text-tertiary)',
      flex: 'none'
    }
  }, trailing));
}
Object.assign(__ds_scope, { Input });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Input.jsx", error: String((e && e.message) || e) }); }

// components/forms/Switch.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
/** Toggle switch. Controlled via `checked` + `onChange(next)`. */
function Switch({
  checked = false,
  onChange,
  disabled = false,
  size = 'md',
  style = {},
  ...props
}) {
  const dims = size === 'sm' ? {
    w: 36,
    h: 20,
    k: 14
  } : {
    w: 44,
    h: 24,
    k: 18
  };
  return /*#__PURE__*/React.createElement("button", _extends({
    role: "switch",
    "aria-checked": checked,
    disabled: disabled,
    onClick: () => !disabled && onChange?.(!checked),
    style: {
      position: 'relative',
      width: dims.w,
      height: dims.h,
      flex: 'none',
      padding: 0,
      border: 'none',
      cursor: disabled ? 'not-allowed' : 'pointer',
      borderRadius: 'var(--radius-pill)',
      background: checked ? 'var(--primary)' : 'var(--gray-300)',
      opacity: disabled ? 0.5 : 1,
      transition: 'background var(--duration-normal) var(--ease-standard)',
      ...style
    }
  }, props), /*#__PURE__*/React.createElement("span", {
    style: {
      position: 'absolute',
      top: (dims.h - dims.k) / 2,
      left: checked ? dims.w - dims.k - (dims.h - dims.k) / 2 : (dims.h - dims.k) / 2,
      width: dims.k,
      height: dims.k,
      borderRadius: '50%',
      background: '#fff',
      boxShadow: '0 1px 3px rgba(0,0,1,0.3)',
      transition: 'left var(--duration-normal) var(--ease-emphasized)'
    }
  }));
}
Object.assign(__ds_scope, { Switch });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Switch.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/ChatHeader.jsx
try { (() => {
/* Conversation header bar. */
function ChatHeader({
  title,
  kind,
  members,
  memoryOpen,
  onToggleMemory
}) {
  const {
    IconButton,
    Avatar
  } = window.TankaDesignSystem_31dfeb;
  const Icon = window.Icon;
  return /*#__PURE__*/React.createElement("header", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 12,
      height: 60,
      flex: 'none',
      padding: '0 16px',
      borderBottom: '1px solid var(--border)',
      background: 'var(--surface-card)'
    }
  }, kind === 'channel' ? /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      width: 34,
      height: 34,
      borderRadius: 'var(--radius-md)',
      background: 'var(--surface-sunken)',
      color: 'var(--text-secondary)'
    }
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "hash",
    size: 18
  })) : /*#__PURE__*/React.createElement(Avatar, {
    name: title,
    size: 34,
    presence: "online"
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      minWidth: 0
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 16,
      fontWeight: 700,
      letterSpacing: '-0.01em',
      color: 'var(--text-primary)'
    }
  }, kind === 'channel' ? `#${title}` : title), /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 12,
      color: 'var(--text-tertiary)'
    }
  }, kind === 'channel' ? `${members} members` : 'Active now')), /*#__PURE__*/React.createElement(IconButton, {
    "aria-label": "Call"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "phone",
    size: 19
  })), /*#__PURE__*/React.createElement(IconButton, {
    "aria-label": "Video"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "video",
    size: 19
  })), /*#__PURE__*/React.createElement(IconButton, {
    "aria-label": "Pinned"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "pin",
    size: 19
  })), /*#__PURE__*/React.createElement(IconButton, {
    variant: memoryOpen ? 'primary' : 'ghost',
    onClick: onToggleMemory,
    "aria-label": "Memory"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "brain",
    size: 19
  })));
}
window.ChatHeader = ChatHeader;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/ChatHeader.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/Composer.jsx
try { (() => {
/* Message composer with Smart Reply row above. */
function Composer({
  replies,
  onSend,
  onUseReply
}) {
  const {
    IconButton
  } = window.TankaDesignSystem_31dfeb;
  const {
    SmartReply
  } = window.TankaDesignSystem_31dfeb;
  const Icon = window.Icon;
  const [val, setVal] = React.useState('');
  const send = () => {
    const t = val.trim();
    if (t) {
      onSend(t);
      setVal('');
    }
  };
  return /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 'none',
      padding: '10px 16px 14px',
      background: 'var(--surface-card)',
      borderTop: '1px solid var(--border)'
    }
  }, replies && replies.length > 0 && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 8,
      marginBottom: 10,
      flexWrap: 'wrap'
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 5,
      fontSize: 11,
      fontWeight: 700,
      letterSpacing: '0.04em',
      textTransform: 'uppercase',
      color: 'var(--primary)'
    }
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "sparkles",
    size: 13,
    color: "var(--primary)"
  }), "Smart Reply"), replies.map((r, i) => /*#__PURE__*/React.createElement(SmartReply, {
    key: i,
    onClick: () => onUseReply(r)
  }, r))), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 8,
      padding: '6px 8px 6px 12px',
      background: 'var(--field-bg)',
      border: '1px solid var(--border)',
      borderRadius: 'var(--radius-xl)'
    }
  }, /*#__PURE__*/React.createElement(IconButton, {
    size: "sm",
    "aria-label": "Attach"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "paperclip",
    size: 18
  })), /*#__PURE__*/React.createElement("input", {
    value: val,
    onChange: e => setVal(e.target.value),
    onKeyDown: e => {
      if (e.key === 'Enter') send();
    },
    placeholder: "Message \u2014 Tanka is listening for what matters\u2026",
    style: {
      flex: 1,
      minWidth: 0,
      border: 'none',
      outline: 'none',
      background: 'transparent',
      fontFamily: 'var(--font-ui)',
      fontSize: 15,
      color: 'var(--field-text)'
    }
  }), /*#__PURE__*/React.createElement(IconButton, {
    size: "sm",
    "aria-label": "Emoji"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "smile",
    size: 18
  })), /*#__PURE__*/React.createElement(IconButton, {
    variant: "primary",
    round: true,
    onClick: send,
    "aria-label": "Send"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "send",
    size: 17,
    color: "#fff"
  }))));
}
window.Composer = Composer;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/Composer.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/Icons.jsx
try { (() => {
/* Lucide-style line icons (1.75 stroke, rounded) used across the Tanka kit.
   Substituting Lucide path data — flag if Tanka ships a licensed set. */
const ICON_PATHS = {
  search: 'M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM21 21l-4.3-4.3',
  send: 'M14.5 9.5 21 3M21 3l-6.5 18-4-8-8-4L21 3z',
  paperclip: 'M21 11.5 12.5 20a5 5 0 0 1-7-7l9-9a3.3 3.3 0 0 1 4.7 4.7l-9 9a1.7 1.7 0 0 1-2.4-2.4l8.1-8.1',
  sparkles: 'M12 3l1.9 4.8L19 9.5l-4.1 2.7L16 17l-4-2.8L8 17l1.1-4.8L5 9.5l5.1-1.7zM19 15l.6 1.6L21 17l-1.4.6L19 19l-.6-1.4L17 17l1.4-.4z',
  brain: 'M12 5a3 3 0 0 0-3 3 3 3 0 0 0-1 5.8V16a2 2 0 0 0 4 0M12 5a3 3 0 0 1 3 3 3 3 0 0 1 1 5.8V16a2 2 0 0 1-4 0M12 5V19',
  hash: 'M4 9h16M4 15h16M10 3 8 21M16 3l-2 18',
  plus: 'M12 5v14M5 12h14',
  settings: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM19.4 15a1.6 1.6 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-2.7 1.1V21a2 2 0 1 1-4 0v-.1A1.6 1.6 0 0 0 7 19.4a1.6 1.6 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0-1.1-2.7H1a2 2 0 1 1 0-4h.1A1.6 1.6 0 0 0 4.6 7a1.6 1.6 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.6 1.6 0 0 0 1.8.3H9a1.6 1.6 0 0 0 1-1.5V1a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 2.7 1.1l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.8V9a1.6 1.6 0 0 0 1.5 1H23a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1z',
  phone: 'M22 16.9v3a2 2 0 0 1-2.2 2 19.8 19.8 0 0 1-8.6-3 19.5 19.5 0 0 1-6-6 19.8 19.8 0 0 1-3-8.7A2 2 0 0 1 4.1 2h3a2 2 0 0 1 2 1.7c.1 1 .4 1.9.7 2.8a2 2 0 0 1-.5 2.1L8.1 9.9a16 16 0 0 0 6 6l1.3-1.3a2 2 0 0 1 2.1-.4c.9.3 1.8.6 2.8.7a2 2 0 0 1 1.7 2z',
  video: 'M23 7l-7 5 7 5V7zM1 5h12a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H1V5z',
  smile: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM8 14s1.5 2 4 2 4-2 4-2M9 9h.01M15 9h.01',
  pin: 'M12 21s7-5.7 7-11a7 7 0 1 0-14 0c0 5.3 7 11 7 11zM12 12a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z',
  bell: 'M18 8a6 6 0 1 0-12 0c0 7-3 9-3 9h18s-3-2-3-9M13.7 21a2 2 0 0 1-3.4 0',
  check: 'M20 6 9 17l-5-5',
  checkcheck: 'M18 6 7 17l-3-3M22 10l-7.5 7.5L13 16',
  chevron: 'M9 18l6-6-6-6',
  x: 'M18 6 6 18M6 6l12 12',
  inbox: 'M22 12h-6l-2 3h-4l-2-3H2M5.5 5h13a2 2 0 0 1 1.8 1.1L22 12v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-6l3.7-5.9A2 2 0 0 1 5.5 5z',
  clock: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 7v5l3 2',
  list: 'M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01'
};
function Icon({
  name,
  size = 20,
  color = 'currentColor',
  strokeWidth = 1.75,
  style = {}
}) {
  const d = ICON_PATHS[name] || '';
  return React.createElement('svg', {
    width: size,
    height: size,
    viewBox: '0 0 24 24',
    fill: 'none',
    stroke: color,
    strokeWidth,
    strokeLinecap: 'round',
    strokeLinejoin: 'round',
    style: {
      flex: 'none',
      display: 'block',
      ...style
    }
  }, React.createElement('path', {
    d
  }));
}
window.Icon = Icon;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/Icons.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/MemoryPanel.jsx
try { (() => {
/* Right-hand Memory panel — Tanka's AI memory bank for the active conversation. */
function MemoryPanel({
  memories,
  onClose
}) {
  const {
    IconButton,
    MemoryChip,
    Button
  } = window.TankaDesignSystem_31dfeb;
  const Icon = window.Icon;
  return /*#__PURE__*/React.createElement("aside", {
    style: {
      width: 320,
      flex: 'none',
      height: '100%',
      display: 'flex',
      flexDirection: 'column',
      background: 'var(--surface-card)',
      borderLeft: '1px solid var(--border)'
    }
  }, /*#__PURE__*/React.createElement("header", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 10,
      height: 60,
      flex: 'none',
      padding: '0 14px 0 16px',
      borderBottom: '1px solid var(--border)'
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      width: 30,
      height: 30,
      borderRadius: 'var(--radius-sm)',
      background: 'var(--brand-gradient)',
      color: '#fff'
    }
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "brain",
    size: 17,
    color: "#fff"
  })), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 15,
      fontWeight: 700,
      color: 'var(--text-primary)'
    }
  }, "Memory"), /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 11,
      color: 'var(--text-tertiary)'
    }
  }, memories.length, " recalled for this chat")), /*#__PURE__*/React.createElement(IconButton, {
    onClick: onClose,
    "aria-label": "Close"
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "x",
    size: 18
  }))), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      overflowY: 'auto',
      padding: 14,
      display: 'flex',
      flexDirection: 'column',
      gap: 10
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      padding: '12px 13px',
      borderRadius: 'var(--radius-md)',
      background: 'var(--theme-a100)',
      border: '1px solid var(--theme-a300)'
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 6,
      marginBottom: 6
    }
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "sparkles",
    size: 14,
    color: "var(--primary)"
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontSize: 11,
      fontWeight: 700,
      letterSpacing: '0.04em',
      textTransform: 'uppercase',
      color: 'var(--primary)'
    }
  }, "AI summary")), /*#__PURE__*/React.createElement("p", {
    style: {
      margin: 0,
      fontSize: 13,
      lineHeight: 1.5,
      color: 'var(--text-secondary)'
    }
  }, "Tanka tracked the key decisions, owners and follow-ups in this conversation \u2014 surfaced below so no one re-explains.")), memories.map((m, i) => /*#__PURE__*/React.createElement(MemoryChip, {
    key: i,
    label: m.label,
    time: m.time
  }, m.text)), /*#__PURE__*/React.createElement(Button, {
    variant: "secondary",
    fullWidth: true,
    leadingIcon: /*#__PURE__*/React.createElement(Icon, {
      name: "list",
      size: 16
    }),
    style: {
      marginTop: 4
    }
  }, "View full memory bank")));
}
window.MemoryPanel = MemoryPanel;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/MemoryPanel.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/Sidebar.jsx
try { (() => {
/* Left navigation sidebar: logo, search, channels, DMs, profile. */
function Sidebar({
  active,
  onSelect
}) {
  const {
    Avatar,
    Badge
  } = window.TankaDesignSystem_31dfeb;
  const {
    channels,
    dms
  } = window.TANKA_DATA;
  const Icon = window.Icon;
  const SectionLabel = ({
    children
  }) => /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 11,
      fontWeight: 700,
      letterSpacing: '0.06em',
      textTransform: 'uppercase',
      color: 'var(--text-tertiary)',
      padding: '0 8px',
      margin: '16px 0 6px'
    }
  }, children);
  const Row = ({
    id,
    children,
    count,
    presenceNode
  }) => {
    const on = active === id;
    return /*#__PURE__*/React.createElement("button", {
      onClick: () => onSelect(id),
      style: {
        display: 'flex',
        alignItems: 'center',
        gap: 10,
        width: '100%',
        textAlign: 'left',
        padding: '7px 8px',
        border: 'none',
        borderRadius: 'var(--radius-sm)',
        cursor: 'pointer',
        background: on ? 'var(--theme-a200)' : 'transparent',
        color: on ? 'var(--primary)' : 'var(--text-secondary)',
        fontFamily: 'var(--font-ui)',
        fontSize: 14,
        fontWeight: on ? 600 : 500,
        transition: 'background var(--duration-fast) var(--ease-standard)'
      },
      onMouseEnter: e => {
        if (!on) e.currentTarget.style.background = 'var(--bg-hover)';
      },
      onMouseLeave: e => {
        if (!on) e.currentTarget.style.background = 'transparent';
      }
    }, presenceNode, /*#__PURE__*/React.createElement("span", {
      style: {
        flex: 1,
        minWidth: 0,
        overflow: 'hidden',
        textOverflow: 'ellipsis',
        whiteSpace: 'nowrap'
      }
    }, children), count ? /*#__PURE__*/React.createElement(Badge, {
      tone: "primary",
      size: "sm"
    }, count) : null);
  };
  return /*#__PURE__*/React.createElement("aside", {
    style: {
      width: 'var(--sidebar-width)',
      flex: 'none',
      height: '100%',
      display: 'flex',
      flexDirection: 'column',
      background: 'var(--surface-card)',
      borderRight: '1px solid var(--border)'
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 10,
      padding: '16px 16px 12px'
    }
  }, /*#__PURE__*/React.createElement("img", {
    src: "../../assets/tanka-mark.svg",
    alt: "Tanka",
    style: {
      width: 28,
      height: 28
    }
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontSize: 20,
      fontWeight: 600,
      letterSpacing: '-0.02em',
      color: 'var(--text-primary)'
    }
  }, "Tanka"), /*#__PURE__*/React.createElement("span", {
    style: {
      marginLeft: 'auto'
    }
  }, /*#__PURE__*/React.createElement(Badge, {
    tone: "flag",
    size: "sm"
  }, "AI"))), /*#__PURE__*/React.createElement("div", {
    style: {
      padding: '0 12px 4px'
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 8,
      height: 36,
      padding: '0 10px',
      background: 'var(--surface-sunken)',
      borderRadius: 'var(--radius-md)',
      color: 'var(--text-tertiary)'
    }
  }, /*#__PURE__*/React.createElement(Icon, {
    name: "search",
    size: 16
  }), /*#__PURE__*/React.createElement("span", {
    style: {
      fontSize: 13,
      fontFamily: 'var(--font-ui)'
    }
  }, "Search memory & chats"))), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      overflowY: 'auto',
      padding: '0 12px 12px'
    }
  }, /*#__PURE__*/React.createElement(SectionLabel, null, "Channels"), channels.map(c => /*#__PURE__*/React.createElement(Row, {
    key: c.id,
    id: c.id,
    count: c.unread,
    presenceNode: /*#__PURE__*/React.createElement(Icon, {
      name: "hash",
      size: 16,
      color: "currentColor"
    })
  }, c.name)), /*#__PURE__*/React.createElement(SectionLabel, null, "Direct messages"), dms.map(d => /*#__PURE__*/React.createElement(Row, {
    key: d.id,
    id: d.id,
    presenceNode: /*#__PURE__*/React.createElement(Avatar, {
      name: d.name,
      size: 22,
      presence: d.presence
    })
  }, d.name))), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 10,
      padding: '10px 14px',
      borderTop: '1px solid var(--divider)'
    }
  }, /*#__PURE__*/React.createElement(Avatar, {
    name: "Jordan Reye",
    size: 32,
    presence: "online"
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      minWidth: 0
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 13,
      fontWeight: 600,
      color: 'var(--text-primary)'
    }
  }, "Jordan Reye"), /*#__PURE__*/React.createElement("div", {
    style: {
      fontSize: 11,
      color: 'var(--text-tertiary)'
    }
  }, "Active")), /*#__PURE__*/React.createElement(Icon, {
    name: "settings",
    size: 18,
    color: "var(--text-tertiary)"
  })));
}
window.Sidebar = Sidebar;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/Sidebar.jsx", error: String((e && e.message) || e) }); }

// ui_kits/messenger/data.js
try { (() => {
/* Fake seed data for the Tanka messenger kit. */
window.TANKA_DATA = {
  me: {
    name: 'You'
  },
  channels: [{
    id: 'general',
    name: 'general',
    kind: 'channel',
    members: 24
  }, {
    id: 'product',
    name: 'product',
    kind: 'channel',
    members: 12,
    unread: 3
  }, {
    id: 'design',
    name: 'design',
    kind: 'channel',
    members: 8
  }, {
    id: 'gtm',
    name: 'go-to-market',
    kind: 'channel',
    members: 15
  }],
  dms: [{
    id: 'kisson',
    name: 'Kisson Lin',
    presence: 'online',
    role: 'CEO'
  }, {
    id: 'aria',
    name: 'Aria Wong',
    presence: 'online',
    role: 'Design Lead'
  }, {
    id: 'sam',
    name: 'Sam Patel',
    presence: 'away',
    role: 'Eng'
  }, {
    id: 'mei',
    name: 'Mei Tan',
    presence: 'busy',
    role: 'PM'
  }],
  threads: {
    product: [{
      id: 1,
      author: 'Aria Wong',
      time: '9:18 AM',
      text: 'Morning! Are we still locked on the March 18 memory beta date?'
    }, {
      id: 2,
      author: 'Sam Patel',
      time: '9:20 AM',
      text: 'Backend is ready. Just need the rollout doc signed off.'
    }, {
      id: 3,
      type: 'memory',
      label: 'Decision',
      time: 'Mar 4',
      text: 'Team agreed to ship the memory beta to all teams on March 18.'
    }, {
      id: 4,
      author: 'Kisson Lin',
      time: '9:23 AM',
      text: 'Yes — March 18. Let\u2019s make sure support is briefed before then.'
    }],
    general: [{
      id: 1,
      author: 'Mei Tan',
      time: '8:40 AM',
      text: 'Welcome to the 6 new folks who joined this week 👋'
    }, {
      id: 2,
      author: 'Kisson Lin',
      time: '8:42 AM',
      text: 'Reminder: all-hands moved to Thursday 10am PT.'
    }, {
      id: 3,
      type: 'memory',
      label: 'Reminder',
      time: 'Today',
      text: 'All-hands rescheduled to Thursday 10:00 AM PT.'
    }],
    design: [{
      id: 1,
      author: 'Aria Wong',
      time: '11:02 AM',
      text: 'New bubble radius shipped to the kit — that tucked corner reads so much better.'
    }, {
      id: 2,
      author: 'You',
      me: true,
      time: '11:05 AM',
      text: 'Agreed. Feels unmistakably Tanka now.'
    }],
    gtm: [{
      id: 1,
      author: 'Mei Tan',
      time: '2:10 PM',
      text: 'Launch metrics are in: 35k smart replies, 111 workdays saved.'
    }, {
      id: 2,
      type: 'memory',
      label: 'Insight',
      time: 'Feb 18',
      text: '16.5 smart replies per user per month since the Oct 2024 pilot.'
    }]
  },
  smartReplies: {
    product: ['Add it to the team calendar', 'Share the rollout doc', 'Brief support today'],
    general: ['Thanks for the heads up', 'I\u2019ll be there', 'Can someone record it?'],
    design: ['Ship it 🚀', 'Looks great', 'Open a PR'],
    gtm: ['Incredible numbers', 'Let\u2019s put this in the deck', 'Share with the board']
  },
  memories: {
    product: [{
      label: 'Decision',
      time: 'Mar 4',
      text: 'Memory beta ships to all teams March 18.'
    }, {
      label: 'Owner',
      time: 'Mar 4',
      text: 'Sam owns backend readiness; Aria owns the rollout doc.'
    }, {
      label: 'Follow-up',
      time: 'Mar 5',
      text: 'Brief the support team before launch day.'
    }],
    general: [{
      label: 'Reminder',
      time: 'Today',
      text: 'All-hands moved to Thursday 10:00 AM PT.'
    }, {
      label: 'People',
      time: 'This week',
      text: '6 new teammates joined across product & design.'
    }],
    design: [{
      label: 'Decision',
      time: 'Today',
      text: 'Adopted the 18/4px tucked bubble radius system-wide.'
    }],
    gtm: [{
      label: 'Insight',
      time: 'Feb 18',
      text: '35,000+ smart replies generated; ~111 workdays saved.'
    }, {
      label: 'Metric',
      time: 'Feb 18',
      text: '16.5 smart replies per user per month.'
    }]
  }
};
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/messenger/data.js", error: String((e && e.message) || e) }); }

__ds_ns.MemoryChip = __ds_scope.MemoryChip;

__ds_ns.MessageBubble = __ds_scope.MessageBubble;

__ds_ns.SmartReply = __ds_scope.SmartReply;

__ds_ns.Avatar = __ds_scope.Avatar;

__ds_ns.Badge = __ds_scope.Badge;

__ds_ns.Button = __ds_scope.Button;

__ds_ns.IconButton = __ds_scope.IconButton;

__ds_ns.Card = __ds_scope.Card;

__ds_ns.Spinner = __ds_scope.Spinner;

__ds_ns.Input = __ds_scope.Input;

__ds_ns.Switch = __ds_scope.Switch;

})();
