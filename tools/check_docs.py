#!/usr/bin/env python3
"""Check current Raynium documentation, project skills and source doc references.

Only Git-tracked and non-ignored new files participate. History references retain
their recorded baseline. Web URLs and Markdown fragments are not validated.
Run from any directory with: python tools/check_docs.py
"""

from __future__ import annotations

from calendar import monthrange
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys
from urllib.parse import unquote


def absolute_path(value: str) -> bool:
    return (PurePosixPath(value).is_absolute()
            or bool(PureWindowsPath(value).drive)
            or bool(PureWindowsPath(value).root))


def check(root: Path, files: list[Path]) -> tuple[list[str], dict[str, int]]:
    root = root.resolve()
    known = {path.relative_to(root).as_posix() for path in files}
    errors: list[str] = []
    counts = dict(files=0, pages=0, anchors=0, links=0, references=0, banners=0)
    extensions = {'.md', '.java', '.h', '.hpp', '.cpp', '.c', '.py', '.cmake',
                  '.txt', '.gradle', '.json', '.yaml', '.yml', '.hlsl', '.hlsli'}
    code_extensions = {'.java', '.h', '.hpp', '.cpp', '.c', '.hlsl', '.hlsli'}
    doc_pattern = re.compile(r'(?<![/\w.-])doc/[A-Za-z0-9_./-]+\.md')
    link_pattern = re.compile(r'\[[^\]]*\]\(([^)]+)\)')
    for entry in ('AGENTS.md', 'README.md', 'doc/architecture/overview.md',
                  'doc/guide/documentation.md', '.agents/skills/raynium-doc/SKILL.md'):
        if entry not in known:
            errors.append(f'missing required entry: {entry}')

    for path in sorted(files):
        relative = path.relative_to(root)
        label = relative.as_posix()
        in_doc = relative.parts[0] == 'doc'
        history = in_doc and len(relative.parts) > 1 and relative.parts[1] == 'history'
        if in_doc and (len(relative.parts) < 3 or relative.parts[1] not in {'architecture', 'guide', 'history'}):
            errors.append(f'{label}: doc only contains architecture/, guide/ and history/')
        if history:
            dated = re.fullmatch(r'(\d{4})-(\d{2})-(\d{2})-[a-z0-9]+(?:-[a-z0-9]+)*\.[a-z0-9]+', path.name)
            if not dated:
                errors.append(f'{label}: expected YYYY-MM-DD-lowercase-topic.ext')
            else:
                year, month, day = map(int, dated.groups())
                if not (1 <= year <= 9999 and 1 <= month <= 12 and 1 <= day <= monthrange(year, month)[1]):
                    errors.append(f'{label}: invalid history date')
        if path.suffix.lower() not in extensions:
            continue
        counts['files'] += 1
        text = path.read_text(encoding='utf-8')
        if in_doc and path.suffix.lower() == '.md':
            counts['pages'] += 1
            head = '\n'.join(text.splitlines()[:8])
            for key in ('适用', '权威', '锚点'):
                if not re.search(rf'^>\s*-\s*{key}:\s*\S', head, re.MULTILINE):
                    errors.append(f'{label}: missing header {key}')
            if not history:
                anchor_lines = re.findall(r'^>\s*-\s*锚点:\s*(.+)$', head, re.MULTILINE)
                anchors = [token for line in anchor_lines for token in re.findall(r'`([^`]+)`', line)]
                if not anchors:
                    errors.append(f'{label}: at least one repository anchor is required')
                for token in anchors:
                    counts['anchors'] += 1
                    normalized = token.replace('\\', '/')
                    if (absolute_path(token) or '..' in PurePosixPath(normalized).parts
                            or not any(fnmatchcase(item, normalized) for item in known)):
                        errors.append(f'{label}: missing or non-portable anchor {token}')
        if history:
            continue

        references = doc_pattern.findall(text)
        counts['references'] += len(references)
        for target in references:
            if target not in known:
                errors.append(f'{label}: missing doc reference {target}')
        if path.suffix.lower() in code_extensions:
            counts['banners'] += len(references)
            if len(references) > 1:
                errors.append(f'{label}: keep at most one owning-document banner')
        if path.suffix.lower() != '.md':
            continue
        # Fenced examples are not live Markdown links.
        markdown = re.sub(r'^```[^\n]*\n.*?^```\s*$', '', text, flags=re.MULTILINE | re.DOTALL)
        for raw in link_pattern.findall(markdown):
            target = raw.strip()
            if target.startswith('<') and '>' in target:
                target = target[1:target.index('>')]
            else:
                target = target.split(' "', 1)[0].split(" '", 1)[0]
            if re.match(r'^[A-Za-z][A-Za-z0-9+.-]*:', target) and not absolute_path(target):
                continue
            if target.startswith('//'):
                continue
            target = unquote(target.split('#', 1)[0])
            if not target:
                continue
            counts['links'] += 1
            resolved = (path.parent / target).resolve()
            if absolute_path(target) or not resolved.is_relative_to(root):
                errors.append(f'{label}: non-portable link {target}')
                continue
            local = resolved.relative_to(root).as_posix()
            if local not in known and not any(item.startswith(local.rstrip('/') + '/') for item in known):
                errors.append(f'{label}: missing or ignored local link {target}')
    return errors, counts


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '--cached', '--others',
                             '--exclude-standard', '-z'], capture_output=True, check=False)
    if result.returncode:
        print('Cannot enumerate repository files with git.', file=sys.stderr)
        return 1
    names = set(result.stdout.decode('utf-8').split('\0')) - {''}
    files = [root / name for name in names
             if (root / name).is_file() and not (root / name).is_symlink()]
    errors, counts = check(root, files)
    print(', '.join(f'{key}={value}' for key, value in counts.items()))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print('Documentation checks passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
