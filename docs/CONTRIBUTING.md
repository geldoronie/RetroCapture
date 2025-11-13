# Contributing Guide - RetroCapture

This document describes the rules and guidelines for contributing to the RetroCapture project, including commit standards, code structure, and contribution process.

## Commit Rules

### Commit Message Format

We follow the **Conventional Commits** standard to maintain a clean history and facilitate automatic changelog generation.

#### Basic Structure

```
<type>(<scope>): <short description>

<optional body>

<optional footer>
```

#### Commit Types

- **`feat`**: New feature
- **`fix`**: Bug fix
- **`docs`**: Documentation changes
- **`style`**: Formatting, missing semicolons, etc. (doesn't affect code)
- **`refactor`**: Code refactoring (no functionality change)
- **`perf`**: Performance improvement
- **`test`**: Adding or fixing tests
- **`build`**: Build system changes (CMake, scripts, etc.)
- **`ci`**: CI/CD changes
- **`chore`**: Maintenance tasks (cleanup, organization)

#### Scope (Optional)

The scope indicates the affected code area:

- `capture`: V4L2 capture module
- `renderer`: OpenGL rendering
- `shader`: Shader system
- `ui`: Graphical interface (ImGui)
- `window`: Window management
- `core`: Application core (Application)
- `build`: Build system
- `docs`: Documentation

#### Commit Examples

**Good:**
```
feat(shader): add support for custom parameters via UI

Allows editing shader parameters in real-time through the graphical interface.
Implements callbacks for value updates and saving modified presets.
```

```
fix(capture): fix freeze when changing resolution

The issue occurred because the V4L2 device wasn't completely closed
before reopening with new configuration. Now the device is explicitly
closed and reopened during reconfiguration.
```

```
docs(architecture): add architecture documentation

Creates ARCHITECTURE.md document with complete description of components,
data flow, and code structure to guide new contributors.
```

```
refactor(renderer): simplify aspect ratio logic

Extracts viewport calculation to separate function to improve readability
and facilitate future testing.
```

**Bad:**
```
fix: bug
```
```
update
```
```
WIP
```
```
fixes
```

### Important Rules

1. **First line must be maximum 72 characters**
   - Use the body for additional details

2. **Use imperative mood in the first line**
   - ✅ "add support for..."
   - ❌ "adding support for..." or "added support for..."

3. **Don't end the first line with a period**

4. **Separate body from first line with a blank line**

5. **Use body to explain "why", not "what"**
   - The "what" is already in the first line
   - Body should explain motivation, context, trade-offs

6. **One commit = one logical change**
   - Don't mix bug fixes with new features
   - Don't mix unrelated changes

### Detailed Examples

#### Feature with Multiple Changes

```
feat(ui): implement V4L2 device selection

- Add scanV4L2Devices() function to discover available devices
- Implement combo box for device selection in V4L2 tab
- Add Refresh button to rescan devices
- Implement setOnDeviceChanged callback to switch device in real-time
- Automatically reopen and reconfigure device when switching
- Reload V4L2 controls for new device
```

#### Bug Fix with Context

```
fix(shader): fix OutputSize type for shaders using vec4

Some RetroArch shaders use OutputSize as vec4 in constructors like
vec4(OutputSize, 1.0 / OutputSize). The system was forcing vec2 for
all cases, causing compilation errors.

Now automatically detects the required type based on usage in shader
source code and declares the uniform with the correct type.
```

#### Refactoring

```
refactor(core): use std::unique_ptr for memory management

Replaces raw pointers with std::unique_ptr to improve memory safety
and follow modern C++ best practices.

This also facilitates resource management and prevents memory leaks
in case of exceptions.
```

## Contribution Process

### 1. Before Committing

- ✅ Code compiles without errors
- ✅ Code follows existing style
- ✅ Changes tested locally
- ✅ Documentation updated (if necessary)
- ✅ Small, focused commits

### 2. Commit Structure

**Make frequent, small commits:**
```
feat(shader): add parsing of #pragma parameter
test(shader): add tests for parameter parsing
docs(shader): document #pragma parameter format
```

**Avoid large commits:**
```
feat: implement complete shader system
  (too large, difficult to review)
```

### 3. Commit Messages in English

**Prefer English** for this project, but be consistent:
- If you start in English, keep it in English
- If you start in Portuguese, keep it in Portuguese

**Exception**: Technical names, APIs, function names can remain in English:
```
fix(shader): fix OutputSize uniform parsing
```

### 4. WIP (Work In Progress) Commits

Avoid commits with "WIP" or "in progress". If necessary, use feature branches:
```bash
git checkout -b feature/new-feature
# ... work on feature ...
git commit -m "feat: implement feature X"
```

### 5. Merge Commits

When merging branches, use:
```
merge: integrate feature/new-feature
```

Or leave Git's default merge (no need for custom message).

## Pre-Commit Checklist

- [ ] Code compiles without warnings
- [ ] Tested locally
- [ ] Commit message follows the standard
- [ ] Commit is focused on one logical change
- [ ] Documentation updated (if necessary)
- [ ] No commented code or debug code
- [ ] No temporary files in commit

## Commit History Examples

### Good History

```
feat(shader): add support for multiple passes
fix(capture): fix memory leak when closing device
docs(architecture): add component documentation
refactor(renderer): simplify viewport calculation
feat(ui): implement V4L2 controls in interface
fix(shader): fix OutputSize type for vec4
test(capture): add tests for setFormat
```

### Bad History

```
fix
update
WIP
fixes bug
refactor
add stuff
```

## Useful Tools

### Git Hooks (Optional)

You can create a pre-commit hook to validate messages:

```bash
# .git/hooks/commit-msg
#!/bin/bash
commit_msg=$(cat "$1")

if ! echo "$commit_msg" | grep -qE "^(feat|fix|docs|style|refactor|perf|test|build|ci|chore)(\(.+\))?: .{1,72}$"; then
    echo "Error: Commit message doesn't follow Conventional Commits standard"
    exit 1
fi
```

### Useful Git Aliases

```bash
# Add to ~/.gitconfig
[alias]
    co = checkout
    br = branch
    ci = commit
    st = status
    lg = log --oneline --graph --decorate
```

## Quick Summary

1. **Format**: `<type>(<scope>): <description>`
2. **Types**: feat, fix, docs, style, refactor, perf, test, build, ci, chore
3. **First line**: maximum 72 characters, imperative, no period
4. **Body**: explain "why", not "what"
5. **One commit = one logical change**
6. **Prefer English** (but be consistent)

## Questions?

If you have questions about how to make a commit, consult:
- This document
- Existing commit history (`git log`)
- Project maintainers

---

**Remember**: Commits are forever. Write messages that make sense 6 months from now!
