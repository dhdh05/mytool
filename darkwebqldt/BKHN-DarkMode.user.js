// ==UserScript==
// @name         BKHN DarkMode
// @namespace    https://github.com/dhdh05/mytool
// @version      2.0.0
// @description  Dark mode cho QLDT HUST (qldt.hust.edu.vn) - giao dien moi Next.js + Ant Design
// @author       dhdh05
// @match        https://qldt.hust.edu.vn/*
// @run-at       document-start
// @resource     DARK_CSS https://raw.githubusercontent.com/dhdh05/mytool/main/darkwebqldt/dark.css
// @require      https://raw.githubusercontent.com/dhdh05/mytool/main/darkwebqldt/bkhn-dark.js
// @updateURL    https://raw.githubusercontent.com/dhdh05/mytool/main/darkwebqldt/BKHN-DarkMode.user.js
// @downloadURL  https://raw.githubusercontent.com/dhdh05/mytool/main/darkwebqldt/BKHN-DarkMode.user.js
// @grant        GM_getResourceText
// @grant        GM_addStyle
// ==/UserScript==

/*
 * dark.css    : doi bien mau Ant Design / bien --ks-* cua QLDT / class Tailwind.
 * bkhn-dark.js: (nap qua @require) xu ly phan con sot luc chay.
 * CSS duoc chen ngay luc document-start nen khong bi nhay trang khi tai trang.
 */
(function () {
    'use strict';
    try {
        const css = GM_getResourceText('DARK_CSS');
        if (css) GM_addStyle(css);
        else console.warn('[BKHN-DarkMode] Khong nap duoc dark.css');
    } catch (e) {
        console.error('[BKHN-DarkMode] Loi khi chen dark.css:', e);
    }
})();
