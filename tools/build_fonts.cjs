// Regenerate the complete UI subset from source, retaining previous glyphs.
const fs = require('fs');
const cp = require('child_process');
const path = require('path');
process.chdir(path.resolve(__dirname, '..'));
const converter = process.argv[2];
if (!converter) throw Error('Pass the absolute path to lv_font_conv.js');
const sources = fs.readdirSync('main').filter(f => f.endsWith('.c') || f === 'content_index.h')
  .map(f => fs.readFileSync('main/' + f, 'utf8')).join('');
const previous = fs.readFileSync('main/fonts/round_clock_cn_20.c', 'utf8').split('******************************************************************************/')[0];
// Quota window labels arrive from the existing LLMQuota snapshot contract.
const symbols = [...new Set((sources + previous + '每周每月小时估算').match(/[^\x00-\x7F]/gu))].join('');
for (const size of [16, 20, 28]) {
  if (size === 20 && process.argv.includes('--ui-only')) continue;
  // Dynamic task titles and voice notes need Chinese beyond the static UI subset.
  // Two-bit antialiasing keeps the full basic CJK body font within flash/PSRAM budget.
  cp.execFileSync(process.execPath, [converter, '--no-compress', '--no-prefilter', '--bpp', size === 20 ? '2' : '4',
    '--size', String(size), '--font', 'managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf',
    // Exclude vertical Japanese repeat marks U+3031..3035: their two-line
    // metrics inflate every UI row. Standard Chinese punctuation ends at 301F.
    '-r', size === 20 ? '0x20-0x7F,0x2000-0x206F,0x2190-0x21FF,0x3000-0x301F,0x4E00-0x9FFF,0xFF01-0xFF60' : '0x20-0x7F', '--symbols', symbols, '--format', 'lvgl', '--force-fast-kern-format',
    '--lv-font-name', `round_clock_cn_${size}`, '-o', `main/fonts/round_clock_cn_${size}.c`], {stdio:'inherit'});
}
console.log(`${symbols.length} non-ASCII UI glyphs, 16/20/28 px`);
