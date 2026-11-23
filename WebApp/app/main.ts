// TimeTaker GUI entry: the shell (rail + current area). GUI_SPEC.md.
import { render } from 'preact';
import { html } from './lib/html.ts';
import './lib/prefs.ts';
import { Rail, expanded } from './shell/Rail.ts';
import { currentArea } from './shell/areas.ts';
import { PAGES } from './pages/pages.ts';

function App() {
  const id = currentArea.value;
  const Page = PAGES[id];
  // key=id: a page's local state (e.g. Track Setup expert mode) resets on every visit.
  return html`
    <${Rail} />
    <main id="main" tabindex="-1" class=${'page page-' + id + (expanded.value ? ' shifted' : '')}
        inert=${expanded.value || undefined}>
      <${Page} key=${id} />
    </main>`;
}

render(html`<${App} />`, document.getElementById('app')!);
