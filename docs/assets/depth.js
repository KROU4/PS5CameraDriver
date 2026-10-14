/* Illustrative stereo geometry; these are not captured camera frames. */
(() => {
  const lab = document.querySelector('[data-depth-lab]');
  if (!lab) return;
  const buttons = [...lab.querySelectorAll('[data-view]')];
  const panels = [...lab.querySelectorAll('[data-panel]')];
  const explanations = [...lab.querySelectorAll('[data-explanation]')];
  const slider = lab.querySelector('input[type="range"]');
  const output = lab.querySelector('output');
  const language = document.documentElement.lang;
  function select(view) {
    buttons.forEach(button => button.setAttribute('aria-pressed', String(button.dataset.view === view)));
    panels.forEach(panel => { panel.hidden = panel.dataset.panel !== view; });
    explanations.forEach(item => { item.hidden = item.dataset.explanation !== view; });
  }
  function update() {
    const distance = Number(slider.value);
    const shift = 22 / distance;
    lab.querySelector('[data-right-subject]').setAttribute('transform', `translate(${-shift} 0)`);
    lab.querySelector('[data-disparity-line]').setAttribute('x2', String(200 - shift));
    const tone = Math.round(100 + 90 / distance);
    lab.querySelector('[data-depth-subject]').setAttribute('fill', `rgb(${tone}, ${tone}, ${tone})`);
    lab.querySelector('[data-blur]').setAttribute('stdDeviation', (3 * (1 / distance - 1 / 3)).toFixed(2));
    output.value = `${distance.toLocaleString(language, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} ${language === 'ru' ? 'м' : 'm'}`;
    slider.setAttribute('aria-valuetext', output.value);
  }
  buttons.forEach(button => {
    button.disabled = false;
    button.addEventListener('click', () => select(button.dataset.view));
  });
  slider.disabled = false;
  slider.addEventListener('input', update);
  select('stereo');
  update();
})();
