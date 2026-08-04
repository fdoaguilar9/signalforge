/* @ds-bundle: {"format":4,"namespace":"SignalforgeDesignSystem_cc3c6b","components":[{"name":"Button","sourcePath":"components/core/Button.jsx"},{"name":"IconButton","sourcePath":"components/core/IconButton.jsx"},{"name":"ListRow","sourcePath":"components/data/ListRow.jsx"},{"name":"StageCard","sourcePath":"components/data/StageCard.jsx"},{"name":"Table","sourcePath":"components/data/Table.jsx"},{"name":"LevelMeter","sourcePath":"components/feedback/LevelMeter.jsx"},{"name":"Modal","sourcePath":"components/feedback/Modal.jsx"},{"name":"StatusBanner","sourcePath":"components/feedback/StatusBanner.jsx"},{"name":"Select","sourcePath":"components/forms/Select.jsx"},{"name":"Slider","sourcePath":"components/forms/Slider.jsx"},{"name":"Toggle","sourcePath":"components/forms/Toggle.jsx"}],"sourceHashes":{"components/core/Button.jsx":"4cdcd2475a12","components/core/IconButton.jsx":"0a40b4217fff","components/data/ListRow.jsx":"f8a42eebcdaf","components/data/StageCard.jsx":"38f573d9ae29","components/data/Table.jsx":"a0e746d7d41f","components/feedback/LevelMeter.jsx":"e41c5bbd89c6","components/feedback/Modal.jsx":"5b4f9d296c01","components/feedback/StatusBanner.jsx":"28b6c0e0dc29","components/forms/Select.jsx":"361015d4a7fe","components/forms/Slider.jsx":"34ccae4ac9ce","components/forms/Toggle.jsx":"badead9b4d6a","ui_kits/board/BoardScreen.jsx":"9519b41aa0c8"},"inlinedExternals":[],"unexposedExports":[]} */

