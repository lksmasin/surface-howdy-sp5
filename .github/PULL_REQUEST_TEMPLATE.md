## Description

Please include a summary of the change and which issue is fixed. Please also include relevant motivation and context.

Fixes # (issue)

## Hardware Verification

Because this bridge interacts with PAM and runs privileged operations, please verify you have tested this on actual Surface hardware and you have a fallback login method configured.

- [ ] I have tested this code on a real Surface Pro 5 (or compatible).
- [ ] I maintained a fallback login path (e.g., password, another TTY) while testing to ensure I wasn't locked out.
- [ ] I verified that the IR LED still triggers correctly.

## Privilege & Security Checklist

- [ ] My code checks the return values of all syscalls and standard library functions.
- [ ] I have avoided introducing any shell-outs (e.g., `system()`, `popen()`).
- [ ] This change does not bypass or extend the 15-second self-timeout watchdog in `ir_bridge`.

## Type of change

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update
