// The plugin's own editor, as third-party plugins ship one.
export function createElement(plugin) {
  const el = document.createElement('div');
  el.className = 'test-gain-gui';
  el.style.cssText = 'width:240px;padding:16px;color:#fff';
  const b = document.createElement('button');
  b.textContent = 'Silence';
  b.addEventListener('click', () => plugin.audioNode.setParameterValues({ gain: { id: 'gain', value: 0, normalized: false } }));
  el.append('Test Gain editor ', b);
  return el;
}
