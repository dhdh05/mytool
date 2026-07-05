// ==UserScript==
// @name         BKHN Auto Survey (Ultimate UI Rút Gọn)
// @namespace    http://tampermonkey.net/
// @version      99.9.10
// @description  Bản độ UI tinh gọn: Mặc định tắt auto, bỏ các tùy chọn thời gian rườm rà.
// @match        *://ctt-sis.hust.edu.vn/*
// @match        *://ctt-daotao.hust.edu.vn/*
// @allFrames    true
// @grant        GM_setValue
// @grant        GM_getValue
// @run-at       document-end 
// ==/UserScript==


(function () {
    'use strict';

    if (window.top !== window.self) return;

    const CONFIG = {
        storageKeys: {
            isEnabled: 'bkhn_survey_enabled',
            strategy: 'bkhn_survey_strategy'
        },
        defaults: {
            isEnabled: false, //mặc định tool sẽ tắt
            strategy: 'realistic'
        },
        timers: {
            scanIntervalMs: 2000, // thời gian quét phiếu(ms)
            reloadWaitMs: 4000    // thời gian tải lại trang(ms)
        },
        keywords: {
            averageScore: ['bình thường', 'vừa phải', 'phù hợp', 'từ trên 2 giờ đến 3 giờ', 'không thay đổi'],
            submitBtn: ['gửi khảo sát', 'gửi', 'lưu']
        },

        selectors: {
            radioTarget: 'input[type="radio"], span[class*="dxeIRadioButton"]',
            submitCandidates: 'input[type="submit"], button, a, div[class*="dxbButton"]'
        }
    };

    class UIManager {
        constructor(automator) {
            this.automator = automator;
            this.host = null;
            this.shadow = null;
            this.settings = {};

            this.loadSettings();
            this.initUI();
        }

        loadSettings() {
            this.settings.isEnabled = GM_getValue(CONFIG.storageKeys.isEnabled, CONFIG.defaults.isEnabled);
            this.settings.strategy = GM_getValue(CONFIG.storageKeys.strategy, CONFIG.defaults.strategy);
        }

        saveSetting(key, value) {
            this.settings[key] = value;
            GM_setValue(CONFIG.storageKeys[key], value);
        }

        initUI() {
            this.host = document.createElement('div');
            this.host.id = 'bkhn-survey-host';
            document.body.appendChild(this.host);
            this.shadow = this.host.attachShadow({ mode: 'open' });

            const styles = `
                :host { all: initial; }
                .bkhn-container { position: fixed; bottom: 20px; right: 20px; z-index: 2147483647; font-family: system-ui, sans-serif; color: #e5e7eb; user-select: none; }
                .bkhn-panel { width: 290px; background: rgba(17, 17, 23, 0.85); backdrop-filter: blur(12px); border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 16px; box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4); overflow: hidden; transition: all 0.4s; display: flex; flex-direction: column; }
                .bkhn-panel.minimized { width: 50px; height: 50px; border-radius: 50%; cursor: pointer; background: linear-gradient(135deg, #a31d2a 0%, #d32f2f 100%); justify-content: center; align-items: center; border: none; }
                .bkhn-panel.minimized .panel-content, .bkhn-panel.minimized .panel-header { display: none; }
                .bkhn-panel.minimized .panel-toggle-icon { display: flex; font-size: 22px; animation: pulse 2s infinite; justify-content: center; align-items: center; width: 100%; height: 100%; }
                .panel-toggle-icon { display: none; }
                @keyframes pulse { 0% { transform: scale(1); } 50% { transform: scale(1.1); } 100% { transform: scale(1); } }
                .panel-header { display: flex; justify-content: space-between; align-items: center; padding: 12px 16px; background: linear-gradient(135deg, rgba(163, 29, 42, 0.15) 0%, rgba(211, 47, 47, 0.15) 100%); border-bottom: 1px solid rgba(255, 255, 255, 0.06); }
                .panel-title { font-size: 13.5px; font-weight: 700; letter-spacing: 0.5px; color: #ff8a80; display: flex; align-items: center; gap: 6px; }
                .panel-minimize-btn { background: none; border: none; color: rgba(255, 255, 255, 0.5); cursor: pointer; font-size: 18px; line-height: 1; padding: 2px 6px; border-radius: 4px; transition: all 0.2s; }
                .panel-minimize-btn:hover { color: #ffffff; background: rgba(255, 255, 255, 0.1); }
                .panel-content { padding: 16px; display: flex; flex-direction: column; gap: 14px; }
                .form-group { display: flex; flex-direction: column; gap: 5px; }
                label { font-size: 10px; font-weight: 600; color: rgba(255, 255, 255, 0.5); text-transform: uppercase; }
                select { background: rgba(255, 255, 255, 0.05); border: 1px solid rgba(255, 255, 255, 0.1); border-radius: 8px; color: #f3f4f6; padding: 7px 10px; font-size: 12.5px; outline: none; transition: all 0.2s; }
                select:focus { border-color: #d32f2f; background: rgba(255, 255, 255, 0.08); box-shadow: 0 0 0 2px rgba(211, 47, 47, 0.25); }
                select option { background: #111117; color: #f3f4f6; }
                .switch-group { display: flex; justify-content: space-between; align-items: center; background: rgba(255, 255, 255, 0.02); padding: 8px 12px; border-radius: 10px; border: 1px solid rgba(255, 255, 255, 0.04); }
                .switch-label-title { font-size: 12.5px; font-weight: 600; }
                .switch-label-sub { font-size: 9.5px; color: rgba(255, 255, 255, 0.4); display: block; margin-top: 2px; }
                .switch { position: relative; display: inline-block; width: 40px; height: 22px; }
                .switch input { opacity: 0; width: 0; height: 0; }
                .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: rgba(255, 255, 255, 0.1); transition: .3s; border-radius: 22px; }
                .slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 3px; bottom: 3px; background-color: white; transition: .3s; border-radius: 50%; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
                input:checked + .slider { background: linear-gradient(135deg, #a31d2a 0%, #d32f2f 100%); }
                input:checked + .slider:before { transform: translateX(18px); }
                .btn-primary { background: linear-gradient(135deg, #a31d2a 0%, #d32f2f 100%); color: white; border: none; border-radius: 8px; padding: 9px; font-weight: 600; cursor: pointer; transition: all 0.2s; display: flex; justify-content: center; align-items: center; gap: 6px; }
                .btn-primary:hover { transform: translateY(-1px); box-shadow: 0 6px 15px rgba(211, 47, 47, 0.35); }
                .status-console { background: rgba(0, 0, 0, 0.25); border: 1px solid rgba(255, 255, 255, 0.05); border-radius: 8px; padding: 8px 10px; font-size: 11px; font-family: monospace; min-height: 38px; max-height: 52px; overflow-y: auto; color: #ff8a80; line-height: 1.4; }
                .status-console.success { color: #34d399; }
                .status-console.error { color: #f87171; }
                .status-console.warn { color: #fbbf24; }
            `;

            const container = document.createElement('div');
            container.className = 'bkhn-container';
            const panel = document.createElement('div');
            panel.className = 'bkhn-panel';

            panel.innerHTML = `
                <div class="panel-toggle-icon">🔥</div>
                <div class="panel-header">
                    <div class="panel-title">🔥 BKHN AutoFillSurvey</div>
                    <button class="panel-minimize-btn">━</button>
                </div>
                <div class="panel-content">
                    <div class="switch-group">
                        <div>
                            <span class="switch-label-title">Auto Điền (Xuyên Khung)</span>
                            <span class="switch-label-sub">Tự săn form trong Iframe</span>
                        </div>
                        <label class="switch">
                            <input type="checkbox" id="bkhn-toggle-auto" ${this.settings.isEnabled ? 'checked' : ''}>
                            <span class="slider"></span>
                        </label>
                    </div>
                    <div class="form-group">
                        <label>Chiến thuật</label>
                        <select id="bkhn-strategy">
                            <option value="max" ${this.settings.strategy === 'max' ? 'selected' : ''}>Max Điểm (Khen hết lời)</option>
                            <option value="realistic" ${this.settings.strategy === 'realistic' ? 'selected' : ''}>Thực tế (Trộn Khá/Tốt)</option>
                            <option value="random" ${this.settings.strategy === 'random' ? 'selected' : ''}>Ngẫu nhiên</option>
                        </select>
                    </div>
                    <button class="btn-primary" id="bkhn-btn-trigger">▶ Điền Thủ Công</button>
                    <div class="status-console" id="bkhn-console">${this.settings.isEnabled ? 'Đang quét tìm Iframe...' : 'Hệ thống sẵn sàng. Vui lòng bật Auto hoặc ấn Điền Thủ Công.'}</div>
                </div>
            `;

            const styleSheet = document.createElement('style');
            styleSheet.textContent = styles;
            this.shadow.appendChild(styleSheet);
            this.shadow.appendChild(container);
            container.appendChild(panel);

            this.bindEvents(panel);
        }

        bindEvents(panel) {
            const toggleAuto = this.shadow.querySelector('#bkhn-toggle-auto');
            const selectStrategy = this.shadow.querySelector('#bkhn-strategy');
            const btnTrigger = this.shadow.querySelector('#bkhn-btn-trigger');

            const toggleMinimize = () => panel.classList.toggle('minimized');
            this.shadow.querySelector('.panel-minimize-btn').addEventListener('click', toggleMinimize);
            this.shadow.querySelector('.panel-toggle-icon').addEventListener('click', toggleMinimize);

            toggleAuto.addEventListener('change', (e) => {
                this.saveSetting('isEnabled', e.target.checked);
                this.writeConsole(`Đã ${e.target.checked ? 'BẬT' : 'TẮT'} Auto.`, e.target.checked ? 'success' : '');
                e.target.checked ? this.automator.start() : this.automator.stop();
            });

            selectStrategy.addEventListener('change', (e) => {
                this.saveSetting('strategy', e.target.value);
                this.writeConsole(`Chiến thuật: ${e.target.options[e.target.selectedIndex].text}`, 'success');
            });

            btnTrigger.addEventListener('click', () => {
                this.writeConsole('Đang kích hoạt thủ công...', 'warn');
                this.automator.processPage(true);
            });
        }

        writeConsole(text, type = '') {
            const box = this.shadow.querySelector('#bkhn-console');
            if (box) {
                box.className = 'status-console ' + type;
                box.innerText = text;
                box.scrollTop = box.scrollHeight;
            }
        }
    }

    class SurveyAutomator {
        constructor() {
            this.scanner = null;
            this.config = CONFIG;
            this.ui = null;
        }

        init() {
            this.ui = new UIManager(this);
            if (this.ui.settings.isEnabled) {
                this.start();
            }
        }

        start() {
            this.stop();
            if (!this.ui.settings.isEnabled) return;
            this.ui.writeConsole("Đang quét tìm Iframe form khảo sát...");
            this.scanner = setInterval(() => this.processPage(), this.config.timers.scanIntervalMs);
        }

        stop() {
            if (this.scanner) {
                clearInterval(this.scanner);
                this.scanner = null;
            }
        }

        processPage(manual = false) {
            const bodyText = document.body.innerText.toLowerCase();
            if (bodyText.includes('bạn đang có 0 phiếu') || bodyText.includes('không còn khảo sát nào') || bodyText.includes('không có khảo sát')) {
                this.stop();
                this.ui.writeConsole("🎉 Tuyệt vời! Xong hết rồi.", "success");

                // Cập nhật lại UI tắt nút switch luôn cho đồng bộ
                const toggleAuto = this.ui.shadow.querySelector('#bkhn-toggle-auto');
                if (toggleAuto && toggleAuto.checked) {
                    toggleAuto.checked = false;
                    this.ui.saveSetting('isEnabled', false);
                }

                if (manual) alert("Đã hoàn thành toàn bộ khảo sát!");
                return;
            }

            let targetDoc = document;
            let radios = targetDoc.querySelectorAll(this.config.selectors.radioTarget);

            if (radios.length === 0) {
                const frames = document.querySelectorAll('iframe, frame');
                for (let i = 0; i < frames.length; i++) {
                    try {
                        let frameDoc = frames[i].contentDocument || frames[i].contentWindow.document;
                        let frameRadios = frameDoc.querySelectorAll(this.config.selectors.radioTarget);
                        if (frameRadios.length > 0) {
                            radios = frameRadios;
                            targetDoc = frameDoc;
                            break;
                        }
                    } catch (e) { }
                }
            }

            if (radios.length === 0) {
                if (manual) this.ui.writeConsole("❌ Không tìm thấy form ở đâu cả!", "error");
                return;
            }

            this.stop();
            this.ui.writeConsole(`🔥 Bẻ khóa thành công! Tìm thấy ${radios.length} câu. Đang tick...`, 'success');

            try {
                const groups = {};
                radios.forEach(r => {
                    const name = r.name || (r.closest('table') ? r.closest('table').id : 'unknown');
                    if (!groups[name]) groups[name] = [];
                    groups[name].push(r);
                });

                this.autoFillAnswers(groups, this.ui.settings.strategy);
                this.submitForm(targetDoc);
            } catch (error) {
                this.ui.writeConsole(`Lỗi: ${error.message}`, 'error');
            }
        }

        autoFillAnswers(groups, strategy) {
            for (const key in groups) {
                const options = groups[key];
                let targetIndex = options.length - 1;

                let avgIndex = -1;
                for (let i = 0; i < options.length; i++) {
                    const text = (options[i].closest('label, div, tr, td') || options[i].parentElement).innerText.toLowerCase();
                    if (this.config.keywords.averageScore.some(k => text.includes(k))) {
                        avgIndex = i; break;
                    }
                }

                if (avgIndex !== -1) {
                    targetIndex = avgIndex;
                } else if (strategy === 'realistic') {
                    if (options.length >= 3) targetIndex = Math.random() < 0.75 ? options.length - 1 : options.length - 2;
                } else if (strategy === 'random') {
                    targetIndex = Math.floor(Math.random() * options.length);
                }

                if (options[targetIndex]) options[targetIndex].click();
            }
        }

        submitForm(doc) {
            const btns = doc.querySelectorAll(this.config.selectors.submitCandidates);
            const submitBtn = Array.from(btns).find(btn => {
                const text = (btn.value || btn.innerText || '').toLowerCase().trim();
                return this.config.keywords.submitBtn.includes(text);
            });

            if (submitBtn) {
                this.ui.writeConsole("Đang nộp bài...", 'warn');
                submitBtn.click();
                setTimeout(() => {
                    this.ui.writeConsole("🔄 Reload để làm phiếu tiếp...", 'success');
                    window.location.href = window.location.href;
                }, this.config.timers.reloadWaitMs);
            } else {
                this.ui.writeConsole("⚠️ Tick xong nhưng không thấy nút Gửi!", 'error');
            }
        }
    }

    const run = () => { new SurveyAutomator().init(); };
    if (document.readyState === 'complete' || document.readyState === 'interactive') run();
    else window.addEventListener('DOMContentLoaded', run);

})();