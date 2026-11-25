// Runs before the first paint: applies a remembered appearance so that the page
// never flashes in the other one. Without a choice the system setting decides.
(function () {
  'use strict';
  try {
    var saved = localStorage.getItem('vortexclean-theme');
    if (saved === 'light' || saved === 'dark') document.documentElement.dataset.theme = saved;
  } catch (error) {
    // Storage can be unavailable (private windows); the system setting still applies.
  }
})();
