// The sidebar (GUI_SPEC §2.1): a collapsed icon rail that expands OVER the
// page to show names.
//   mouse:    hover expands, click navigates and collapses (stays collapsed
//             until the pointer moves onto another item, or leaves and
//             comes back);
//   touch:    a tap anywhere on the rail expands it, a tap on an item then
//             navigates and collapses, a tap outside (scrim) collapses;
//   keyboard: focus on the rail expands it, Enter navigates, Esc collapses.
import { signal } from '@preact/signals';
import { html } from '../lib/html.ts';
import { t } from '../i18n/i18n.ts';
import { AREAS, GROUPS, currentArea, navigate, type Area } from './areas.ts';
import { connection } from './connection.ts';   // colours the Status and Track Setup icons
import { Icon } from './icons.ts';

/** Open: the page slides right and dims (main.ts reads this). */
export const expanded = signal(false);
/** How the user last interacted with the rail. */
let input: 'mouse' | 'touch' | 'keyboard' = 'mouse';
/** After a mouse click: the clicked area. Hovering it does not re-open the
 *  rail; moving onto another item (or leaving the rail) does. */
let hoverLocked: string | null = null;

function collapse() { expanded.value = false; }

function onPointerDown(e: PointerEvent) {
  input = e.pointerType === 'mouse' ? 'mouse' : 'touch';
}
function onPointerEnter(e: PointerEvent) {
  if (e.pointerType === 'mouse' && hoverLocked === null) expanded.value = true;
}
function onPointerLeave(e: PointerEvent) {
  if (e.pointerType !== 'mouse') return;
  hoverLocked = null;
  collapse();
}
/** Hover lock: re-open as soon as the pointer is over a different item. */
function onPointerOver(e: PointerEvent) {
  if (e.pointerType !== 'mouse' || hoverLocked === null) return;
  const item = (e.target as Element).closest('.rail-item');
  if (item && item.getAttribute('href') !== '#/' + hoverLocked) {
    hoverLocked = null;
    expanded.value = true;
  }
}
function onKeyDown(e: KeyboardEvent) {
  input = 'keyboard';
  if (e.key === 'Escape') { collapse(); (e.target as HTMLElement).blur?.(); }
}
function onFocusIn() {
  if (input === 'keyboard') expanded.value = true;
}
function onFocusOut(e: FocusEvent) {
  const rail = e.currentTarget as HTMLElement;
  if (!rail.contains(e.relatedTarget as Node | null) && input === 'keyboard') collapse();
}
/** A tap on the rail background (not an item) while collapsed: expand. */
function onRailClick(e: MouseEvent) {
  if (!expanded.value && input === 'touch') { expanded.value = true; e.preventDefault(); }
}

function go(id: string, e: MouseEvent) {
  e.preventDefault();
  e.stopPropagation();                // the rail's own click handler must not re-open it
  if (!expanded.value && input === 'touch') {   // collapsed tap: open first
    expanded.value = true;
    return;
  }
  navigate(id);
  collapse();
  if (input === 'mouse') hoverLocked = id;
  else (e.currentTarget as HTMLElement).blur();
  if (input !== 'mouse') document.getElementById('main')?.focus({ preventScroll: true });
}

/** Icon colour for areas that show a state (GUI_SPEC §2.1): '' = normal. */
function indicator(area: Area): { cls: '' | 'ok' | 'off' | 'fail'; title?: string } {
  const c = connection.value;
  if (area.indicator === 'powerPack')       // green with a Power Pack, dark grey without
    return c.powerPack ? { cls: 'ok', title: t('conn.powerPack') } : { cls: 'off', title: t('conn.noPowerPack') };
  if (area.indicator === 'link') {          // grey: nothing connected; red: connected, a check fails; else green
    const title = t(`conn.${c.link}` as 'conn.offline');
    if (c.link === 'offline') return { cls: 'off', title };
    return { cls: c.overall === 'fail' ? 'fail' : 'ok', title };
  }
  return { cls: '' };
}

function Item({ area }: { area: Area }) {
  const active = currentArea.value === area.id;
  const ind = indicator(area);
  return html`<a href=${'#/' + area.id} class=${'rail-item' + (active ? ' active' : '')}
      aria-current=${active ? 'page' : undefined} onClick=${(e: MouseEvent) => go(area.id, e)}>
    <span class=${'rail-icon' + (ind.cls ? ' st-' + ind.cls : '')} title=${ind.title}><${Icon} name=${area.icon} /></span>
    <span class="rail-label">${t(area.title)}</span>
  </a>`;
}

export function Rail() {
  const open = expanded.value;
  return html`
    <div class=${'scrim' + (open ? ' on' : '')} onClick=${collapse} aria-hidden="true"></div>
    <nav class=${'rail' + (open ? ' open' : '')} aria-label=${t('nav.label')}
        onPointerDown=${onPointerDown} onPointerEnter=${onPointerEnter} onPointerLeave=${onPointerLeave} onPointerOver=${onPointerOver}
        onKeyDown=${onKeyDown} onFocusIn=${onFocusIn} onFocusOut=${onFocusOut} onClick=${onRailClick}>
      <div class="rail-brand">
        <span class="brand-mark" aria-hidden="true"></span>
        <span class="brand-word" aria-label=${t('app.name')}></span>
      </div>
      ${GROUPS.map(g => html`
        <div class=${'rail-group rail-group-' + g.id}>
          <div class="rail-group-label">${t(g.label)}</div>
          ${AREAS.filter(a => a.group === g.id).map(a => html`<${Item} key=${a.id} area=${a} />`)}
        </div>`)}
    </nav>`;
}
