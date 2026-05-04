# Contributing to CLMA

First off, thank you for considering contributing! We welcome all kinds of contributions — bug fixes, new features, documentation improvements, and more.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Pull Request Guidelines](#pull-request-guidelines)
- [Style Guides](#style-guides)
  - [Python Style](#python-style)
  - [C++ Style](#c-style)
  - [Commit Messages](#commit-messages)
- [Testing](#testing)
- [Questions?](#questions)

## Code of Conduct

This project adheres to a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

## Getting Started

1. **Fork the repository** and clone your fork:
   ```bash
   git clone https://github.com/your-username/CLMA.git
   cd CLMA
   ```

2. **Set up the development environment**:
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   pip install flask flask-cors pybind11 pytest
   ```

3. **Build the C++ core**:
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Debug
   make -j$(nproc)
   cd ..
   ```

4. **Run the tests** to make sure everything works:
   ```bash
   source venv/bin/activate
   cd tests && python -m pytest -v
   ```

## Development Workflow

1. **Create a branch**: `git checkout -b feat/your-feature-name`
2. **Make your changes** — keep them focused on one concern per PR.
3. **Add or update tests** — we maintain >100 test cases and aim to keep coverage high.
4. **Run the full test suite** before committing.
5. **Commit** following our commit message convention (see below).
6. **Push** to your fork and open a Pull Request.

## Pull Request Guidelines

- **One PR = one concern.** A PR should address a single bug, feature, or documentation improvement.
- **Include tests.** New features should come with test coverage. Bug fixes should add a test that catches the regression.
- **Keep the diff small.** If your change touches 20+ files, consider splitting it into smaller PRs.
- **Update documentation** if your change affects public APIs, configuration, or the Web UI.
- **Write a clear PR description** explaining *what* changed and *why*.

### PR Title Convention

```
<type>(<scope>): <short description>
```

Examples:
- `feat(aan): add true tree topology with recursive decomposition`
- `fix(web-ui): resolve sidebar delay on SSE stream completion`
- `docs(readme): update benchmark table for AAN mode`
- `test(aan): add 28 test cases for router and executors`

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `ci`, `chore`

## Style Guides

### Python Style

- Follow [PEP 8](https://peps.python.org/pep-0008/).
- Use type hints for all function signatures.
- 4-space indentation.
- Use `snake_case` for functions and variables, `PascalCase` for classes.
- Chinese comments in UI-related code are acceptable for consistency.

### C++ Style

- Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
- Use 2-space indentation (project convention).
- `PascalCase` for classes, `snake_case` for functions and variables.
- Prefer `constexpr` over `#define` for constants.
- Use `//` for comments, `///` for documentation comments.

### Commit Messages

```
<type>(<scope>): <imperative, present tense summary>

[optional body: explain the motivation and context]
```

Good examples:
- `feat(aan): implement true tree topology with recursive decomposition`
- `fix(web-ui): flush SSE done event before closing connection`
- `docs(readme): update quick-start with AAN configuration`

Bad examples:
- `fix bug`
- `update stuff`
- `changes`

## Testing

We maintain test suites in two languages:

**Python tests** (core framework + integration):
```bash
source venv/bin/activate
cd tests && python -m pytest -v
```

**C++ tests** (core engine):
```bash
cd build && ctest -j$(nproc)
```

**When adding a new feature:**
1. Add unit tests for the new logic.
2. Update the test count in `README.md` if you add new test cases.
3. Run the full suite and ensure no regressions.

## Questions?

- Open a [Discussion](https://github.com/kriely/CLMA/discussions)
- File an [Issue](https://github.com/kriely/CLMA/issues)
- Star the repo and follow the project for updates!
