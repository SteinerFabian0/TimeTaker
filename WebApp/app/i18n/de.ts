// German. Missing keys fall back to English (i18n.ts).
import type { en } from './en.ts';

export const de: Partial<Record<keyof typeof en, string>> = {
  'nav.label': 'Hauptnavigation',
  'nav.group.drive': 'Fahren',
  'nav.group.plan': 'Planen',
  'nav.group.system': 'System',

  'area.grandPrix': 'Grand Prix',
  'area.timeAttack': 'Time Attack',
  'area.training': 'Training',
  'area.tournaments': 'Turniere',
  'area.racePresets': 'Rennvorlagen',
  'area.trackSetup': 'Bahn-Setup',
  'area.status': 'Status',
  'area.settings': 'Einstellungen',

  'sub.grandPrix': 'Teilnehmer eintragen, Turnier oder Rennvorlage wählen und losfahren.',
  'sub.timeAttack': 'Dauerzeitnahme auf jeder Spur. Für den großen Bildschirm.',
  'sub.training': 'Jede Runde einer Session, Bestzeiten und mit dem Power Pack Gas-Verläufe.',
  'sub.tournaments': 'Turniervorlagen aus Rennvorlagen bauen.',
  'sub.racePresets': 'Runden, Rennen pro Spur, Punkte und Gleichstand.',
  'sub.trackSetup': 'Bahntempo, Leistungsgrenzen und Spurausgleich.',
  'sub.status': 'Verbindung und Zustand von Steuermodul, BLE-Brücke und jeder Spur.',
  'sub.settings': 'Sprache, Darstellung und Daten.',

  'conn.offline': 'Nicht verbunden',
  'conn.sim': 'Simulator',
  'conn.noPowerPack': 'Kein Power Pack',

  'common.planned': 'Hier geplant',
  'common.preview': 'Vorschau mit Beispieldaten',
  'common.comingSoon': 'Kommt bald',

  'pp.lockTitle': 'Dafür braucht es das Power Pack',
  'pp.lockBody': 'Verbinde einen TimeTaker mit Power Pack, um die Bahnleistung einzustellen. Die Einstellungen unten zeigen, was hier möglich ist.',

  'gp.lane': 'Spur {n}',
  'gp.lap': 'Runde',
  'gp.last': 'Letzte',
  'gp.best': 'Beste',
  'gp.gap': 'Abstand',
  'gp.raceOf': 'Rennen {n} von {total}',
  'gp.groupA': 'Gruppe A',
  'gp.start': 'Rennen starten',
  'gp.next': 'Nächstes Rennen',

  'status.connection': 'Verbindung',
  'status.debugLink': 'Debug-Konsole öffnen',

  'ts.speed': 'Bahntempo',
  'ts.level': 'Stufe {n}',
  'ts.speedHint': 'Skaliert die ganze Bahn. Stufe 1 ist das langsamste Tempo, bei dem jedes Auto noch sicher anfährt; Stufe 5 ist das volle Limit aus den Experteneinstellungen.',
  'ts.expert': 'Experteneinstellungen zeigen',
  'ts.expertHint': 'Leistungsgrenzen, Strombegrenzung pro Spur und Spurausgleich. Bei jedem Öffnen wieder aus.',

  'set.language': 'Sprache',
  'set.theme': 'Darstellung',
  'set.theme.dark': 'Dunkel',
  'set.theme.light': 'Hell',
  'set.theme.system': 'System',
  'set.lanes': 'Spurfarben',
  'set.about': 'Info',
  'set.version': 'Web-App {v}',
};
