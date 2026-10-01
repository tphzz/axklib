import { mount } from 'svelte';
import '../app.css';
import EditorNavigationHarness from './EditorNavigationHarness.svelte';

const scale = Number(new URLSearchParams(location.search).get('scale') ?? 1);
document.documentElement.style.zoom = String(scale);
mount(EditorNavigationHarness, { target: document.getElementById('app')! });
