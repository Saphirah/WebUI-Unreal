// Independent of the JSON bridge. Native calls setEnabled after each navigation.
(() => {
  if (window.__webuiSelection) return;
  let enabled = false;
  const roots = new Map();
  const css = '*{user-select:none!important;-webkit-user-select:none!important}' +
    '*::selection{background:transparent!important;color:inherit!important}';
  const clear = root => {
    if (!enabled) return;
    const selection = root.getSelection?.();
    if (selection && !selection.isCollapsed) selection.collapseToEnd();
    const field = root.activeElement;
    if (field && typeof field.selectionStart === 'number' && field.selectionStart !== field.selectionEnd) {
      try { field.setSelectionRange(field.selectionEnd, field.selectionEnd); } catch (_) {}
    }
    if (field?.shadowRoot) clear(field.shadowRoot);
  };
  const install = root => {
    if (!root || roots.has(root)) return;
    const doc = root.ownerDocument || root;
    const style = doc.createElement('style');
    style.textContent = css;
    const cancel = event => { if (enabled) event.preventDefault(); };
    const changed = () => clear(root);
    const keys = event => {
      if (!enabled) return;
      if (((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a') ||
          (event.shiftKey && /^(ArrowLeft|ArrowRight|ArrowUp|ArrowDown|Home|End|PageUp|PageDown)$/.test(event.key)))
        event.preventDefault();
    };
    const scan = () => {
      if (!enabled) return;
      const parent = root.head || root.documentElement || root;
      if (!style.isConnected) parent.appendChild(style);
      for (const element of root.querySelectorAll('*')) {
        if (element.shadowRoot) install(element.shadowRoot);
        if (element.tagName === 'IFRAME') {
          try { install(element.contentDocument); } catch (_) {}
        }
      }
    };
    roots.set(root, {style, scan});
    root.addEventListener('selectstart', cancel, true);
    root.addEventListener('keydown', keys, true);
    root.addEventListener('selectionchange', changed, true);
    root.addEventListener('select', changed, true);
    root.addEventListener('load', scan, true);
    new MutationObserver(scan).observe(root, {childList:true, subtree:true});
    scan(); clear(root);
  };
  window.__webuiSelection = {setEnabled(value) {
    enabled = value;
    if (enabled) install(document);
    for (const [root, entry] of roots) {
      if (enabled) { entry.scan(); clear(root); } else entry.style.remove();
    }
  }};
})();
