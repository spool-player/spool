# Security Policy

## Supported versions

Spool is prerelease software. Security fixes are provided only for the latest published `v0.x` prerelease and the current `master` branch. Older prereleases are unsupported.

## Reporting a vulnerability

Do not open a public issue and do not attach tokens, credentials, raw logs, server URLs, media names, account names, or private network details.

Use GitHub private vulnerability reporting for this repository. Include the affected version, platform, impact, minimal reproduction steps, and any proposed mitigation. Use synthetic data wherever possible.

We aim to acknowledge a report within 7 days, provide an initial assessment within 14 days, and coordinate a remediation and disclosure date with the reporter. These are response targets, not a service-level guarantee.

## Disclosure

Please allow a reasonable remediation window before public disclosure. Once a fix is available, the project will publish a security advisory describing affected versions, impact, mitigations, and fixed versions without exposing private user data.

## Transport and certificate trust

HTTPS is the default and recommended server transport. Certificate validation is never disabled globally. A certificate exception is scoped to the exact server origin and certificate fingerprint that the user confirms. Remembered certificates can be reviewed and removed from **Settings → Network → Remembered certificates**. Removing an entry restores normal platform trust validation on the next connection.

HTTP may be used only for an explicitly approved local address. Credentials are denied for public HTTP destinations and are not forwarded across an origin-changing redirect.
