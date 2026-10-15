// Страница «Поддержка и загрузки»: что эмулируется по-настоящему (карточка на тип: как подключить
// в 1С, скачать драйвер), остальное оборудование, установщик для других компьютеров.
// Данные — GET /api/v1/downloads, открыт без токена. Выводятся только через textContent.
"use strict";

const $ = (id) => document.getElementById(id);

function el(tag, attrs, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") e.className = v;
    else e.setAttribute(k, v);
  }
  for (const c of children) {
    if (c == null) continue;
    e.append(c instanceof Node ? c : String(c));
  }
  return e;
}

function size(n) {
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " МБ";
  if (n >= 1 << 10) return Math.round(n / (1 << 10)) + " КБ";
  return n + " Б";
}

// Как подключить в 1С — по типу оборудования (тип экземпляра в БПО и что проверить).
const HOWTO = {
  kkt: {
    type: "ККТ с передачей данных в ОФД",
    check: "«Тест устройства», затем открыть смену и пробить чек.",
  },
  acquiring: {
    type: "Эквайринговый терминал",
    check: "«Тест устройства», затем оплата картой: карту «прикладывают» в окне терминала (трей у кассы или карточка устройства в панели управления).",
  },
  scales: {
    type: "Электронные весы",
    check: "«Тест устройства», затем «Получить вес»: вес кладут в карточке весов в панели управления или вводят в окне весов в трее (режим «спрашивать вес»).",
  },
  label_scales: {
    type: "Весы с печатью этикеток",
    check: "«Тест устройства», затем выгрузка товаров в весы; этикетку печатают кнопкой у товара в карточке весов в панели управления.",
  },
  receipt_printer: {
    type: "Принтер чеков",
    check: "«Тест устройства», затем печать документа: он появится на ленте принтера в панели управления и во всплывающем окне трея.",
  },
  label_printer: {
    type: "Принтер этикеток",
    check: "«Тест устройства», затем «Печать этикеток и ценников» с шаблоном для принтера этикеток: вид этикеток — на ленте принтера в панели управления.",
  },
};

function downloadButton(f, label, cls) {
  if (!f) return el("span", { class: "muted" }, "не опубликован");
  return el("a", { class: "btn " + (cls || "primary") + " dl-btn", href: f.url, download: f.name }, label);
}

function supportedCard(s) {
  const pkg = s.package;
  const how = HOWTO[s.key] || { type: s.equipment_type_ru, check: "«Тест устройства»." };
  const driverName = "ЭМУЛЯТОР Эмулкон" + (pkg ? " " + pkg.version : "") + " (imiron.ru)";
  const steps = el("ol", { class: "dl-steps" },
    el("li", {}, "Справочник ", el("b", {}, "«Драйверы оборудования»"), " → ", el("b", {}, "«Загрузить новый драйвер из файла»"),
      " → архив этого типа (или общий — сразу для всех типов)."),
    el("li", {}, el("b", {}, "«Подключаемое оборудование»"), " → создать экземпляр: тип ", el("b", {}, how.type),
      ", драйвер ", el("b", {}, "«" + driverName + "»"), "."),
    el("li", {}, "В настройке — ", el("b", {}, "«Адрес сервера»"), ": ", copyable(location.origin), "."),
    el("li", {}, how.check));
  return el("article", { class: "dl-card" },
    el("div", { class: "dl-card-head" },
      el("h3", {}, s.title),
      el("span", { class: "chip ok" }, "модель"),
      s.driver ? el("span", { class: "chip ok" }, "драйвер есть") : null),
    el("div", { class: "muted" }, s.equipment_type_ru + " · " + s.equipment_type),
    s.features && s.features.length ? el("ul", { class: "dl-features" }, ...s.features.map((f) => el("li", {}, f))) : null,
    el("details", { class: "dl-howto" }, el("summary", {}, "Как подключить в 1С"), steps),
    el("div", { class: "dl-card-foot" },
      s.driver ? downloadButton(pkg, "Скачать драйвер этого типа", "") : el("span", { class: "muted" }, "драйвер в разработке"),
      pkg && s.driver ? el("span", { class: "dl-meta" }, `${pkg.name} · ${size(pkg.size)}`) : null));
}

function copyable(text) {
  const code = el("code", { class: "dl-copy", title: "Скопировать" }, text);
  code.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(text);
      code.classList.add("copied");
      setTimeout(() => code.classList.remove("copied"), 1200);
    } catch (_) { /* буфер обмена недоступен (http не на localhost) — адрес виден и так */ }
  });
  return code;
}

function render(c) {
  $("version").textContent = "v" + c.server_version;
  $("revision").textContent = "требования к драйверам БПО " + c.interface_revision;
  $("dl-unpublished").hidden = c.published;

  const eq = c.equipment || [];
  const full = eq.filter((s) => s.model === "full");
  $("dl-supported").replaceChildren(...full.map(supportedCard));

  const g = c.driver_package;
  const types = eq.filter((s) => s.driver).map((s) => s.title).join(", ");
  $("dl-general").replaceChildren(g
    ? el("div", { class: "dl-setup-row" }, downloadButton(g, "Скачать общий драйвер"),
      el("span", { class: "dl-meta" }, `${g.name} · версия ${g.version} · ${size(g.size)}`),
      el("div", { class: "muted dl-types" }, "Типы: " + types + "."))
    : el("span", { class: "muted" }, "Драйвер на этом сервере не опубликован."));

  const rest = eq.filter((s) => s.model !== "full");
  $("support-body").closest("section").hidden = rest.length === 0; // все типы на настоящих моделях
  $("support-body").replaceChildren(...rest.map((s) =>
    el("tr", {},
      el("td", {}, s.title, el("div", { class: "muted" }, s.equipment_type_ru + " · " + s.equipment_type)),
      el("td", {}, el("span", { class: "chip" }, "заглушка")),
      el("td", {}, s.driver ? downloadButton(s.package, "Скачать", "") : el("span", { class: "muted" }, "в разработке")))));

  // Установщик нужен другим компьютерам: на самом сервере Эмулкон уже стоит.
  const local = ["127.0.0.1", "localhost", "[::1]"].includes(location.hostname);
  $("setup-here").textContent = local
    ? "На этом компьютере Эмулкон уже работает. Установщик — для касс и других рабочих мест: откройте эту страницу там по адресу сервера в сети."
    : "";
  $("setup-here").hidden = !local;
  $("setup-file").replaceChildren(c.setup
    ? el("div", { class: "dl-setup-row" }, downloadButton(c.setup, "Скачать установщик", ""),
      el("span", { class: "dl-meta" }, `${c.setup.name} · версия ${c.setup.version} · ${size(c.setup.size)}`))
    : el("span", { class: "muted" }, "Установщик на этом сервере не опубликован."));

  const dlls = (c.driver_dlls || []).map((f) =>
    el("tr", {},
      el("td", {}, el("a", { href: f.url, download: f.name, class: "mono" }, f.name)),
      el("td", {}, f.arch), el("td", {}, f.version),
      el("td", { class: "num" }, size(f.size)),
      el("td", { class: "mono" }, f.sha256)));
  $("dll-body").replaceChildren(...(dlls.length ? dlls : [el("tr", {}, el("td", { colspan: "5", class: "muted" }, "нет файлов"))]));
}

async function load() {
  try {
    const r = await fetch("/api/v1/downloads", { cache: "no-store" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    render(await r.json());
  } catch (e) {
    const w = $("dl-error");
    w.textContent = "Не удалось получить каталог загрузок: " + e.message;
    w.hidden = false;
  }
}

load();
