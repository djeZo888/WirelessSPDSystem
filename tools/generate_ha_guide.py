#!/usr/bin/env python3
"""Embed the complete local HA guide and REST download, using only the stdlib."""
import argparse
import html
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / 'docs/home-assistant.md'
EXAMPLES = ROOT / 'integrations/home-assistant'
HEADER = ROOT / 'firmware/servers/tconnectpro_868/ha_guide.h'


def example(name):
    path = EXAMPLES / name
    if path.parent != EXAMPLES or not path.is_file():
        raise ValueError(f'Unknown HA example: {name}')
    return f'<!-- ha-example:{name} -->\n```yaml\n{path.read_text().rstrip()}\n```\n<!-- /ha-example:{name} -->'


def refresh_examples(source):
    source = re.sub(r'\{\{include:([a-z-]+\.yaml)\}\}', lambda m: example(m[1]), source)
    return re.sub(r'<!-- ha-example:([a-z-]+\.yaml) -->.*?<!-- /ha-example:\1 -->',
                  lambda m: example(m[1]), source, flags=re.S)


def inline(text):
    # Protect literal code before processing the small Markdown subset used here.
    snippets = []
    def code(match):
        snippets.append('<code>' + html.escape(match[1]) + '</code>')
        return f'\x00{len(snippets)-1}\x00'
    text = re.sub(r'`([^`]+)`', code, text)
    text = html.escape(text)
    text = re.sub(r'\[([^\]]+)\]\(([^\s)]+)\)',
                  lambda m: '<a href="' + m[2] + '" rel="noopener"' +
                  (' target="_blank"' if m[2].startswith(('https://', 'http://')) else '') +
                  '>' + m[1] + '</a>', text)
    text = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', text)
    return re.sub('\x00(\\d+)\x00', lambda m: snippets[int(m[1])], text)


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def render(source):
    lines, out, toc = source.splitlines(), [], []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.startswith('<!--'):
            i += 1
            continue
        if line.startswith('```'):
            language, code = line[3:], []
            i += 1
            while i < len(lines) and not lines[i].startswith('```'):
                code.append(lines[i]); i += 1
            if i == len(lines):
                raise ValueError('Unclosed Markdown code block')
            out.append('<pre><code class="language-' + html.escape(language) + '">' +
                       html.escape('\n'.join(code)) + '</code></pre>')
            i += 1
            continue
        if match := re.match(r'^(#{1,3}) (.+)$', line):
            level, text = len(match[1]), match[2]
            anchor = slug(text)
            out.append(f'<h{level} id="{anchor}">' + inline(text) + f'</h{level}>')
            if level == 2:
                toc.append((anchor, text))
            i += 1
            continue
        if line.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                cells = [cell.strip() for cell in lines[i].strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?', cell) for cell in cells):
                    rows.append(cells)
                i += 1
            out.append('<div class="table-scroll"><table><thead><tr>' +
                       ''.join('<th>' + inline(cell) + '</th>' for cell in rows[0]) +
                       '</tr></thead><tbody>' + ''.join('<tr>' +
                       ''.join('<td>' + inline(cell) + '</td>' for cell in row) +
                       '</tr>' for row in rows[1:]) + '</tbody></table></div>')
            continue
        if re.match(r'^\d+\. ', line):
            items, current = [], []
            while i < len(lines) and lines[i].strip():
                if match := re.match(r'^\d+\. (.*)', lines[i]):
                    if current:
                        items.append(' '.join(current))
                    current = [match[1]]
                else:
                    current.append(lines[i].strip())
                i += 1
            if current:
                items.append(' '.join(current))
            out.append('<ol>' + ''.join('<li>' + inline(item) + '</li>' for item in items) + '</ol>')
            continue
        paragraph = []
        while i < len(lines) and lines[i].strip() and not lines[i].startswith(('```', '#', '|', '<!--')):
            paragraph.append(lines[i].strip()); i += 1
        out.append('<p>' + inline(' '.join(paragraph)) + '</p>')
    navigation = '<nav aria-label="Setup steps"><h2>In this guide</h2><ol>' + ''.join(
        '<li><a href="#' + anchor + '">' + inline(text) + '</a></li>' for anchor, text in toc) + '</ol></nav>'
    return '\n'.join(out[:2]) + '\n' + navigation + '\n' + '\n'.join(out[2:])


STYLE = '''
:root{color-scheme:light;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#17242e;background:#fff;font-size:15px;line-height:1.65}*{box-sizing:border-box}body{margin:0}main{max-width:960px;margin:auto;padding:28px 30px 42px}h1,h2,h3{line-height:1.25;letter-spacing:-.025em}h1{font-size:29px;margin:12px 0 18px}h2{font-size:21px;margin:38px 0 14px;scroll-margin-top:18px}h3{font-size:17px}p{margin:13px 0}a{color:#087f76;text-underline-offset:3px;overflow-wrap:anywhere}a:focus-visible{outline:3px solid #4cc3b4;outline-offset:3px}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:.86em;background:#f0f4f6;padding:2px 4px;border-radius:3px;overflow-wrap:anywhere}pre{max-width:100%;overflow-x:auto;border:1px solid #dce4e9;border-radius:9px;background:#f5f8fa;padding:15px;line-height:1.5;font-size:13px}pre code{background:none;padding:0;white-space:pre;overflow-wrap:normal;font-size:inherit}li{margin:8px 0;padding-left:4px}nav{padding:18px 22px;border:1px solid #dce4e9;border-radius:12px;background:#f9fbfc;margin:22px 0}nav h2{margin:0 0 10px;font-size:17px}nav ol{list-style:none;padding-left:0;margin:0;columns:2;column-gap:25px}nav li{padding-left:0;break-inside:avoid;font-size:13px;margin:5px 0}.table-scroll{max-width:100%;overflow-x:auto;border:1px solid #dce4e9;border-radius:8px;margin:18px 0}table{border-collapse:collapse;width:100%;font-size:13px}th,td{padding:10px 12px;text-align:left;border-bottom:1px solid #e5ebef;vertical-align:top}th{background:#f5f8fa}tr:last-child td{border-bottom:0}.embedded .downloads{display:none}.downloads{display:flex;flex-wrap:wrap;gap:12px;align-items:center;font-size:12px;padding-bottom:15px;border-bottom:1px solid #e5ebef}.downloads a{display:inline-block;padding:7px 10px;background:#f5f8fa;border:1px solid #dce4e9;border-radius:7px;text-decoration:none}.foot{font-size:12px;color:#637583;margin-top:32px;border-top:1px solid #e5ebef;padding-top:16px}@media(max-width:600px){main{padding:16px 16px 30px}h1{font-size:25px}h2{font-size:19px;margin-top:30px}nav{padding:15px}nav ol{columns:1}pre{font-size:11px;padding:12px}th,td{padding:9px}}@media print{main{padding:0;max-width:none}nav{display:none}.downloads{display:none}pre{white-space:pre-wrap;overflow:visible}pre code{white-space:pre-wrap;overflow-wrap:anywhere}h2{break-after:avoid}table{break-inside:avoid}a{color:inherit}}
'''.strip()


def generated(source):
    document = '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wireless SPD System — Home Assistant setup</title><style>' + STYLE + '</style></head><body><main>\n'
    document += '<div class="downloads"><a href="/ha-guide" download="WirelessSPDSystem-Home-Assistant.html">Save complete HTML guide</a><a href="/ha-rest.yaml" download="wireless-spd-rest.yaml">Download REST YAML</a><a href="/" target="_top">Receiver dashboard</a></div>\n'
    document += render(source) + '\n<p class="foot">This guide and all example code are stored in the receiver. External reference links are optional. Saved HTML remains readable without a network connection.</p>\n</main></body></html>\n'
    yaml = (EXAMPLES / 'rest-example.yaml').read_text()
    assert ')HAHTML"' not in document and ')HAYAML"' not in yaml
    header = '// Generated by tools/generate_ha_guide.py; edit the canonical guide/examples.\n#pragma once\n\n'
    header += 'constexpr char HA_GUIDE_HTML[] PROGMEM = R"HAHTML(' + document + ')HAHTML";\n\n'
    header += 'constexpr char HA_REST_YAML[] PROGMEM = R"HAYAML(' + yaml + ')HAYAML";\n'
    return {DOC: source, EXAMPLES / 'guide.html': document, HEADER: header}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Reject stale generated guide/header/example blocks')
    args = parser.parse_args()
    artifacts = generated(refresh_examples(DOC.read_text()))
    stale = [str(path.relative_to(ROOT)) for path, text in artifacts.items() if not path.exists() or path.read_text() != text]
    if args.check:
        if stale:
            raise SystemExit('Regenerate the HA guide: ' + ', '.join(stale))
    else:
        for path, text in artifacts.items():
            path.write_text(text)
    print('Home Assistant guide and embedded REST example are synchronized.')


if __name__ == '__main__':
    main()
