#!/usr/bin/env python3
"""Validate the self-contained static site without network or third-party tools."""
from __future__ import annotations
from html.parser import HTMLParser
from pathlib import Path
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / 'docs'


class Page(HTMLParser):
    def __init__(self, path: Path):
        super().__init__(convert_charrefs=True)
        self.path = path
        self.links: list[str] = []
        self.ids: set[str] = set()
        self.language: str | None = None
        self.title = False
        self.main = False
        self.feed(path.read_text(encoding='utf-8'))
        self.close()

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == 'html':
            self.language = values.get('lang')
        if tag == 'title':
            self.title = True
        if tag == 'main':
            self.main = True
        if 'id' in values:
            self.ids.add(values['id'])
        for attr in ('href', 'src'):
            if values.get(attr):
                self.links.append(values[attr])


def main() -> int:
    pages = {path.resolve(): Page(path) for path in DOCS.glob('*.html')}
    failures: list[str] = []
    if len(pages) < 12:
        failures.append('Expected all twelve documentation pages')
    if not (DOCS / '.nojekyll').is_file():
        failures.append('Missing .nojekyll')
    checked = 0
    for path, page in pages.items():
        label = path.relative_to(ROOT)
        if page.language != 'en' or not page.title or not page.main:
            failures.append(f'{label}: missing English language, title, or main element')
        for link in page.links:
            parsed = urlsplit(link)
            if parsed.scheme or parsed.netloc:
                # External references are not verified without a network request.
                continue
            target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
            if not target.is_relative_to(DOCS.resolve()):
                failures.append(f'{label}: link leaves the published docs tree: {link}')
                continue
            if not target.is_file():
                failures.append(f'{label}: missing target: {link}')
            elif parsed.fragment and target.suffix == '.html':
                target_page = pages.get(target)
                if target_page is None or unquote(parsed.fragment) not in target_page.ids:
                    failures.append(f'{label}: missing fragment: {link}')
            checked += 1
    if failures:
        print('\n'.join(failures), file=sys.stderr)
        return 1
    print(f'PASS: {len(pages)} English HTML pages; {checked} local links and fragments; self-contained docs tree')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
