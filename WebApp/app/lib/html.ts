// htm bound to Preact: JSX-like templates without a build step.
import { h } from 'preact';
import htm from 'htm';

export const html = htm.bind(h);
