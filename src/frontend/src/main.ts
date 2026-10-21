import { mount } from "svelte";

import "./app.css";
import "./assets/fonts.css";

import App from "./App.svelte";

import { initOverlayScrollbar } from "./utils/overlay-scrollbar";

const app = mount(App, { target: document.getElementById("app")! });

initOverlayScrollbar({
  targetSelector:
    "[data-tabs-content][data-state='active'] .group-list, [data-tabs-content][data-state='active'] .subscription-list, .app-tab-content[data-state='active'] .settings-content[data-state='active']",
  headerSelector:
    ".app-tab-content[data-state='active'] .settings-toolbar, .app-tab-content[data-state='active'] .page-controls",
});

export default app;
