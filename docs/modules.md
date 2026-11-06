# JavaScript modules

Браузер выполняет внешние и inline `<script type="module">` после разбора
HTML, до DOMContentLoaded. Classic-скрипты с nomodule пропускаются.

Поддержаны:

- import './side-effect.js';
- import defaultValue, {name as alias} from './dependency.js';
- import * as namespace from './dependency.js';
- export var/let/const, function, class и export default;
- export {local as publicName}, export {name as alias} from './other.js';
- export * from и export * as namespace from;
- import.meta.url, import.meta.resolve();
- import(specifier), возвращающий Promise с namespace или отклонением;
- live bindings, запрет присваивания импортам, отдельные области переменных;
- кэш модулей по разрешённому URL, однократное выполнение, циклы функций;
- ошибки загрузки, MIME, отсутствующих экспортов и повторных имён.

Используется лексер движка, чтобы слова import/export внутри строк,
комментариев и регулярных выражений не меняли программу. Модули преобразуются
в изолированные generator-функции. Сначала создаются экспортные getter-функции
и связывается граф, затем выполняются тела зависимостей. Кэш живёт в realm
страницы и очищается при навигации. Пределы: 128 модулей, 6 МиБ исходников,
общий бюджет выполнения скриптов страницы.

Это рабочее подмножество ES modules, а не полная реализация стандарта.
Пока отсутствуют import maps/bare specifiers, import attributes, top-level
await, destructuring в экспортируемом объявлении и полная обработка конфликтов
export-star. Не реализованы полностью strict-mode semantics и temporal dead
zone в циклах. События MediaQueryList и возможности JS/Web API вне модулей
также имеют собственные ограничения.

Проверки: make jstest (tests/modules_test.py), make browsertest (ASan/UBSan),
python3 tests/browser_smoke.py (настоящая MyOS в QEMU, отдельный образ диска,
сжатые HTTP-зависимости, live bindings и dynamic import).

Проверка текущего mkpnet.ru загрузила index, rolldown-runtime, react-vendor,
markdown-vendor, useToasts, createContextHook, ui-vendor и utils-vendor.
Добавлены async function*, async generator methods, очередь next/throw/return,
await внутри генератора, ожидание Promise из yield и for await...of
(включая обычные iterable с Promise-значениями). Есть базовое yield* из
асинхронного и обычного итератора. Очередь и ожидания сохраняются GC.
Тесты проверяют конкурентные next(), rejected yield, finally при return/break,
завершение генератора и асинхронную итерацию с таймером.

У делегирования yield* остаются ограничения пересылки throw/return и
возвращаемого значения; полное соответствие спецификации не заявляется.
Вызов return с Promise на уже завершённом генераторе также требует доработки.

После расширения async текущий mkpnet.ru выполняет модули и показывает
содержимое главной страницы. Последующие блокеры Blob, AbortController и
контракт Element.animate устранены. Текущий результат и оставшиеся ограничения
описаны в [compatibility.md](compatibility.md).
