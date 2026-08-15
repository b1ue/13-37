# Security policy

## Supported version

Security fixes are applied to the current `main` branch. Older firmware builds
and releases may not receive backports.

## Reporting a vulnerability

Use GitHub's **Report a vulnerability** option on the repository Security page.
This creates a private security advisory visible only to the reporter and the
repository owner. Do not disclose an unpatched vulnerability in a public pull
request, commit comment, or discussion.

Include the affected component, reproduction steps, impact, and a minimal proof
of concept. Do not include real credentials, private radio captures, location
history, or other users' data. You should receive an acknowledgement within
seven days.

The stock bridge is intended for a trusted LAN, VPN, or TLS-protected endpoint.
Reports that depend on exposing it directly to the public internet without the
documented controls may be treated as deployment-hardening requests.

Private stock strategies, model parameters, reports, and the real preset map
must live outside this public repository. Commit only the generic bridge and
display protocol. Leave `expose_job_details` disabled unless authenticated API
clients genuinely need private process output for diagnostics.