(() => {

const __ds_ns = (window.SignalforgeDesignSystem_cc3c6b = window.SignalforgeDesignSystem_cc3c6b || {});

const __ds_scope = {};

(__ds_ns.__errors = __ds_ns.__errors || []);

// components/core/Button.jsx
try { (() => {
function Button({
  variant = 'primary',
  size = 'md',
  disabled = false,
  children,
  onClick
}) {
  const sizes = {
    sm: {
      padding: '6px 10px',
      fontSize: 'var(--text-xs)'
    },
    md: {
      padding: '8px 14px',
      fontSize: 'var(--text-sm)'
    },
    lg: {
      padding: '10px 18px',
      fontSize: 'var(--text-base)'
    }
  };
  const base = {
    fontFamily: 'var(--font-ui)',
    fontWeight: 600,
    borderRadius: 'var(--radius-sm)',
    border: '1px solid transparent',
    cursor: disabled ? 'default' : 'pointer',
    opacity: disabled ? 0.45 : 1,
    transition: 'background 100ms linear',
    ...sizes[size]
  };
  const variants = {
    primary: {
      background: 'var(--accent-primary)',
      color: 'var(--accent-primary-on)'
    },
    secondary: {
      background: 'var(--bg-surface-raised)',
      color: 'var(--text-primary)',
      borderColor: 'var(--border-hairline)'
    },
    ghost: {
      background: 'transparent',
      color: 'var(--text-secondary)'
    },
    danger: {
      background: 'transparent',
      color: 'var(--meter-clip)',
      borderColor: 'var(--meter-clip)'
    }
  };
  return React.createElement('button', {
    disabled,
    onClick,
    style: {
      ...base,
      ...variants[variant]
    }
  }, children);
}
Object.assign(__ds_scope, { Button });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Button.jsx", error: String((e && e.message) || e) }); }

// components/core/IconButton.jsx
try { (() => {
function IconButton({
  label = 'learn',
  bound = false,
  bindingText = '',
  armed = false,
  disabled = false,
  onClick
}) {
  return React.createElement('div', {
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 'var(--space-2)',
      fontFamily: 'var(--font-ui)'
    }
  }, React.createElement('button', {
    disabled: disabled || armed,
    onClick,
    title: bound ? 'Rebind -- currently ' + bindingText : 'Bind a MIDI message to this',
    style: {
      background: bound ? 'var(--bg-inset)' : 'var(--bg-surface-raised)',
      color: 'var(--text-secondary)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-sm)',
      padding: '3px 9px',
      fontSize: 'var(--text-2xs)',
      fontFamily: 'var(--font-mono)',
      textTransform: 'uppercase',
      letterSpacing: 'var(--tracking-wide)',
      cursor: disabled || armed ? 'default' : 'pointer',
      opacity: disabled || armed ? 0.45 : 1
    }
  }, label), bound && React.createElement('span', {
    style: {
      fontFamily: 'var(--font-mono)',
      fontSize: 'var(--text-2xs)',
      color: 'var(--text-disabled)'
    }
  }, bindingText));
}
Object.assign(__ds_scope, { IconButton });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/IconButton.jsx", error: String((e && e.message) || e) }); }

// components/data/ListRow.jsx
try { (() => {
function ListRow({
  label,
  selected = false,
  sublabel,
  right,
  onClick
}) {
  return React.createElement('div', {
    onClick,
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      padding: 'var(--space-2) var(--space-3)',
      background: selected ? 'var(--bg-surface-raised)' : 'transparent',
      borderRadius: 'var(--radius-sm)',
      cursor: onClick ? 'pointer' : 'default',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)'
    }
  }, React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column'
    }
  }, React.createElement('span', {
    style: {
      color: selected ? 'var(--accent-primary)' : 'var(--text-primary)',
      fontWeight: selected ? 600 : 400
    }
  }, label), sublabel && React.createElement('span', {
    style: {
      fontSize: 'var(--text-2xs)',
      fontFamily: 'var(--font-mono)',
      color: 'var(--text-disabled)'
    }
  }, sublabel)), right && React.createElement('div', null, right));
}
Object.assign(__ds_scope, { ListRow });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/ListRow.jsx", error: String((e && e.message) || e) }); }

// components/data/StageCard.jsx
try { (() => {
function StageCard({
  index,
  name,
  bypassed = false,
  expanded = false,
  onToggleExpand,
  onToggleBypass,
  onMoveUp,
  onMoveDown,
  onRemove,
  children
}) {
  const iconBtn = (label, onClick, disabled) => React.createElement('button', {
    onClick,
    disabled,
    style: {
      width: 24,
      height: 24,
      background: 'var(--bg-surface-raised)',
      color: 'var(--text-secondary)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-sm)',
      cursor: disabled ? 'default' : 'pointer',
      opacity: disabled ? 0.35 : 1,
      fontFamily: 'var(--font-mono)',
      fontSize: 'var(--text-xs)'
    }
  }, label);
  return React.createElement('div', {
    style: {
      background: 'var(--bg-surface)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-md)',
      boxShadow: 'var(--shadow-panel)',
      padding: 'var(--space-3) var(--space-4)',
      fontFamily: 'var(--font-ui)'
    }
  }, React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-2)'
    }
  }, React.createElement('span', {
    onClick: onToggleExpand,
    style: {
      cursor: 'pointer',
      color: 'var(--text-secondary)',
      width: 14
    }
  }, expanded ? '▾' : '▸'), React.createElement('input', {
    type: 'checkbox',
    checked: !bypassed,
    onChange: onToggleBypass,
    style: {
      accentColor: 'var(--accent-primary)'
    }
  }), React.createElement('span', {
    style: {
      color: bypassed ? 'var(--text-disabled)' : 'var(--text-primary)',
      flex: 1
    }
  }, index + '. ' + name), iconBtn('^', onMoveUp, false), iconBtn('v', onMoveDown, false), iconBtn('x', onRemove, false)), expanded && React.createElement('div', {
    style: {
      marginTop: 'var(--space-3)',
      paddingLeft: 'var(--space-6)',
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-3)'
    }
  }, children));
}
Object.assign(__ds_scope, { StageCard });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/StageCard.jsx", error: String((e && e.message) || e) }); }

// components/data/Table.jsx
try { (() => {
function Table({
  rows = []
}) {
  return React.createElement('table', {
    style: {
      borderCollapse: 'collapse',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)',
      width: '100%'
    }
  }, React.createElement('tbody', null, rows.map((r, i) => React.createElement('tr', {
    key: i
  }, React.createElement('td', {
    style: {
      color: 'var(--text-secondary)',
      padding: '4px 12px 4px 0'
    }
  }, r[0]), React.createElement('td', {
    style: {
      color: 'var(--text-primary)',
      fontFamily: 'var(--font-mono)',
      padding: '4px 0'
    }
  }, r[1])))));
}
Object.assign(__ds_scope, { Table });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/data/Table.jsx", error: String((e && e.message) || e) }); }

// components/feedback/LevelMeter.jsx
try { (() => {
function LevelMeter({
  label,
  db = -100,
  clipping = false
}) {
  const silent = !isFinite(db);
  const fraction = Math.max(0, Math.min(1, (db + 60) / 60));
  let color = 'var(--meter-comfortable)';
  if (clipping || db >= 0) color = 'var(--meter-clip)';else if (db >= -6) color = 'var(--meter-hot)';else if (db >= -18) color = 'var(--meter-warn)';
  return React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-3)',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)'
    }
  }, React.createElement('span', {
    style: {
      width: 60,
      color: 'var(--text-secondary)'
    }
  }, label), React.createElement('div', {
    style: {
      flex: 1,
      height: 16,
      borderRadius: 'var(--radius-sm)',
      background: 'var(--bg-inset)',
      boxShadow: 'var(--shadow-inset)',
      position: 'relative',
      overflow: 'hidden'
    }
  }, React.createElement('div', {
    style: {
      position: 'absolute',
      inset: 0,
      width: fraction * 100 + '%',
      background: color,
      transition: 'width 100ms linear'
    }
  })), React.createElement('span', {
    style: {
      width: 70,
      fontFamily: 'var(--font-mono)',
      fontSize: 'var(--text-xs)',
      color: 'var(--text-primary)',
      textAlign: 'right'
    }
  }, silent ? '--' : db.toFixed(1) + ' dB'));
}
Object.assign(__ds_scope, { LevelMeter });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/LevelMeter.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Modal.jsx
try { (() => {
function Modal({
  title,
  open = true,
  onClose,
  children,
  footer
}) {
  if (!open) return null;
  return React.createElement('div', {
    style: {
      position: 'absolute',
      inset: 0,
      background: 'rgba(0,0,0,0.6)',
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      fontFamily: 'var(--font-ui)'
    }
  }, React.createElement('div', {
    style: {
      background: 'var(--bg-surface)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-lg)',
      boxShadow: 'var(--shadow-modal)',
      minWidth: 360,
      maxWidth: '80%'
    }
  }, React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      padding: 'var(--space-4)',
      borderBottom: '1px solid var(--border-hairline)'
    }
  }, React.createElement('span', {
    style: {
      fontWeight: 600,
      color: 'var(--text-primary)'
    }
  }, title), onClose && React.createElement('button', {
    onClick: onClose,
    style: {
      background: 'transparent',
      border: 'none',
      color: 'var(--text-secondary)',
      cursor: 'pointer',
      fontSize: 'var(--text-lg)',
      lineHeight: 1
    }
  }, '×')), React.createElement('div', {
    style: {
      padding: 'var(--space-4)',
      color: 'var(--text-primary)'
    }
  }, children), footer && React.createElement('div', {
    style: {
      padding: 'var(--space-4)',
      borderTop: '1px solid var(--border-hairline)',
      display: 'flex',
      gap: 'var(--space-2)',
      justifyContent: 'flex-end'
    }
  }, footer)));
}
Object.assign(__ds_scope, { Modal });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Modal.jsx", error: String((e && e.message) || e) }); }

// components/feedback/StatusBanner.jsx
try { (() => {
function StatusBanner({
  tone = 'warning',
  children
}) {
  const tones = {
    warning: 'var(--status-warning)',
    unavailable: 'var(--status-unavailable)',
    clip: 'var(--meter-clip)'
  };
  return React.createElement('div', {
    style: {
      color: tones[tone],
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)',
      padding: 'var(--space-2) 0',
      borderBottom: '1px solid var(--border-hairline)'
    }
  }, children);
}
Object.assign(__ds_scope, { StatusBanner });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/StatusBanner.jsx", error: String((e && e.message) || e) }); }

// components/forms/Select.jsx
try { (() => {
function Select({
  label,
  value,
  options = [],
  placeholder = '(any available port)',
  onChange
}) {
  return React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 4,
      fontFamily: 'var(--font-ui)',
      minWidth: 260
    }
  }, label && React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-secondary)'
    }
  }, label), React.createElement('select', {
    value: value ?? '',
    onChange: e => onChange && onChange(e.target.value),
    style: {
      background: 'var(--bg-inset)',
      color: 'var(--text-primary)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-sm)',
      padding: '8px 10px',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)',
      boxShadow: 'var(--shadow-inset)'
    }
  }, React.createElement('option', {
    value: ''
  }, placeholder), options.map((o, i) => React.createElement('option', {
    key: i,
    value: o
  }, o))));
}
Object.assign(__ds_scope, { Select });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Select.jsx", error: String((e && e.message) || e) }); }

// components/forms/Slider.jsx
try { (() => {
function Slider({
  label,
  value = 0,
  min = 0,
  max = 1,
  unit = '',
  logarithmic = false,
  onChange
}) {
  const pct = Math.max(0, Math.min(1, (value - min) / (max - min))) * 100;
  return React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 4,
      fontFamily: 'var(--font-ui)',
      minWidth: 220
    }
  }, React.createElement('div', {
    style: {
      display: 'flex',
      justifyContent: 'space-between',
      fontSize: 'var(--text-xs)',
      color: 'var(--text-secondary)'
    }
  }, React.createElement('span', null, label), React.createElement('span', {
    style: {
      fontFamily: 'var(--font-mono)',
      color: 'var(--text-primary)'
    }
  }, value.toFixed(2) + unit)), React.createElement('input', {
    type: 'range',
    min,
    max,
    step: (max - min) / 1000,
    value,
    onChange: e => onChange && onChange(parseFloat(e.target.value)),
    style: {
      accentColor: 'var(--accent-primary)',
      background: 'transparent'
    }
  }), logarithmic && React.createElement('span', {
    style: {
      fontSize: 'var(--text-2xs)',
      color: 'var(--text-disabled)',
      fontFamily: 'var(--font-mono)'
    }
  }, 'logarithmic'));
}
Object.assign(__ds_scope, { Slider });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Slider.jsx", error: String((e && e.message) || e) }); }

// components/forms/Toggle.jsx
try { (() => {
function Toggle({
  checked = false,
  label,
  onChange
}) {
  return React.createElement('label', {
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 'var(--space-2)',
      cursor: 'pointer',
      fontFamily: 'var(--font-ui)',
      fontSize: 'var(--text-sm)',
      color: checked ? 'var(--text-primary)' : 'var(--text-disabled)'
    }
  }, React.createElement('span', {
    onClick: () => onChange && onChange(!checked),
    style: {
      width: 32,
      height: 18,
      borderRadius: 'var(--radius-pill)',
      background: checked ? 'var(--accent-primary)' : 'var(--bg-inset)',
      boxShadow: 'var(--shadow-inset)',
      position: 'relative',
      transition: 'background 100ms linear',
      flexShrink: 0
    }
  }, React.createElement('span', {
    style: {
      position: 'absolute',
      top: 2,
      left: checked ? 16 : 2,
      width: 14,
      height: 14,
      borderRadius: '50%',
      background: checked ? 'var(--accent-primary-on)' : 'var(--gray-400)',
      transition: 'left 100ms linear'
    }
  })), label && React.createElement('span', null, label));
}
Object.assign(__ds_scope, { Toggle });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Toggle.jsx", error: String((e && e.message) || e) }); }

// ui_kits/board/BoardScreen.jsx
try { (() => {
const {
  Button,
  IconButton,
  Toggle,
  Slider,
  Select,
  LevelMeter,
  StatusBanner,
  Table,
  ListRow,
  StageCard,
  Modal
} = window.SignalforgeDesignSystem_cc3c6b;
const PRESETS = [{
  name: 'Clean',
  stem: '01-clean',
  binding: ''
}, {
  name: 'Rhythm',
  stem: '02-rhythm',
  binding: 'PC 3'
}, {
  name: 'Lead',
  stem: '03-lead',
  binding: ''
}];
const PLUGINS = [{
  name: 'Neural Amp Modeler',
  uri: 'neural-amp-modeler-lv2',
  io: '1 in / 1 out'
}, {
  name: 'ZeroConvolv (IR loader)',
  uri: 'zeroconvolv#CfgMono',
  io: '1 in / 1 out'
}, {
  name: 'x42 Reverb',
  uri: 'x42-reverb',
  io: '2 in / 2 out'
}, {
  name: 'C* Tape Delay',
  uri: 'cs-delay',
  io: '1 in / 1 out'
}];
function BoardScreen() {
  const [current, setCurrent] = React.useState(1);
  const [stages, setStages] = React.useState([{
    name: 'Neural Amp Modeler',
    bypassed: false,
    expanded: true,
    controls: [{
      symbol: 'input_level',
      value: 0,
      min: -24,
      max: 24
    }, {
      symbol: 'output_level',
      value: -3,
      min: -24,
      max: 24
    }, {
      symbol: 'quality_scale',
      value: 1,
      min: 0,
      max: 1
    }]
  }, {
    name: 'ZeroConvolv (IR loader)',
    bypassed: false,
    expanded: false,
    controls: [{
      symbol: 'dry',
      value: -60,
      min: -60,
      max: 0
    }, {
      symbol: 'wet',
      value: 0,
      min: -60,
      max: 0
    }]
  }]);
  const [warning, setWarning] = React.useState('');
  const [settingsOpen, setSettingsOpen] = React.useState(false);
  const [addOpen, setAddOpen] = React.useState(false);
  const [search, setSearch] = React.useState('');
  const [unsaved, setUnsaved] = React.useState(false);
  const toggleExpand = i => setStages(s => s.map((st, idx) => idx === i ? {
    ...st,
    expanded: !st.expanded
  } : st));
  const toggleBypass = i => {
    setStages(s => s.map((st, idx) => idx === i ? {
      ...st,
      bypassed: !st.bypassed
    } : st));
    setUnsaved(true);
  };
  const move = (i, dir) => {
    setStages(s => {
      const arr = [...s];
      const j = i + dir;
      if (j < 0 || j >= arr.length) return s;
      [arr[i], arr[j]] = [arr[j], arr[i]];
      return arr;
    });
    setUnsaved(true);
  };
  const remove = i => {
    if (stages.length <= 1) return;
    setStages(s => s.filter((_, idx) => idx !== i));
    setUnsaved(true);
  };
  const addPlugin = name => {
    setStages(s => [...s, {
      name,
      bypassed: false,
      expanded: false,
      controls: [{
        symbol: 'mix',
        value: 0.5,
        min: 0,
        max: 1
      }]
    }]);
    setAddOpen(false);
    setUnsaved(true);
  };
  return React.createElement('div', {
    style: {
      background: 'var(--bg-app)',
      color: 'var(--text-primary)',
      fontFamily: 'var(--font-ui)',
      width: '100%',
      maxWidth: 960,
      minHeight: 720,
      margin: '0 auto',
      padding: 'var(--space-6)',
      boxSizing: 'border-box',
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-5)'
    }
  },
  // header / menu bar
  React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      borderBottom: '1px solid var(--border-hairline)',
      paddingBottom: 'var(--space-3)'
    }
  }, React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'baseline',
      gap: 'var(--space-3)'
    }
  }, React.createElement('span', {
    style: {
      fontFamily: 'var(--font-display)',
      fontWeight: 600,
      fontSize: 'var(--text-xl)'
    }
  }, 'Signalforge'), React.createElement('span', {
    style: {
      fontFamily: 'var(--font-mono)',
      fontSize: 'var(--text-sm)',
      color: 'var(--text-secondary)'
    }
  }, PRESETS[current].name)), React.createElement(Button, {
    variant: 'ghost',
    size: 'sm',
    onClick: () => setSettingsOpen(true)
  }, 'Settings')), warning && React.createElement(StatusBanner, {
    tone: 'warning'
  }, warning),
  // Presets
  React.createElement('section', null, React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-2)',
      marginBottom: 'var(--space-2)',
      color: 'var(--text-secondary)',
      fontSize: 'var(--text-xs)',
      textTransform: 'uppercase',
      letterSpacing: 'var(--tracking-wide)'
    }
  }, 'Presets'), React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-2)',
      marginBottom: 'var(--space-2)'
    }
  }, React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-disabled)'
    }
  }, 'Step:'), React.createElement(IconButton, {
    label: 'learn'
  }), React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-disabled)'
    }
  }, 'next'), React.createElement(IconButton, {
    label: 'learn'
  }), React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-disabled)'
    }
  }, 'prev')), React.createElement('div', {
    style: {
      background: 'var(--bg-surface)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-md)',
      padding: 'var(--space-2)',
      display: 'flex',
      flexDirection: 'column',
      gap: 2
    }
  }, PRESETS.map((p, i) => React.createElement(ListRow, {
    key: i,
    label: p.name,
    sublabel: p.stem,
    selected: i === current,
    right: React.createElement(IconButton, {
      bound: !!p.binding,
      bindingText: p.binding
    }),
    onClick: () => setCurrent(i)
  })))),
  // Engine
  React.createElement('section', null, React.createElement('div', {
    style: {
      color: 'var(--text-secondary)',
      fontSize: 'var(--text-xs)',
      textTransform: 'uppercase',
      letterSpacing: 'var(--tracking-wide)',
      marginBottom: 'var(--space-2)'
    }
  }, 'Engine'), React.createElement('div', {
    style: {
      background: 'var(--bg-surface)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-md)',
      padding: 'var(--space-4)',
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-3)'
    }
  }, React.createElement(Table, {
    rows: [['Sample rate', '48000 Hz'], ['Block size', '256 frames'], ['Latency', '5.3 ms round trip'], ['MIDI bindings', '4']]
  }), React.createElement(LevelMeter, {
    label: 'Input',
    db: -24.6
  }), React.createElement(LevelMeter, {
    label: 'Output',
    db: -3.1
  }))),
  // Chain
  React.createElement('section', null, React.createElement('div', {
    style: {
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      marginBottom: 'var(--space-2)'
    }
  }, React.createElement('span', {
    style: {
      color: 'var(--text-secondary)',
      fontSize: 'var(--text-xs)',
      textTransform: 'uppercase',
      letterSpacing: 'var(--tracking-wide)'
    }
  }, 'Chain'), React.createElement(Button, {
    variant: 'secondary',
    size: 'sm',
    onClick: () => setAddOpen(true)
  }, 'Add stage')), React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-3)'
    }
  }, stages.map((st, i) => React.createElement(StageCard, {
    key: i,
    index: i,
    name: st.name,
    bypassed: st.bypassed,
    expanded: st.expanded,
    onToggleExpand: () => toggleExpand(i),
    onToggleBypass: () => toggleBypass(i),
    onMoveUp: () => move(i, -1),
    onMoveDown: () => move(i, 1),
    onRemove: () => remove(i)
  }, st.controls.map((c, ci) => React.createElement('div', {
    key: ci,
    style: {
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-3)'
    }
  }, React.createElement(Slider, {
    label: c.symbol,
    value: c.value,
    min: c.min,
    max: c.max
  }), React.createElement(IconButton, {
    label: 'learn'
  }))))))), React.createElement('div', {
    style: {
      borderTop: '1px solid var(--border-hairline)',
      paddingTop: 'var(--space-3)',
      display: 'flex',
      alignItems: 'center',
      gap: 'var(--space-3)'
    }
  }, React.createElement(Button, {
    variant: 'ghost'
  }, 'Quit'), React.createElement(Button, {
    variant: 'primary',
    disabled: !unsaved,
    onClick: () => setUnsaved(false)
  }, 'Save preset'), unsaved && React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-disabled)'
    }
  }, 'unsaved changes')), settingsOpen && React.createElement(Modal, {
    title: 'Audio Settings',
    onClose: () => setSettingsOpen(false),
    footer: React.createElement(React.Fragment, null, React.createElement(Button, {
      variant: 'ghost',
      onClick: () => setSettingsOpen(false)
    }, 'Cancel'), React.createElement(Button, {
      variant: 'primary',
      onClick: () => setSettingsOpen(false)
    }, 'Apply'))
  }, React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-4)'
    }
  }, React.createElement(Select, {
    label: 'Input (guitar in)',
    options: ['Scarlett 2i2: in 1', 'Scarlett 2i2: in 2']
  }), React.createElement(Select, {
    label: 'Output (sound out)',
    options: ['Built-in Audio: out 1/2']
  }), React.createElement(Select, {
    label: 'Buffer size',
    value: '256 samples  (5.3 ms)',
    options: ['64 samples  (1.3 ms)', '128 samples  (2.7 ms)', '256 samples  (5.3 ms)', '512 samples  (10.7 ms)']
  }))), addOpen && React.createElement(Modal, {
    title: 'Add stage',
    onClose: () => setAddOpen(false),
    footer: React.createElement(Button, {
      variant: 'ghost',
      onClick: () => setAddOpen(false)
    }, 'Cancel')
  }, React.createElement('div', {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--space-3)'
    }
  }, React.createElement('input', {
    placeholder: 'Search plugins...',
    value: search,
    onChange: e => setSearch(e.target.value),
    style: {
      background: 'var(--bg-inset)',
      border: '1px solid var(--border-hairline)',
      borderRadius: 'var(--radius-sm)',
      color: 'var(--text-primary)',
      padding: '8px 10px',
      fontFamily: 'var(--font-ui)'
    }
  }), PLUGINS.filter(p => p.name.toLowerCase().includes(search.toLowerCase())).map((p, i) => React.createElement(ListRow, {
    key: i,
    label: p.name,
    sublabel: p.io,
    onClick: () => addPlugin(p.name)
  })), React.createElement('span', {
    style: {
      fontSize: 'var(--text-xs)',
      color: 'var(--text-disabled)'
    }
  }, PLUGINS.length + ' of ' + PLUGINS.length))));
}
window.BoardScreen = BoardScreen;
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/board/BoardScreen.jsx", error: String((e && e.message) || e) }); }

__ds_ns.Button = __ds_scope.Button;

__ds_ns.IconButton = __ds_scope.IconButton;

__ds_ns.ListRow = __ds_scope.ListRow;

__ds_ns.StageCard = __ds_scope.StageCard;

__ds_ns.Table = __ds_scope.Table;

__ds_ns.LevelMeter = __ds_scope.LevelMeter;

__ds_ns.Modal = __ds_scope.Modal;

__ds_ns.StatusBanner = __ds_scope.StatusBanner;

__ds_ns.Select = __ds_scope.Select;

__ds_ns.Slider = __ds_scope.Slider;

__ds_ns.Toggle = __ds_scope.Toggle;

})();
