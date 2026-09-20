# Security policy

## Supported versions

Security fixes are applied to the latest stable major release.

## Reporting

Do not publish exploitable details in a public issue before maintainers have had a reasonable opportunity to assess them. Use the repository's private security advisory channel when available.

## Security posture

LiteMon is intentionally local-only. It exposes no network listener and uses no remote telemetry. The user service runs unprivileged with systemd hardening. External vendor utilities are invoked with fixed argument lists, bounded execution time and no shell interpolation.
