(() => {
  'use strict';

  const I18N = {
    en: {
      'a11y.skip': 'Skip to content',
      'nav.homeAria': 'PedalBoard home',
      'nav.guide': 'Getting started',
      'nav.effects': 'Effects',
      'nav.recording': 'Recording',
      'nav.advanced': 'Advanced',
      'nav.download': 'Download',
      'nav.language': 'Language',
      'nav.githubAria': 'Open PedalBoard on GitHub',
      'nav.menu': 'Open menu',
      'nav.close': 'Close menu',
      'gate.kicker': 'FIRST VISIT',
      'gate.title': 'Choose your language',
      'gate.lead': 'PedalBoard has a full English and Chinese interface. Pick one now — you can switch it in the header at any time.',
      'gate.zhNote': '完整中文界面',
      'gate.enNote': 'Full English interface',
      'gate.skip': 'Continue with the detected language',
      'shot.enlarge': 'Enlarge screenshot',
      'shot.zoom': 'Tap to enlarge',
      'shot.dialog': 'Screenshot preview',
      'shot.close': 'Close screenshot',
      'shot.pan': 'Swipe to pan',
      'fx.filterAria': 'Filter effects by category',
      'fx.all': 'All',
      'fx.dynamics': 'Dynamics',
      'fx.filter': 'Filter',
      'fx.drive': 'Drive',
      'fx.mod': 'Modulation',
      'fx.pitch': 'Pitch',
      'fx.time': 'Time',
      'fx.special': 'Special',
      'fx.count': '{n} effect types',
      'fx.startTitle': 'Start with these six',
      'fx.startLead': 'You do not need all 24 on day one. These are the easiest to hear and the hardest to make sound bad.',
      'dl.kicker': 'DOWNLOAD',
      'dl.title': 'Unzip it. Play.',
      'dl.lead': 'Windows 10/11 x64. Unzip, run, choose your language. No installer, no account, no telemetry.',
      'dl.primary': 'Download for Windows',
      'dl.secondary': 'View source',
      'dl.trust1': 'No installer',
      'dl.trust2': 'No account',
      'dl.trust3': 'AGPLv3',
      'dl.badge': 'LATEST',
      'dl.platformLabel': 'Platform',
      'dl.platformValue': 'Windows 10/11 x64',
      'dl.sizeLabel': 'Download',
      'dl.sizeValue': '3.4 MB zip',
      'dl.licenseLabel': 'License',
      'dl.buildLabel': 'Build',
      'dl.buildValue': 'Release · static CRT',
      'dl.shaLabel': 'SHA-256',
      'dl.copy': 'Copy',
      'dl.copied': 'Copied',
      'dl.manual': 'Press Ctrl+C',
      'dl.copyAria': 'Copy SHA-256 checksum',
      'dl.step1t': 'Download',
      'dl.step1d': 'Grab the zip from GitHub Releases.',
      'dl.step2t': 'Unzip & run',
      'dl.step2d': 'Any simple path such as D:\\PedalBoard. PedalBoard.exe is self-contained.',
      'dl.step3t': 'Pick a language',
      'dl.step3d': 'English or 中文, then open Devices and choose your input.',
      'dl.note1': 'The exe is not code-signed, so SmartScreen may warn on first run: More info → Run anyway.',
      'dl.note2': 'Audacity and VB-CABLE are independent third-party software and are not bundled with PedalBoard.',
      'dl.guideLink': 'Read the full setup guide',
      'board.chainAria': 'Reorder the default chain',
      'board.moveUp': 'Move up',
      'board.moveDown': 'Move down',
      'board.reset': 'Reset',
      'board.chainHint': 'Drag a card or use ↑ ↓',
      'board.moved': '{name} now sits in slot {slot}.',
      'board.resetDone': 'Chain reset to the default order.',
      'footer.tag': 'A Windows guitar pedalboard that opens ready to play. Free and open source.',
      'footer.built': 'Built with C++ and JUCE 8.',
      'footer.site': 'Site',
      'footer.project': 'Project',
      'footer.third': 'Third-party',
      'footer.source': 'Source code',
      'footer.releases': 'Releases',
      'footer.readme': 'Documentation',
      'footer.license': 'AGPLv3 License',
      'footer.top': 'Back to top',
      'footer.rights': '© 2026 PedalBoard.',
      'footer.note': 'Audacity and VB-CABLE are independent third-party software, not bundled with PedalBoard.',
      'footer.contactKicker': 'CONTACT',
      'footer.contactTitle': 'Questions, bugs, or a song you made?',
      'footer.contactLead': 'Email me directly. I read every message.',
      'meta.fallbackTitle': 'PedalBoard — a Windows guitar pedalboard you can play immediately',
      'meta.fallbackDesc': 'Unzip, choose your devices, and play. A beginner-friendly Windows guitar pedalboard with 24 effect types and a ready-made default chain.'
    },
    zh: {
      'a11y.skip': '跳到正文',
      'nav.homeAria': 'PedalBoard 首页',
      'nav.guide': '新手入门',
      'nav.effects': '效果器',
      'nav.recording': '录音教程',
      'nav.advanced': '进阶',
      'nav.download': '下载',
      'nav.language': '语言',
      'nav.githubAria': '在 GitHub 上打开 PedalBoard',
      'nav.menu': '打开菜单',
      'nav.close': '关闭菜单',
      'gate.kicker': '第一次打开',
      'gate.title': '选择界面语言',
      'gate.lead': 'PedalBoard 提供完整中文和英文界面。现在选一个即可，之后随时可以在右上角切换。',
      'gate.zhNote': '完整中文界面',
      'gate.enNote': 'Full English interface',
      'gate.skip': '使用当前识别到的语言继续',
      'shot.enlarge': '放大查看截图',
      'shot.zoom': '点按放大',
      'shot.dialog': '截图预览',
      'shot.close': '关闭截图',
      'shot.pan': '左右滑动查看',
      'fx.filterAria': '按类别筛选效果器',
      'fx.all': '全部',
      'fx.dynamics': '动态',
      'fx.filter': '滤波',
      'fx.drive': '失真',
      'fx.mod': '调制',
      'fx.pitch': '音高',
      'fx.time': '时间',
      'fx.special': '特殊',
      'fx.count': '{n} 种效果',
      'fx.startTitle': '先玩这六个',
      'fx.startLead': '第一天不需要 24 个全用上。这几个最容易听出区别，也最不容易调难听。',
      'dl.kicker': '下载',
      'dl.title': '解压，就能弹。',
      'dl.lead': 'Windows 10/11 x64。解压、运行、选择语言。免安装、无账号、无遥测。',
      'dl.primary': '下载 Windows 版',
      'dl.secondary': '查看源码',
      'dl.trust1': '免安装',
      'dl.trust2': '无账号',
      'dl.trust3': 'AGPLv3',
      'dl.badge': '最新',
      'dl.platformLabel': '平台',
      'dl.platformValue': 'Windows 10/11 x64',
      'dl.sizeLabel': '下载大小',
      'dl.sizeValue': '3.4 MB 压缩包',
      'dl.licenseLabel': '许可证',
      'dl.buildLabel': '构建',
      'dl.buildValue': 'Release · 静态 CRT',
      'dl.shaLabel': 'SHA-256',
      'dl.copy': '复制',
      'dl.copied': '已复制',
      'dl.manual': '按 Ctrl+C 复制',
      'dl.copyAria': '复制 SHA-256 校验值',
      'dl.step1t': '下载',
      'dl.step1d': '从 GitHub Releases 下载压缩包。',
      'dl.step2t': '解压运行',
      'dl.step2d': '放到 D:\\PedalBoard 这类简单路径，PedalBoard.exe 自带运行库。',
      'dl.step3t': '选择语言',
      'dl.step3d': 'English 或中文，然后打开「音频设备」选择输入。',
      'dl.note1': 'exe 未做代码签名，首次运行可能出现 SmartScreen 提示：更多信息 → 仍要运行。',
      'dl.note2': 'Audacity 与 VB-CABLE 都是独立的第三方软件，不随 PedalBoard 分发。',
      'dl.guideLink': '阅读完整上手教程',
      'board.chainAria': '重排默认链路',
      'board.moveUp': '上移',
      'board.moveDown': '下移',
      'board.reset': '重置',
      'board.chainHint': '拖动卡片或使用 ↑ ↓',
      'board.moved': '{name} 现在在第 {slot} 槽。',
      'board.resetDone': '链路已恢复默认顺序。',
      'footer.tag': '一块打开就能弹的 Windows 吉他效果器板。免费开源。',
      'footer.built': '使用 C++ 与 JUCE 8 构建。',
      'footer.site': '站点',
      'footer.project': '项目',
      'footer.third': '第三方',
      'footer.source': '源码',
      'footer.releases': '版本发布',
      'footer.readme': '文档',
      'footer.license': 'AGPLv3 许可证',
      'footer.top': '回到顶部',
      'footer.rights': '© 2026 PedalBoard。',
      'footer.note': 'Audacity 与 VB-CABLE 是独立的第三方软件，不随 PedalBoard 分发。',
      'footer.contactKicker': '联系',
      'footer.contactTitle': '有问题、发现 bug，或者做出了歌？',
      'footer.contactLead': '直接写信给我，每一封我都会看。',
      'meta.fallbackTitle': 'PedalBoard — 打开就能弹的 Windows 吉他效果器板',
      'meta.fallbackDesc': '解压、选设备、开弹。新手友好的 Windows 吉他效果器板，24 种效果，默认链路已经接好。'
    }
  };

  const SUPPORTED = ['en', 'zh'];
  const STORAGE_KEY = 'pedalboard-site-language';
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const html = document.documentElement;
  const body = document.body;
  const nav = $('#primaryNav');
  const navToggle = $('#navToggle');
  const gate = $('#langGate');
  const copyBtn = $('#copySha');
  const copyLabel = $('#copyLabel');
  const fxCount = $('#fxCount');
  const chainList = $('#miniChain');
  const chainStatus = $('#chainStatus');
  const resetChain = $('#resetChain');
  const filterButtons = $$('.filter-btn');
  const fxCards = $$('.fx-card');

  let current = 'en';
  let copyTimer = 0;
  let dragItem = null;
  let gateReturnFocus = null;

  function readStored() {
    try {
      const stored = window.localStorage.getItem(STORAGE_KEY);
      return SUPPORTED.includes(stored) ? stored : '';
    } catch (_) {
      return '';
    }
  }

  function queryLanguage() {
    try {
      const query = new URLSearchParams(window.location.search).get('lang');
      if (!query) return '';
      return query.toLowerCase().startsWith('zh') ? 'zh' : 'en';
    } catch (_) {
      return '';
    }
  }

  function detectLanguage() {
    const query = queryLanguage();
    if (query) return query;
    const stored = readStored();
    if (stored) return stored;
    return (navigator.language || 'en').toLowerCase().startsWith('zh') ? 'zh' : 'en';
  }

  function translation(key) {
    return I18N[current][key] ?? I18N.en[key] ?? '';
  }

  function updateFxCount() {
    if (!fxCount) return;
    const shown = fxCards.filter(card => !card.hidden).length;
    fxCount.textContent = translation('fx.count').replace('{n}', String(shown));
  }

  function setLanguage(lang, persist = true) {
    if (!SUPPORTED.includes(lang)) lang = 'en';
    current = lang;
    html.lang = lang === 'zh' ? 'zh-CN' : 'en';

    $$('[data-i18n]').forEach(el => {
      const value = translation(el.dataset.i18n);
      if (value) el.textContent = value;
    });

    $$('[data-en]').forEach(el => {
      const value = el.dataset[lang];
      if (value) el.textContent = value;
    });

    $$('[data-en-aria]').forEach(el => {
      const value = el.dataset[lang === 'zh' ? 'zhAria' : 'enAria'];
      if (value) el.setAttribute('aria-label', value);
    });

    $$('[data-i18n-aria]').forEach(el => {
      const value = translation(el.dataset.i18nAria);
      if (value) el.setAttribute('aria-label', value);
    });

    const title = body.dataset[lang === 'zh' ? 'titleZh' : 'titleEn'] || translation('meta.fallbackTitle');
    const description = body.dataset[lang === 'zh' ? 'descZh' : 'descEn'] || translation('meta.fallbackDesc');
    document.title = title;

    const desc = $('meta[name="description"]');
    if (desc) desc.setAttribute('content', description);
    const ogTitle = $('meta[property="og:title"]');
    if (ogTitle) ogTitle.setAttribute('content', title);
    const ogDesc = $('meta[property="og:description"]');
    if (ogDesc) ogDesc.setAttribute('content', description);

    $$('.lang-btn').forEach(btn => {
      btn.setAttribute('aria-pressed', String(btn.dataset.lang === lang));
    });

    if (copyBtn && copyBtn.dataset.copied === 'true') {
      if (copyLabel) copyLabel.textContent = translation('dl.copied');
      copyBtn.setAttribute('aria-label', translation('dl.copied'));
    } else if (copyBtn && copyBtn.dataset.copied === 'manual') {
      if (copyLabel) copyLabel.textContent = translation('dl.manual');
      copyBtn.setAttribute('aria-label', translation('dl.manual'));
    } else if (copyBtn) {
      copyBtn.setAttribute('aria-label', translation('dl.copyAria'));
    }

    if (navToggle && navToggle.getAttribute('aria-expanded') === 'true') {
      navToggle.setAttribute('aria-label', translation('nav.close'));
    }

    updateFxCount();

    if (persist) {
      try { window.localStorage.setItem(STORAGE_KEY, lang); } catch (_) { /* ignore */ }
    }
  }

  /* ---------- first-visit language gate ---------- */
  function closeGate(lang) {
    if (!gate || gate.hidden) return;
    setLanguage(lang || current, true);
    gate.hidden = true;
    body.classList.remove('gate-open');
    if (gateReturnFocus && typeof gateReturnFocus.focus === 'function') gateReturnFocus.focus();
  }

  function openGate() {
    if (!gate) return;
    setLanguage(current, false);
    gateReturnFocus = document.activeElement;
    gate.hidden = false;
    body.classList.add('gate-open');
    const first = $('.lang-choice', gate);
    if (first) first.focus();
  }

  if (gate) {
    gate.addEventListener('click', event => {
      const choice = event.target.closest('[data-gate-lang]');
      if (choice) {
        closeGate(choice.dataset.gateLang);
        return;
      }
      if (event.target.closest('[data-gate-skip]')) {
        closeGate(current);
        return;
      }
      if (event.target === gate) closeGate(current);
    });
    document.addEventListener('keydown', event => {
      if (event.key === 'Escape' && !gate.hidden) closeGate(current);
    });
  }

  $$('.lang-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      setLanguage(btn.dataset.lang, true);
      if (gate && !gate.hidden) {
        gate.hidden = true;
        body.classList.remove('gate-open');
      }
    });
  });

  /* ---------- header ---------- */
  const header = $('#siteHeader');
  const updateHeader = () => {
    if (header) header.classList.toggle('is-scrolled', window.scrollY > 24);
  };
  updateHeader();
  window.addEventListener('scroll', updateHeader, { passive: true });

  function setMenu(open) {
    if (!nav || !navToggle) return;
    nav.classList.toggle('is-open', open);
    navToggle.setAttribute('aria-expanded', String(open));
    navToggle.setAttribute('aria-label', translation(open ? 'nav.close' : 'nav.menu'));
  }

  if (navToggle) {
    navToggle.addEventListener('click', () => {
      setMenu(navToggle.getAttribute('aria-expanded') !== 'true');
    });
  }

  if (nav) {
    $$('a', nav).forEach(link => link.addEventListener('click', () => setMenu(false)));
  }

  document.addEventListener('keydown', event => {
    if (event.key === 'Escape') setMenu(false);
  });

  document.addEventListener('click', event => {
    if (!nav || !nav.classList.contains('is-open')) return;
    if (nav.contains(event.target) || (navToggle && navToggle.contains(event.target))) return;
    setMenu(false);
  });

  /* ---------- reorderable chain demo ---------- */
  const initialOrder = chainList
    ? $$('.chain-item', chainList).map(item => item.dataset.id)
    : [];

  function renumberChain(flash = false) {
    if (!chainList) return;
    const pulse = $('.chain-pulse', chainList);
    $$('.chain-item', chainList).forEach((item, index) => {
      const slot = $('.slot', item);
      if (slot) slot.textContent = String(index + 1).padStart(2, '0');
      if (flash) {
        item.classList.remove('moved');
        void item.offsetWidth;
        item.classList.add('moved');
      }
    });
    if (pulse) chainList.appendChild(pulse);
  }

  function announceChain(item, reset = false) {
    if (!chainStatus) return;
    if (reset) {
      chainStatus.textContent = translation('board.resetDone');
      return;
    }
    if (!item) return;
    const name = $('.name', item)?.textContent?.trim() || '';
    const slot = $('.slot', item)?.textContent || '';
    chainStatus.textContent = translation('board.moved')
      .replace('{name}', name)
      .replace('{slot}', slot);
  }

  if (chainList) {
    chainList.addEventListener('click', event => {
      const button = event.target.closest('button[data-move]');
      if (!button) return;
      const item = button.closest('.chain-item');
      if (!item) return;
      const items = $$('.chain-item', chainList);
      const index = items.indexOf(item);
      if (button.dataset.move === 'up' && index > 0) {
        chainList.insertBefore(item, items[index - 1]);
      } else if (button.dataset.move === 'down' && index < items.length - 1) {
        chainList.insertBefore(items[index + 1], item);
      } else {
        return;
      }
      renumberChain(true);
      announceChain(item);
    });

    chainList.addEventListener('dragstart', event => {
      if (event.target.closest('button')) {
        event.preventDefault();
        return;
      }
      const item = event.target.closest('.chain-item');
      if (!item) return;
      dragItem = item;
      item.classList.add('dragging');
      event.dataTransfer.effectAllowed = 'move';
      event.dataTransfer.setData('text/plain', item.dataset.id || '');
    });

    chainList.addEventListener('dragover', event => {
      if (!dragItem) return;
      event.preventDefault();
      const target = event.target.closest('.chain-item');
      if (!target || target === dragItem) return;
      const rect = target.getBoundingClientRect();
      const after = event.clientY - rect.top > rect.height / 2;
      chainList.insertBefore(dragItem, after ? target.nextElementSibling : target);
      renumberChain(false);
    });

    chainList.addEventListener('drop', event => event.preventDefault());

    chainList.addEventListener('dragend', () => {
      if (!dragItem) return;
      const item = dragItem;
      dragItem = null;
      item.classList.remove('dragging');
      renumberChain(true);
      announceChain(item);
    });

    if (resetChain) {
      resetChain.addEventListener('click', () => {
        const pulse = $('.chain-pulse', chainList);
        initialOrder.forEach(id => {
          const item = chainList.querySelector(`[data-id="${id}"]`);
          if (item) chainList.insertBefore(item, pulse);
        });
        renumberChain(true);
        announceChain(null, true);
      });
    }
  }

  /* ---------- effect filters ---------- */
  function applyFilter(category, animate = true) {
    let shown = 0;
    fxCards.forEach(card => {
      const visible = category === 'all' || card.dataset.cat === category;
      card.hidden = !visible;
      card.classList.remove('is-in');
      if (visible) {
        shown += 1;
        if (animate) {
          void card.offsetWidth;
          card.classList.add('is-in');
        }
      }
    });
    filterButtons.forEach(button => {
      const active = button.dataset.filter === category;
      button.classList.toggle('is-active', active);
      button.setAttribute('aria-pressed', String(active));
    });
    if (fxCount) fxCount.textContent = translation('fx.count').replace('{n}', String(shown));
  }

  filterButtons.forEach(button => {
    button.addEventListener('click', () => applyFilter(button.dataset.filter));
  });

  /* ---------- copy checksum ---------- */
  if (copyBtn) {
    copyBtn.addEventListener('click', async () => {
      const hash = copyBtn.dataset.hash || '';
      let copied = false;
      try {
        await navigator.clipboard.writeText(hash);
        copied = true;
      } catch (_) { /* fall through to a selection based copy */ }

      if (!copied) {
        const oldField = document.getElementById('copyFallback');
        if (oldField) oldField.remove();
        const field = document.createElement('textarea');
        field.id = 'copyFallback';
        field.value = hash;
        field.setAttribute('readonly', '');
        field.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;pointer-events:none';
        document.body.appendChild(field);
        field.focus();
        field.select();
        try { copied = document.execCommand('copy'); } catch (_) { copied = false; }
        if (copied) {
          field.remove();
        } else {
          // Clipboard APIs can be blocked (file://, focus rules). Leave the
          // full hash selected so the user can finish with Ctrl+C.
          copyBtn.dataset.copied = 'manual';
          copyBtn.classList.add('is-copied');
          copyBtn.setAttribute('aria-label', translation('dl.manual'));
          if (copyLabel) copyLabel.textContent = translation('dl.manual');
          window.clearTimeout(copyTimer);
          copyTimer = window.setTimeout(() => {
            field.remove();
            copyBtn.dataset.copied = 'false';
            copyBtn.classList.remove('is-copied');
            if (copyLabel) copyLabel.textContent = translation('dl.copy');
            copyBtn.setAttribute('aria-label', translation('dl.copyAria'));
          }, 8000);
          return;
        }
      }

      copyBtn.dataset.copied = 'true';
      copyBtn.classList.add('is-copied');
      copyBtn.setAttribute('aria-label', translation('dl.copied'));
      if (copyLabel) copyLabel.textContent = translation('dl.copied');
      window.clearTimeout(copyTimer);
      copyTimer = window.setTimeout(() => {
        copyBtn.dataset.copied = 'false';
        copyBtn.classList.remove('is-copied');
        if (copyLabel) copyLabel.textContent = translation('dl.copy');
        copyBtn.setAttribute('aria-label', translation('dl.copyAria'));
      }, 1800);
    });
  }

  /* ---------- screenshot lightbox ---------- */
  function initShotLightbox() {
    const frames = $$('.shot-frame').filter(frame => frame.querySelector('img') && !frame.closest('.shot-prompt'));
    if (!frames.length) return;

    const overlay = document.createElement('div');
    overlay.className = 'lightbox';
    overlay.hidden = true;
    overlay.innerHTML = `
      <div class="lightbox-shell" role="dialog" aria-modal="true" aria-labelledby="lightboxLabel">
        <div class="lightbox-bar">
          <div class="lightbox-meta">
            <span class="lightbox-label" id="lightboxLabel" data-i18n="shot.dialog"></span>
            <span class="lightbox-pan" data-i18n="shot.pan" aria-hidden="true"></span>
          </div>
          <button class="lightbox-close" type="button" data-i18n-aria="shot.close"><span aria-hidden="true">&times;</span></button>
        </div>
        <div class="lightbox-scroll"><div class="lightbox-stage"></div></div>
        <div class="lightbox-caption"></div>
      </div>`;
    body.appendChild(overlay);

    const stage = $('.lightbox-stage', overlay);
    const scroll = $('.lightbox-scroll', overlay);
    const caption = $('.lightbox-caption', overlay);
    const closeBtn = $('.lightbox-close', overlay);
    let returnFocus = null;

    function close() {
      if (overlay.hidden) return;
      overlay.hidden = true;
      body.classList.remove('lb-open');
      stage.textContent = '';
      if (returnFocus && document.contains(returnFocus)) returnFocus.focus();
      returnFocus = null;
    }

    function open(frame, img) {
      returnFocus = document.activeElement;
      stage.textContent = '';
      const copy = img.cloneNode(true);
      copy.removeAttribute('loading');
      copy.removeAttribute('tabindex');
      copy.removeAttribute('role');
      copy.removeAttribute('data-i18n-aria');
      copy.className = 'lightbox-img';
      stage.appendChild(copy);
      $$('.shot-pin', frame).forEach(pin => stage.appendChild(pin.cloneNode(true)));
      const figure = frame.closest('.shot-figure') || frame;
      const figcaption = $('.shot-caption', figure);
      caption.textContent = '';
      if (figcaption) caption.appendChild(figcaption.cloneNode(true));
      const legend = figure.nextElementSibling;
      if (legend && legend.classList.contains('shot-legend')) {
        caption.appendChild(legend.cloneNode(true));
      }
      overlay.hidden = false;
      body.classList.add('lb-open');
      scroll.scrollTop = 0;
      scroll.scrollLeft = 0;
      closeBtn.focus();
      window.requestAnimationFrame(() => {
        const firstPin = $('.shot-pin', stage);
        if (!firstPin) return;
        scroll.scrollLeft = Math.max(0, firstPin.offsetLeft - scroll.clientWidth / 2);
      });
    }

    frames.forEach(frame => {
      $$('img', frame).forEach(img => {
        img.classList.add('is-zoomable');
        img.tabIndex = 0;
        img.setAttribute('role', 'button');
        img.setAttribute('data-i18n-aria', 'shot.enlarge');
        img.addEventListener('click', () => open(frame, img));
        img.addEventListener('keydown', event => {
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            open(frame, img);
          }
        });
      });

      const hint = document.createElement('span');
      hint.className = 'shot-zoomhint';
      hint.dataset.i18n = 'shot.zoom';
      hint.setAttribute('aria-hidden', 'true');
      frame.appendChild(hint);
    });

    closeBtn.addEventListener('click', close);
    overlay.addEventListener('click', event => {
      if (event.target === overlay) close();
    });

    document.addEventListener('keydown', event => {
      if (overlay.hidden) return;
      if (event.key === 'Escape') {
        event.preventDefault();
        close();
        return;
      }
      if (event.key !== 'Tab') return;
      const focusables = $$('button, a[href], [tabindex]:not([tabindex="-1"])', overlay)
        .filter(el => el.offsetParent !== null);
      if (!focusables.length) return;
      const first = focusables[0];
      const last = focusables[focusables.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    });
  }

  initShotLightbox();

  /* ---------- reveal on scroll ---------- */
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const reveals = $$('.reveal');
  if (reduceMotion || !('IntersectionObserver' in window)) {
    reveals.forEach(el => el.classList.add('is-visible'));
  } else {
    const observer = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add('is-visible');
        observer.unobserve(entry.target);
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -36px 0px' });
    reveals.forEach(el => observer.observe(el));

    // Fallback for deep links and environments where the observer is late.
    let revealTick = 0;
    const revealInView = () => {
      revealTick = 0;
      const limit = window.innerHeight * 0.94;
      reveals.forEach(el => {
        if (el.classList.contains('is-visible')) return;
        const rect = el.getBoundingClientRect();
        if (rect.top < limit && rect.bottom > 0) el.classList.add('is-visible');
      });
    };
    const queueReveal = () => {
      if (revealTick) return;
      revealTick = window.requestAnimationFrame(revealInView);
    };
    window.addEventListener('scroll', queueReveal, { passive: true });
    window.addEventListener('load', queueReveal);
    window.setTimeout(revealInView, 900);
  }

  /* ---------- init ---------- */
  const initial = detectLanguage();
  setLanguage(initial, false);
  applyFilter('all', false);
  renumberChain(false);

  if (gate && !readStored() && !queryLanguage()) openGate();

  $$('[data-year]').forEach(el => { el.textContent = String(new Date().getFullYear()); });
})();
