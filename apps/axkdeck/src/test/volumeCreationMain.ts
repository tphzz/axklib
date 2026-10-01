import { mount } from 'svelte';
import '../app.css';
import VolumeCreationHarness from './VolumeCreationHarness.svelte';
mount(VolumeCreationHarness, { target: document.getElementById('app')! });
