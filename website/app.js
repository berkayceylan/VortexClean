// Page behaviour: appearance toggle, screenshot preview, copy buttons.
import { startInspector } from './inspector.js';

const root = document.documentElement;
const systemDark = window.matchMedia('(prefers-color-scheme: dark)');
const announcer = document.getElementById('announcer');

export function announce(message) {
  announcer.textContent = '';
  requestAnimationFrame(() => { announcer.textContent = message; });
}

// --- Appearance --------------------------------------------------------------

const isDark = () => (root.dataset.theme ? root.dataset.theme === 'dark' : systemDark.matches);
const toggle = document.getElementById('theme-toggle');

function reflectTheme() {
  toggle.setAttribute('aria-label', isDark() ? 'Switch to light appearance' : 'Switch to dark appearance');
  toggle.dataset.shows = isDark() ? 'dark' : 'light';
  reflectPreview();
}

toggle.addEventListener('click', () => {
  root.dataset.theme = isDark() ? 'light' : 'dark';
  try {
    localStorage.setItem('vortexclean-theme', root.dataset.theme);
  } catch {
    // Not remembered, but the page still switches.
  }
  reflectTheme();
});
systemDark.addEventListener('change', reflectTheme);

// --- Screenshot preview ------------------------------------------------------

const preview = document.getElementById('preview');
const frame = preview.querySelector('.shot__frame');
const range = preview.querySelector('.shot__range');
const names = { standard: 'PixelForge Studio', package: 'Tidewater Backup', suite: 'Fernwood Writer' };

/// 100 shows only the light picture, 0 only the dark one.
function reflectPreview() {
  const mode = preview.dataset.mode === 'auto' ? (isDark() ? 'dark' : 'light') : preview.dataset.mode;
  const split = mode === 'light' ? 100 : mode === 'dark' ? 0 : Number(range.value);
  frame.style.setProperty('--split', `${split}%`);
  frame.dataset.split = String(mode === 'split');
  range.hidden = mode !== 'split';
  const choice = preview.querySelector(`input[name="shot-mode"][value="${mode}"]`);
  if (choice) choice.checked = true;
}

for (const input of preview.querySelectorAll('input[name="shot-mode"]')) {
  input.addEventListener('change', () => {
    preview.dataset.mode = input.value;
    reflectPreview();
  });
}

for (const input of preview.querySelectorAll('input[name="shot-app"]')) {
  input.addEventListener('change', () => {
    preview.dataset.app = input.value;
    for (const tone of ['light', 'dark']) {
      const picture = preview.querySelector(`.shot__${tone}`);
      const source = picture.querySelector('source');  // gone if this browser could not decode it
      if (source) source.srcset = `assets/${input.value}-${tone}.webp`;
      const image = picture.querySelector('img');
      image.src = `assets/${input.value}-${tone}.png`;
      image.alt = `VortexClean in ${tone} mode, listing everything that belongs to ${names[input.value]}`;
    }
  });
}

// <picture> chooses a file by its type alone. If the browser claims a type but then
// cannot decode the file, nothing falls back by itself: drop the source, keep the PNG.
for (const picture of preview.querySelectorAll('picture')) {
  const image = picture.querySelector('img');
  const fallBack = () => picture.querySelector('source')?.remove();
  image.addEventListener('error', fallBack);
  if (image.complete && image.naturalWidth === 0) fallBack();
}

range.addEventListener('input', () => {
  frame.dataset.dragging = 'true';
  reflectPreview();
});
range.addEventListener('change', () => { delete frame.dataset.dragging; });

// --- Copy buttons ------------------------------------------------------------

export async function copyText(text, source) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // Clipboard access refused: select the text so that it can be copied by hand.
    const selection = window.getSelection();
    selection.removeAllRanges();
    const area = document.createRange();
    area.selectNodeContents(source);
    selection.addRange(area);
    return false;
  }
}

export function bindCopy(button, source, textOf = () => source.textContent) {
  const label = button.textContent;
  let timer = 0;
  button.addEventListener('click', async () => {
    const copied = await copyText(textOf(), source);
    button.textContent = copied ? 'Copied' : 'Press ⌘C';
    announce(copied ? 'Copied' : 'Selected. Press Command C to copy.');
    clearTimeout(timer);
    timer = setTimeout(() => { button.textContent = label; }, 1800);
  });
}

for (const button of document.querySelectorAll('.code .copy')) {
  const code = button.parentElement.querySelector('code');
  bindCopy(button, code);
}

reflectTheme();
startInspector(document.getElementById('inspector'), { announce, bindCopy });
