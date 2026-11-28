#!/usr/bin/env python3
"""Rank catalog items as streaming candidates through the spoolet CLI.

The report is privacy-allowlisted: item IDs and titles, numeric bitrate, size
and dimensions, plus per-library counters. Provider payloads and their errors,
stream URLs and credentials never reach stdout or stderr.
"""

import argparse
import json
import math
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

PAGE_SIZE = 100
TIMEOUT_SECONDS = 30
READ_FAILURE = 'provider read failed'


class ReadFailure(Exception):
    """A catalog page could not be read; provider detail stays out of output."""


def bounded_int(low, high):
    def parse(text):
        value = int(text)
        if not low <= value <= high:
            raise argparse.ArgumentTypeError('expected {}..{}'.format(low, high))
        return value

    return parse


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError('expected a positive integer')
    return value


def positive_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value) or value <= 0:
        return None
    return value


def positive_dimension(value):
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        return None
    return value


def strongest_source(sources):
    if not isinstance(sources, list):
        return None
    best = None
    for source in sources:
        if not isinstance(source, dict):
            continue
        bitrate = positive_number(source.get('bitrate'))
        if bitrate is None:
            continue
        size = positive_number(source.get('sizeBytes')) or 0
        if best is None or (bitrate, size) > (best['bitrate'], best['sizeBytes']):
            best = {'bitrate': bitrate, 'sizeBytes': size,
                    'width': positive_dimension(source.get('width')),
                    'height': positive_dimension(source.get('height'))}
    return best


def fetch_page(spoolet, instance, library, offset):
    argv = [spoolet, '--instance', instance, 'catalog', library, str(offset), str(PAGE_SIZE)]
    try:
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError):
        raise ReadFailure()
    if completed.returncode != 0:
        raise ReadFailure()
    try:
        payload = json.loads(completed.stdout)
    except ValueError:
        raise ReadFailure()
    if not isinstance(payload, dict) or payload.get('ok') is not True:
        raise ReadFailure()
    result = payload.get('result')
    items = result.get('items') if isinstance(result, dict) else None
    if (not isinstance(items, list) or len(items) > PAGE_SIZE
            or result.get('offset') != offset):
        raise ReadFailure()
    return items, result


def scan_library(args, library):
    seen = set()
    candidates = []
    scanned = 0
    unknown_bitrate = 0

    def consume(items):
        nonlocal scanned, unknown_bitrate
        for item in items:
            if not isinstance(item, dict):
                continue
            item_id = item.get('id')
            if not isinstance(item_id, str) or item_id in seen:
                continue
            seen.add(item_id)
            scanned += 1
            best = strongest_source(item.get('sources'))
            if best is None:
                unknown_bitrate += 1
                continue
            if not item.get('playable'):
                continue
            title = item.get('title')
            candidates.append({'id': item_id, 'title': title if isinstance(title, str) else '',
                               'library': library, 'bitrate': best['bitrate'], 'sizeBytes': best['sizeBytes'],
                               'width': best['width'], 'height': best['height']})

    items, result = fetch_page(args.spoolet, args.instance, library, 0)
    total = result.get('total')
    if isinstance(total, bool) or not isinstance(total, int) or total < 0:
        total = None
    exhausted = result.get('exhausted') is True
    pages = 1
    inconsistent = total is not None and len(items) > total
    stride = len(items)
    consume(items)
    if not exhausted and result.get('seekable') is True and stride > 0:
        offset = stride
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            while not exhausted and pages < args.max_pages:
                batch = [offset + stride * step
                         for step in range(min(args.workers, args.max_pages - pages))
                         if total is None or offset + stride * step < total]
                if not batch:
                    break
                pending = [pool.submit(fetch_page, args.spoolet, args.instance, library, page_offset)
                           for page_offset in batch]
                # Observe every request, including failures after a terminal page.
                # They may reveal an unstable inventory or inconsistent paging.
                for page_offset, future in zip(batch, pending):
                    page_items, page_result = future.result()
                    pages += 1
                    page_total = page_result.get('total')
                    if total is not None and page_total != total:
                        inconsistent = True
                    terminal = page_result.get('exhausted') is True
                    if len(page_items) < stride and not terminal:
                        inconsistent = True
                    if terminal and total is not None and page_offset + len(page_items) < total:
                        inconsistent = True
                    consume(page_items)
                    exhausted |= terminal
                offset = batch[-1] + stride
    covered = total is not None and scanned == total
    summary = {'library': library, 'scanned': scanned, 'total': total, 'unknownBitrate': unknown_bitrate,
               'complete': exhausted and covered and not inconsistent and unknown_bitrate == 0}
    return candidates, summary


def ranking_key(entry):
    return (-entry['bitrate'], -entry['sizeBytes'], entry['id'])


def parse_args(argv):
    parser = argparse.ArgumentParser(description='Rank catalog items by stream bitrate.')
    parser.add_argument('--spoolet', required=True, help='spoolet executable to drive')
    parser.add_argument('--instance', required=True, help='account instance id')
    parser.add_argument('--library', action='append', required=True, help='library to scan; repeatable')
    parser.add_argument('--workers', type=bounded_int(1, 4), default=4)
    parser.add_argument('--max-pages', type=positive_int, default=1000)
    parser.add_argument('--top', type=positive_int, default=10)
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    report = {'candidates': [], 'libraries': []}
    try:
        for library in args.library:
            candidates, summary = scan_library(args, library)
            report['candidates'].extend(candidates)
            report['libraries'].append(summary)
    except ReadFailure:
        print(READ_FAILURE, file=sys.stderr)
        return 1
    report['candidates'].sort(key=ranking_key)
    report['candidates'] = report['candidates'][:args.top]
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    print()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
