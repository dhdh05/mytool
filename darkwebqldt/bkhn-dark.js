/*
 * BKHN DarkMode - lop JS du phong.
 * dark.css lo phan lon trang (Ant Design + Tailwind). Phan con sot - style inline,
 * class rieng sinh ngau nhien - duoc xu ly o day: tim phan tu co nen sang / chu toi
 * va doi sang mau toi tuong ung (giu sac do, chi ha do sang).
 * Chi quet phan tu moi them vao (MutationObserver), gom lai moi 150ms.
 */
(function () {
    'use strict';
    if (window.__bkhnDark) return;
    window.__bkhnDark = true;

    const SKIP = new Set(['IMG', 'VIDEO', 'CANVAS', 'SVG', 'PICTURE', 'IFRAME', 'SCRIPT', 'STYLE', 'LINK', 'META', 'NOSCRIPT', 'BR', 'PATH']);

    function parse(c) {
        const m = c && c.match(/[\d.]+/g);
        if (!m || m.length < 3) return null;
        return { r: +m[0], g: +m[1], b: +m[2], a: m.length > 3 ? +m[3] : 1 };
    }

    function toHsl({ r, g, b }) {
        r /= 255; g /= 255; b /= 255;
        const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2;
        let h = 0, s = 0;
        if (max !== min) {
            const d = max - min;
            s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
            h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
            h *= 60;
        }
        return { h, s, l };
    }

    const hsl = (h, s, l, a) => `hsla(${h.toFixed(0)}, ${(s * 100).toFixed(0)}%, ${(l * 100).toFixed(0)}%, ${a})`;

    function fix(el) {
        if (SKIP.has(el.tagName.toUpperCase()) || el.dataset.bkhnKeep !== undefined) return;
        const cs = getComputedStyle(el);

        const bg = parse(cs.backgroundColor);
        if (bg && bg.a > 0.4) {
            const c = toHsl(bg);
            if (c.l > 0.82) {
                /* nen sang -> nen toi cung sac do: trang -> #1d1e21, xanh nhat -> xanh dam */
                el.style.setProperty('background-color', hsl(c.h, Math.min(c.s, 0.45), 0.11 + (1 - c.l) * 0.5, bg.a), 'important');
            }
        }

        const fg = parse(cs.color);
        if (fg && fg.a > 0.12) {
            const c = toHsl(fg);
            if (c.l < 0.3 && c.s < 0.35) {
                /* chu den / xam dam -> chu sang */
                el.style.setProperty('color', hsl(c.h, c.s, 0.9 - c.l * 0.5, fg.a), 'important');
            } else if (c.l < 0.48 && c.s >= 0.35) {
                /* chu mau dam (vd do HUST #AA1D2B) -> sang len de doc duoc tren nen toi */
                el.style.setProperty('color', hsl(c.h, Math.min(c.s, 0.85), 0.66, fg.a), 'important');
            }
        }

        const bd = parse(cs.borderTopColor);
        if (bd && cs.borderTopWidth !== '0px' && bd.a > 0.3 && toHsl(bd).l > 0.8) {
            el.style.setProperty('border-color', 'var(--bk-border, #3a3b40)', 'important');
        }
    }

    function scan(root) {
        if (root.nodeType !== 1) return;
        fix(root);
        const all = root.getElementsByTagName('*');
        for (let i = 0; i < all.length; i++) fix(all[i]);
    }

    let pending = new Set(), timer = 0;
    function flush() {
        timer = 0;
        const roots = [...pending];
        pending.clear();
        for (const n of roots) if (n.isConnected) scan(n);
    }
    function queue(n) {
        pending.add(n);
        if (!timer) timer = setTimeout(flush, 150);
    }

    function start() {
        scan(document.body);
        new MutationObserver(muts => {
            for (const m of muts) {
                if (m.type === 'childList') m.addedNodes.forEach(n => n.nodeType === 1 && queue(n));
                else if (m.target.nodeType === 1 && !m.target.dataset.bkhnBusy) queue(m.target);
            }
        }).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
    }

    if (document.body) start();
    else document.addEventListener('DOMContentLoaded', start);
})();
