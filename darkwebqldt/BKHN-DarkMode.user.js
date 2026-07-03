// ==UserScript==
// @name         BKHN DarkMode
// @namespace    http://tampermonkey.net/
// @version      1.0.0
// @description  Handcrafted Dark Mode theme for HUST QLDT (qldt.hust.edu.vn)
// @author       Antigravity
// @match        *://qldt.hust.edu.vn/*
// @run-at       document-start
// @resource     DARK_CSS file:///D:/Study/HIMH/tool/darkwebqldt/dark.css
// @grant        GM_getResourceText
// @grant        GM_addStyle
// ==/UserScript==

(function () {
    'use strict';

    // Function to inject style
    function injectStyle() {
        try {
            const css = GM_getResourceText("DARK_CSS");
            if (css) {
                GM_addStyle(css);
            } else {
                console.warn("[BKHN-DarkMode] css resource not loaded yet.");
            }
        } catch (e) {
            console.error("[BKHN-DarkMode] Error injecting dark.css:", e);
        }
    }

    // Run immediately at document-start to prevent white flashes
    injectStyle();

    // Listen for DOMContentLoaded to handle any late DOM attachments or checks
    window.addEventListener('DOMContentLoaded', () => {
        console.log("[BKHN-DarkMode] DOM fully loaded. Dark Mode theme applied.");
    });
})();
