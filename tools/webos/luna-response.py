#!/usr/bin/env python3
"""Validate JSON LS2 replies, including tty chatter and subscription updates."""
import json
import sys


def objects(text):
    decoder = json.JSONDecoder()
    offset = 0
    while offset < len(text):
        start = text.find("{", offset)
        if start < 0:
            return
        try:
            value, end = decoder.raw_decode(text, start)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        yield value
        offset = end


def contains_app(value, app_id):
    if isinstance(value, dict):
        return any(key in ("id", "appId") and item == app_id or contains_app(item, app_id)
                   for key, item in value.items())
    return isinstance(value, list) and any(contains_app(item, app_id) for item in value)


def main():
    mode = sys.argv[1]
    replies = list(objects(sys.stdin.read()))
    if any(reply.get("returnValue") is False or "errorText" in reply for reply in replies):
        return 1
    if mode == "install":
        return 0 if any(reply.get("finished") is True or reply.get("statusText") == "Finished."
                        or reply.get("state") == "installed" for reply in replies) else 2
    successful = [reply for reply in replies if reply.get("returnValue") is True]
    if mode == "registration":
        return 0 if any(contains_app(reply, sys.argv[2]) for reply in successful) else 1
    return 0 if successful else 1


if __name__ == "__main__":
    sys.exit(main())
